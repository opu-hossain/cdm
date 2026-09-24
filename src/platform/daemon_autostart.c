// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "daemon_autostart.h"
#include "file_io.h"
#include "ipc_socket.h"
#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENTRY_NAME "cdm-daemon.desktop"
#define MAX_ENTRY_SIZE 65536

static int append(char *out, size_t capacity, size_t *used, const char *data,
                  size_t length) {
  if (length >= capacity - *used)
    return -1;
  memcpy(out + *used, data, length);
  *used += length;
  out[*used] = '\0';
  return 0;
}

static int path_join(char *out, size_t size, const char *base,
                     const char *suffix) {
  int n = snprintf(out, size, "%s/%s", base, suffix);
  return n > 0 && (size_t)n < size ? 0 : -1;
}

static int user_entry_path(char *out, size_t size) {
  const char *config = getenv("XDG_CONFIG_HOME");
  char fallback[PATH_MAX];
  if (!config || config[0] != '/') {
    const char *home = getenv("HOME");
    if (!home || home[0] != '/' ||
        path_join(fallback, sizeof(fallback), home, ".config") != 0)
      return -1;
    config = fallback;
  }
  char autostart[PATH_MAX];
  if (path_join(autostart, sizeof(autostart), config, "autostart") != 0)
    return -1;
  return path_join(out, size, autostart, ENTRY_NAME);
}

static int system_entry_path(char *out, size_t size) {
  const char *dirs = getenv("XDG_CONFIG_DIRS");
  if (!dirs || !*dirs)
    dirs = "/etc/xdg";
  const char *part = dirs;
  while (*part) {
    const char *end = strchr(part, ':');
    size_t length = end ? (size_t)(end - part) : strlen(part);
    if (length > 0 && part[0] == '/' && length < PATH_MAX) {
      char base[PATH_MAX], dir[PATH_MAX], candidate[PATH_MAX];
      memcpy(base, part, length);
      base[length] = '\0';
      if (path_join(dir, sizeof(dir), base, "autostart") == 0 &&
          path_join(candidate, sizeof(candidate), dir, ENTRY_NAME) == 0 &&
          access(candidate, F_OK) == 0) {
        if (snprintf(out, size, "%s", candidate) >= (int)size)
          return -1;
        return 0;
      }
    }
    if (!end)
      break;
    part = end + 1;
  }
  return 1;
}

static int read_entry(const char *path, bool user_file, char **content) {
  *content = NULL;
  struct stat info;
  if (lstat(path, &info) != 0)
    return errno == ENOENT ? 1 : -1;
  if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
      info.st_size > MAX_ENTRY_SIZE ||
      (user_file && info.st_uid != getuid()))
    return -1;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return -1;
  char *buffer = calloc((size_t)info.st_size + 2, 1);
  if (!buffer) {
    close(fd);
    return -1;
  }
  size_t used = 0;
  while (used < (size_t)info.st_size) {
    ssize_t count = read(fd, buffer + used, (size_t)info.st_size - used);
    if (count <= 0) {
      free(buffer);
      close(fd);
      return -1;
    }
    used += (size_t)count;
  }
  close(fd);
  *content = buffer;
  return 0;
}

static bool key_value(const char *content, const char *key, char *value,
                      size_t size) {
  bool section = false, found = false;
  size_t key_len = strlen(key);
  const char *line = content;
  while (*line) {
    const char *end = strchr(line, '\n');
    if (!end)
      end = line + strlen(line);
    size_t len = (size_t)(end - line);
    if (len >= 2 && line[0] == '[')
      section = len == strlen("[Desktop Entry]") &&
                strncmp(line, "[Desktop Entry]", len) == 0;
    else if (section && len > key_len &&
             strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
      size_t n = len - key_len - 1;
      if (n >= size)
        n = size - 1;
      memcpy(value, line + key_len + 1, n);
      value[n] = '\0';
      found = true;
    }
    line = *end ? end + 1 : end;
  }
  return found;
}

