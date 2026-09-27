// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "spawn.h"

#include "../utils/log.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>

void get_self_exe_path(char *buf, size_t buf_size) {
  DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)buf_size);
  if (len == 0 || len == buf_size)
    buf[0] = '\0';
}

int spawn_daemon_detached(const char *exe_path) {
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  memset(&pi, 0, sizeof(pi));

  char cmdline[1024];
  snprintf(cmdline, sizeof(cmdline), "\"%s\" daemon", exe_path);

  BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                           DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL,
                           NULL, &si, &pi);
  if (!ok) {
    fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
    return -1;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return 0;
}

int spawn_browser_popup_detached(const char *exe_path, unsigned int offer_id) {
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  memset(&pi, 0, sizeof(pi));
  char cmdline[1200];
  snprintf(cmdline, sizeof(cmdline), "\"%s\" browser-popup --offer %u",
           exe_path, offer_id);
  BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                           DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                           NULL, NULL, &si, &pi);
  if (!ok)
    return -1;
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return 0;
}

void spawn_media_tools_init(void) {}
bool spawn_ffmpeg_available(void) { return false; }
int spawn_ffmpeg_remux(const char *input, const char *output,
                       const _Atomic bool *cancel, const _Atomic bool *pause,
                       int timeout_sec) {
  (void)input; (void)output; (void)cancel; (void)pause; (void)timeout_sec;
  /* TODO(platform): implement argv-safe ffmpeg spawning and cancellation. */
  return -1;
}

int spawn_ffmpeg_merge(const char *video, const char *audio, const char *output,
                       const _Atomic bool *cancel, const _Atomic bool *pause, int timeout_sec) {
 (void)video;(void)audio;(void)output;(void)cancel;(void)pause;(void)timeout_sec;return -1;
}

int spawn_post_action(const char *action, const char *argument) {
  (void)action;
  (void)argument;
  /* TODO(platform): use a safely quoted CreateProcess argv on Windows. */
  return -1;
}

#else /* Linux / macOS */

#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
void get_self_exe_path(char *buf, size_t buf_size) {
  uint32_t size = (uint32_t)buf_size;
  if (_NSGetExecutablePath(buf, &size) != 0)
    buf[0] = '\0';
}
#else
void get_self_exe_path(char *buf, size_t buf_size) {
  ssize_t len = readlink("/proc/self/exe", buf, buf_size - 1);
  if (len == -1)
    buf[0] = '\0';
  else
    buf[len] = '\0';
}
#endif

static int split_command(char *text, char *argv[32]) {
  char *read = text;
  char *write = text;
  int count = 0;
  while (*read) {
    while (*read == ' ' || *read == '\t')
      read++;
    if (!*read)
      break;
    if (count >= 31)
      return -1;
    argv[count++] = write;
    char quote = 0;
    while (*read) {
      if (*read == '\\' && read[1]) {
        read++;
        *write++ = *read++;
      } else if (quote && *read == quote) {
        quote = 0;
        read++;
      } else if (!quote && (*read == '\'' || *read == '"')) {
        quote = *read++;
      } else if (!quote && (*read == ' ' || *read == '\t')) {
        read++;
        break;
      } else {
        *write++ = *read++;
      }
    }
    if (quote)
      return -1;
    *write++ = '\0';
  }
  argv[count] = NULL;
  return count;
}