static bool executable(const char *value) {
  if (!*value)
    return false;
  if (strchr(value, '/'))
    return access(value, X_OK) == 0;
  const char *path = getenv("PATH");
  if (!path)
    return false;
  char *copy = strdup(path);
  if (!copy)
    return false;
  bool found = false;
  char *save = NULL;
  for (char *dir = strtok_r(copy, ":", &save); dir;
       dir = strtok_r(NULL, ":", &save)) {
    char candidate[PATH_MAX];
    if (path_join(candidate, sizeof(candidate), dir, value) == 0 &&
        access(candidate, X_OK) == 0) {
      found = true;
      break;
    }
  }
  free(copy);
  return found;
}

static bool entry_enabled(const char *content) {
  char hidden[16] = {0}, type[32] = {0};
  char exec[PATH_MAX] = {0}, try_exec[PATH_MAX] = {0};
  if (!key_value(content, "Type", type, sizeof(type)) ||
      strcmp(type, "Application") != 0 ||
      !key_value(content, "Exec", exec, sizeof(exec)) || !exec[0])
    return false;
  if (key_value(content, "Hidden", hidden, sizeof(hidden)) &&
      strcmp(hidden, "true") == 0)
    return false;
  return !key_value(content, "TryExec", try_exec, sizeof(try_exec)) ||
         executable(try_exec);
}

/* 1 when the first system entry is runnable, 0 otherwise, -1 on error. */
static int system_entry_enabled(void) {
  char path[PATH_MAX];
  int found = system_entry_path(path, sizeof(path));
  if (found != 0)
    return found < 0 ? -1 : 0;
  char *content = NULL;
  if (read_entry(path, false, &content) != 0)
    return -1;
  int enabled = entry_enabled(content) ? 1 : 0;
  free(content);
  return enabled;
}

static int ensure_user_dir(const char *entry) {
  char dir[PATH_MAX];
  if (snprintf(dir, sizeof(dir), "%s", entry) >= (int)sizeof(dir))
    return -1;
  char *slash = strrchr(dir, '/');
  if (!slash)
    return -1;
  *slash = '\0';
  struct stat info;
  if (lstat(dir, &info) != 0) {
    if (errno != ENOENT || file_ensure_directory(dir) != 0)
      return -1;
    if (lstat(dir, &info) != 0)
      return -1;
  }
  return S_ISDIR(info.st_mode) && info.st_uid == getuid() ? 0 : -1;
}

static int write_entry(const char *path, const char *content) {
  if (ensure_user_dir(path) != 0)
    return -1;
  char *existing = NULL;
  int state = read_entry(path, true, &existing);
  free(existing);
  if (state < 0)
    return -1;
  char temporary[PATH_MAX];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >=
      (int)sizeof(temporary))
    return -1;
  int fd = mkstemp(temporary);
  if (fd < 0)
    return -1;
  size_t length = strlen(content), offset = 0;
  int result = 0;
  if (fchmod(fd, 0644) != 0)
    result = -1;
  while (result == 0 && offset < length) {
    ssize_t n = write(fd, content + offset, length - offset);
    if (n <= 0)
      result = -1;
    else
      offset += (size_t)n;
  }
  if (result == 0 && fsync(fd) != 0)
    result = -1;
  if (close(fd) != 0)
    result = -1;
  if (result == 0 && rename(temporary, path) != 0)
    result = -1;
  if (result != 0)
    unlink(temporary);
  return result;
}

static int toggle_hidden(const char *source, bool hidden, char *out,
                         size_t capacity) {
  bool section = false, has_section = false, changed = false;
  bool extras_added = false;
  char type[32] = {0}, name[256] = {0}, command[PATH_MAX] = {0};
  bool need_type = !hidden && !key_value(source, "Type", type, sizeof(type));
  bool need_name = !hidden && !key_value(source, "Name", name, sizeof(name));
  bool need_exec = !hidden && !key_value(source, "Exec", command, sizeof(command));
  char exe[PATH_MAX] = {0};
  if (need_exec) {
    get_self_exe_path(exe, sizeof(exe));
    if (exe[0] != '/')
      return -1;
  }
  size_t used = 0;
  const char *line = source;
  while (*line) {
    const char *end = strchr(line, '\n');
    if (!end)
      end = line + strlen(line);
    size_t len = (size_t)(end - line);
    bool heading = len > 0 && line[0] == '[';
    if (heading && section && !changed) {
      const char *key = hidden ? "Hidden=true\n" : "Hidden=false\n";
      if (append(out, capacity, &used, key, strlen(key)) != 0)
        return -1;
      changed = true;
    }
    if (heading && section && !extras_added) {
      char extra[PATH_MAX + 32];
      int n = 0;
      if (need_type && append(out, capacity, &used, "Type=Application\n", 17) != 0)
        return -1;
      if (need_name && append(out, capacity, &used,
                              "Name=Core Download Manager Daemon\n", 34) != 0)
        return -1;
      if (need_exec) {
        n = snprintf(extra, sizeof(extra), "Exec=%s daemon\n", exe);
        if (n < 0 || (size_t)n >= sizeof(extra) ||
            append(out, capacity, &used, extra, (size_t)n) != 0)
          return -1;
      }
      extras_added = true;
    }
    if (heading) {
      section = len == strlen("[Desktop Entry]") &&
                strncmp(line, "[Desktop Entry]", len) == 0;
      has_section |= section;
    }
    if (section && len >= 7 && strncmp(line, "Hidden=", 7) == 0) {
      if (!changed) {
        const char *key = hidden ? "Hidden=true\n" : "Hidden=false\n";
        if (append(out, capacity, &used, key, strlen(key)) != 0)
          return -1;
        changed = true;
      }
    } else {
      if (append(out, capacity, &used, line, len) != 0 ||
          append(out, capacity, &used, "\n", 1) != 0)
        return -1;
    }
    line = *end ? end + 1 : end;
  }
  if (!has_section)
    return -1;
  if (!changed) {
    const char *key = hidden ? "Hidden=true\n" : "Hidden=false\n";
    if (append(out, capacity, &used, key, strlen(key)) != 0)
      return -1;
  }
  if (!extras_added) {
    if (need_type && append(out, capacity, &used, "Type=Application\n", 17) != 0)
      return -1;
    if (need_name && append(out, capacity, &used,
                            "Name=Core Download Manager Daemon\n", 34) != 0)
      return -1;
    if (need_exec) {
      char extra[PATH_MAX + 32];
      int n = snprintf(extra, sizeof(extra), "Exec=%s daemon\n", exe);
      if (n < 0 || (size_t)n >= sizeof(extra) ||
          append(out, capacity, &used, extra, (size_t)n) != 0)
        return -1;
    }
  }
  return 0;
}

typedef enum {
  AUTOSTART_WROTE,
  AUTOSTART_UPDATED,
  AUTOSTART_REMOVED,
  AUTOSTART_USING_SYSTEM
} AutostartAction;

typedef struct {
  AutostartAction action;
  char path[PATH_MAX];
} AutostartChange;