int spawn_post_action(const char *action, const char *argument) {
  if (!action)
    return -1;
#ifdef __APPLE__
  /* TODO(platform): map power actions to the macOS power manager. */
  if (strcmp(action, "shutdown") == 0 || strcmp(action, "sleep") == 0)
    return -1;
#endif
  char command[512];
  char *argv[32] = {0};
  if (strcmp(action, "shutdown") == 0) {
    argv[0] = "systemctl";
    argv[1] = "poweroff";
  } else if (strcmp(action, "sleep") == 0) {
    argv[0] = "systemctl";
    argv[1] = "suspend";
  } else if (strcmp(action, "command") == 0 && argument &&
             strlen(argument) < sizeof(command)) {
    memcpy(command, argument, strlen(argument) + 1);
    if (split_command(command, argv) <= 0)
      return -1;
  } else {
    return -1;
  }
  int error_pipe[2];
  if (pipe(error_pipe) != 0)
    return -1;
  if (fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
    close(error_pipe[0]);
    close(error_pipe[1]);
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(error_pipe[0]);
    close(error_pipe[1]);
    return -1;
  }
  if (pid == 0) {
    close(error_pipe[0]);
    if (setsid() < 0)
      _exit(127);
    pid_t child = fork();
    if (child < 0)
      _exit(127);
    if (child == 0) {
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
          close(devnull);
      }
      execvp(argv[0], argv);
      char marker = 'E';
      (void)write(error_pipe[1], &marker, 1);
      _exit(127);
    }
    _exit(0);
  }
  close(error_pipe[1]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    close(error_pipe[0]);
    return -1;
  }
  char marker = 0;
  ssize_t result;
  do {
    result = read(error_pipe[0], &marker, 1);
  } while (result < 0 && errno == EINTR);
  close(error_pipe[0]);
  return result == 0 ? 0 : -1;
}

int spawn_daemon_detached(const char *exe_path) {
  pid_t pid = fork();
  if (pid < 0) {
    LOG_ERROR("first fork failed for '%s'", exe_path);
    return -1;
  }

  if (pid == 0) {
    setsid();
    pid_t pid2 = fork();
    if (pid2 == 0) {
      execl(exe_path, exe_path, "daemon", (char *)NULL);
      LOG_ERROR("execl failed for '%s'", exe_path);
      _exit(127);
    }
    _exit(0);
  }
  waitpid(pid, NULL, 0);
  LOG_INFO("launched '%s' as daemon", exe_path);
  return 0;
}

int spawn_browser_popup_detached(const char *exe_path, unsigned int offer_id) {
  int ready_pipe[2];
  if (pipe(ready_pipe) != 0)
    return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    return -1;
  }
  if (pid == 0) {
    close(ready_pipe[0]);
    if (setsid() < 0)
      _exit(127);
    pid_t child = fork();
    if (child < 0)
      _exit(127);
    if (child == 0) {
      char ready_fd[16];
      snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
      setenv("CDM_BROWSER_READY_FD", ready_fd, 1);
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
          close(devnull);
      }
      char id[16];
      snprintf(id, sizeof(id), "%u", offer_id);
      execl(exe_path, exe_path, "browser-popup", "--offer", id,
            (char *)NULL);
      _exit(127);
    }
    close(ready_pipe[1]);
    _exit(0);
  }
  close(ready_pipe[1]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    close(ready_pipe[0]);
    return -1;
  }
  struct pollfd pollfd = {.fd = ready_pipe[0], .events = POLLIN};
  int ready;
  do {
    ready = poll(&pollfd, 1, 5000);
  } while (ready < 0 && errno == EINTR);
  char marker = 0;
  if (ready > 0 && (pollfd.revents & POLLIN))
    ready = (int)read(ready_pipe[0], &marker, 1);
  close(ready_pipe[0]);
  return ready == 1 && marker == 'R' ? 0 : -1;
}

/* Presence is immutable after call_once; no mutable path lookup during jobs. */
#include <spawn.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <threads.h>
#include <limits.h>
#include <stdint.h>
#include "thread.h"
extern char **environ;
static once_flag ffmpeg_once = ONCE_FLAG_INIT;
static char ffmpeg_executable[1024];