static int set_enabled(bool enabled, AutostartChange *change) {
  char path[PATH_MAX];
  if (user_entry_path(path, sizeof(path)) != 0)
    return -1;
  snprintf(change->path, sizeof(change->path), "%s", path);
  char *current = NULL;
  int state = read_entry(path, true, &current);
  if (state < 0)
    return -1;
  bool had_entry = state == 0;
  if (state == 0) {
    char marker[16] = {0};
    if (enabled && key_value(current, "X-CDM-Generated", marker,
                             sizeof(marker)) && strcmp(marker, "true") == 0) {
      free(current);
      int system_state = system_entry_enabled();
      if (system_state < 0)
        return -1;
      if (system_state == 1) {
        if (unlink(path) != 0)
          return -1;
        change->action = AUTOSTART_REMOVED;
        return 0;
      }
      current = NULL;
      state = 1;
    }
    if (state == 0) {
      char updated[MAX_ENTRY_SIZE + 128] = {0};
      int result = toggle_hidden(current, !enabled, updated, sizeof(updated));
      free(current);
      if (result != 0 || write_entry(path, updated) != 0)
        return -1;
      change->action = AUTOSTART_UPDATED;
      return 0;
    }
  }
  if (!enabled) {
    if (write_entry(path, "[Desktop Entry]\nType=Application\n"
                          "Name=Core Download Manager Daemon\n"
                          "Hidden=true\nX-CDM-Generated=true\n") != 0)
      return -1;
    change->action = had_entry ? AUTOSTART_UPDATED : AUTOSTART_WROTE;
    return 0;
  }
  int system_state = system_entry_enabled();
  if (system_state < 0)
    return -1;
  if (state == 1 && system_state == 1) {
    if (system_entry_path(change->path, sizeof(change->path)) != 0)
      return -1;
    change->action = AUTOSTART_USING_SYSTEM;
    return 0;
  }
  char exe[PATH_MAX] = {0};
  get_self_exe_path(exe, sizeof(exe));
  if (exe[0] != '/')
    return -1;
  char entry[PATH_MAX * 2];
  int length = snprintf(entry, sizeof(entry),
                        "[Desktop Entry]\nType=Application\n"
                        "Name=Core Download Manager Daemon\n"
                        "Exec=%s daemon\nTryExec=%s\nTerminal=false\n"
                        "NoDisplay=true\nHidden=false\nX-CDM-Generated=true\n",
                        exe, exe);
  if (length <= 0 || (size_t)length >= sizeof(entry) ||
      write_entry(path, entry) != 0)
    return -1;
  change->action = had_entry ? AUTOSTART_UPDATED : AUTOSTART_WROTE;
  return 0;
}

static int show_status(void) {
  char path[PATH_MAX];
  if (user_entry_path(path, sizeof(path)) != 0)
    return -1;
  char *content = NULL;
  int state = read_entry(path, true, &content);
  if (state < 0)
    return -1;
  if (state == 1) {
    int found = system_entry_path(path, sizeof(path));
    if (found < 0)
      return -1;
    if (found == 0 && read_entry(path, false, &content) != 0)
      return -1;
    if (found == 1)
      path[0] = '\0';
  }
  pid_t daemon_pid = 0;
  int daemon_state = ipc_server_get_pid(&daemon_pid);
  if (daemon_state < 0) {
    free(content);
    fprintf(stderr, "cdm daemon status: could not determine daemon PID\n");
    return -2;
  }
  const char *autostart = "unavailable";
  if (content) {
    autostart = entry_enabled(content) ? "enabled" : "disabled";
  }
  printf("Autostart: %s\n", autostart);
  printf("Entry: %s\n", path[0] ? path : "(none)");
  if (daemon_state == 1)
    printf("Daemon: running (pid %ld)\n", (long)daemon_pid);
  else
    printf("Daemon: not running\n");
  free(content);
  return 0;
}

int daemon_autostart_command(const char *action) {
  int result = -1;
  AutostartChange change = {0};
  if (strcmp(action, "enable") == 0)
    result = set_enabled(true, &change);
  else if (strcmp(action, "disable") == 0)
    result = set_enabled(false, &change);
  else if (strcmp(action, "status") == 0)
    result = show_status();
  if (result == 0 && strcmp(action, "status") != 0) {
    const char *state = strcmp(action, "enable") == 0 ? "enabled" : "disabled";
    if (change.action == AUTOSTART_USING_SYSTEM)
      printf("Autostart: %s (using %s; no file changed)\n", state,
             change.path);
    else {
      const char *verb = change.action == AUTOSTART_REMOVED ? "removed" :
                         change.action == AUTOSTART_UPDATED ? "updated" : "wrote";
      printf("Autostart: %s (%s %s)\n", state, verb, change.path);
    }
  } else if (result == -1) {
    if (change.path[0])
      fprintf(stderr, "cdm daemon %s: could not update autostart at %s\n",
              action, change.path);
    else
      fprintf(stderr, "cdm daemon %s: could not inspect autostart\n", action);
  }
  return result == 0 ? 0 : 1;
}