static void locate_ffmpeg(void) {
  const char *path = getenv("PATH");
  if (!path) return;
  for (const char *start = path;;) {
    const char *end = strchr(start, ':');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    char candidate[1024], resolved[PATH_MAX];
    int n = -1;
    if (length < sizeof(candidate))
      n = length ? snprintf(candidate, sizeof(candidate), "%.*s/ffmpeg", (int)length, start)
                 : snprintf(candidate, sizeof(candidate), "./ffmpeg");
    struct stat st;
    if (length < sizeof(candidate) && n >= 0 && (size_t)n < sizeof(candidate) &&
        realpath(candidate, resolved) && strlen(resolved) < sizeof(ffmpeg_executable) &&
        stat(resolved, &st) == 0 && S_ISREG(st.st_mode) && access(resolved, X_OK) == 0) {
      strcpy(ffmpeg_executable, resolved); return;
    }
    if (!end) break;
    start = end + 1;
  }
}

void spawn_media_tools_init(void) { call_once(&ffmpeg_once, locate_ffmpeg); }
bool spawn_ffmpeg_available(void) {
  spawn_media_tools_init(); return ffmpeg_executable[0] != '\0';
}

static bool remux_interrupted(const _Atomic bool *cancel, const _Atomic bool *pause) {
  return (cancel && atomic_load(cancel)) || (pause && atomic_load(pause));
}

static uint64_t monotonic_ms(void) {
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0;
  return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static int run_ffmpeg(const char *input, const char *audio, const char *output,
                       const _Atomic bool *cancel, const _Atomic bool *pause,
                       int timeout_sec) {
  if (!input || !output || timeout_sec < 1 || !spawn_ffmpeg_available()) return -1;
  if (remux_interrupted(cancel, pause)) return -2;
  char *argv[] = {ffmpeg_executable, "-nostdin", "-hide_banner", "-v", "error", "-y",
      "-protocol_whitelist", "file,pipe", "-format_whitelist", "mpegts,mov,aac,mp3",
      "-i", (char *)input, "-c", "copy", "-bsf:a", "aac_adtstoasc", "-f", "mp4",
      (char *)output, NULL};
  char *merge_argv[] = {ffmpeg_executable, "-nostdin", "-hide_banner", "-v", "error", "-y",
      "-protocol_whitelist", "file,pipe", "-format_whitelist", "mpegts,mov,aac,mp3",
      "-i", (char *)input, "-protocol_whitelist", "file,pipe", "-format_whitelist", "mpegts,mov,aac,mp3",
      "-i", (char *)audio, "-map", "0:v:0", "-map", "1:a:0", "-c", "copy", "-f", "mp4", (char *)output, NULL};
  char **chosen_argv = audio ? merge_argv : argv;
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) return -1;
  int error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (!error) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t pid = -1;
  if (!error) error = posix_spawn(&pid, ffmpeg_executable, &actions, NULL, chosen_argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (error) return -1;
  uint64_t start = monotonic_ms();
  int status = 0, result = 0;
  for (;;) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) break;
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) { result = -1; break; }
    bool interrupted = remux_interrupted(cancel, pause);
    if (interrupted || !start || monotonic_ms() - start >= (uint64_t)timeout_sec * 1000) {
      result = interrupted ? -2 : 124;
      kill(pid, SIGTERM);
      for (int i = 0; i < 5; i++) {
        dm_thread_sleep_ms(100);
        got = waitpid(pid, &status, WNOHANG);
        if (got == pid) return result;
      }
      kill(pid, SIGKILL);
      do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
      return result;
    }
    dm_thread_sleep_ms(100);
  }
  if (result) return result;
  return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

int spawn_ffmpeg_remux(const char *input, const char *output,
                       const _Atomic bool *cancel, const _Atomic bool *pause, int timeout_sec) {
  return run_ffmpeg(input, NULL, output, cancel, pause, timeout_sec);
}
int spawn_ffmpeg_merge(const char *video, const char *audio, const char *output,
                       const _Atomic bool *cancel, const _Atomic bool *pause, int timeout_sec) {
  if (!audio || !*audio) return -1;
  return run_ffmpeg(video, audio, output, cancel, pause, timeout_sec);
}

#endif
