// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "browser_install.h"

#include "../platform/file_io.h"
#include "../platform/spawn.h"
#include "../vendor/cJSON.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOST_NAME "org.cdm.browser"

static const char *manifest_dir(const char *browser) {
  if (strcmp(browser, "--chrome") == 0)
    return "/.config/google-chrome/NativeMessagingHosts";
  if (strcmp(browser, "--chromium") == 0)
    return "/.config/chromium/NativeMessagingHosts";
  if (strcmp(browser, "--firefox") == 0)
    return "/.mozilla/native-messaging-hosts";
  return NULL;
}

static bool valid_chromium_id(const char *id) {
  if (!id || strlen(id) != 32)
    return false;
  for (const char *p = id; *p; p++)
    if (*p < 'a' || *p > 'p')
      return false;
  return true;
}

static bool get_manifest_path(const char *browser, char *out, size_t capacity,
                              bool create_directory) {
  const char *suffix = manifest_dir(browser);
  const char *home = getenv("HOME");
  if (!suffix || !home || home[0] != '/')
    return false;
  char dir[1024];
  int length = snprintf(dir, sizeof(dir), "%s%s", home, suffix);
  if (length < 0 || (size_t)length >= sizeof(dir))
    return false;
  if (create_directory && file_ensure_directory(dir) != 0)
    return false;
  length = snprintf(out, capacity, "%s/%s.json", dir, HOST_NAME);
  return length >= 0 && (size_t)length < capacity;
}

static bool get_host_path(char *out, size_t capacity) {
  get_self_exe_path(out, capacity);
  char *base = strrchr(out, '/');
  if (!base || strcmp(base + 1, "cdm") != 0)
    return false;
  if ((size_t)(base + 1 - out) + sizeof("cdm_native_host") > capacity)
    return false;
  strcpy(base + 1, "cdm_native_host");
  return access(out, X_OK) == 0;
}

static int write_manifest(const char *path, const char *host_path,
                          const char *browser, const char *id) {
  cJSON *root = cJSON_CreateObject();
  cJSON *allowed = cJSON_CreateArray();
  if (!root || !allowed) {
    cJSON_Delete(root);
    cJSON_Delete(allowed);
    return -1;
  }
  cJSON_AddStringToObject(root, "name", HOST_NAME);
  cJSON_AddStringToObject(root, "description", "Core Download Manager browser host");
  cJSON_AddStringToObject(root, "path", host_path);
  cJSON_AddStringToObject(root, "type", "stdio");
  if (strcmp(browser, "--firefox") == 0) {
    cJSON_AddItemToArray(allowed, cJSON_CreateString(id));
    cJSON_AddItemToObject(root, "allowed_extensions", allowed);
  } else {
    char origin[80];
    snprintf(origin, sizeof(origin), "chrome-extension://%s/", id);
    cJSON_AddItemToArray(allowed, cJSON_CreateString(origin));
    cJSON_AddItemToObject(root, "allowed_origins", allowed);
  }
  char *json = cJSON_Print(root);
  cJSON_Delete(root);
  if (!json)
    return -1;
  char temporary[1200];
  int length = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
  if (length < 0 || (size_t)length >= sizeof(temporary)) {
    cJSON_free(json);
    return -1;
  }
  int fd = mkstemp(temporary);
  if (fd < 0) {
    cJSON_free(json);
    return -1;
  }
  FILE *file = fdopen(fd, "w");
  bool ok = file && fputs(json, file) >= 0 && fputc('\n', file) != EOF &&
            fflush(file) == 0 && fsync(fd) == 0;
  if (file)
    ok = fclose(file) == 0 && ok;
  else
    close(fd);
  if (ok)
    ok = rename(temporary, path) == 0;
  if (!ok)
    unlink(temporary);
  cJSON_free(json);
  return ok ? 0 : -1;
}

static bool is_our_manifest(const char *path) {
  FILE *file = fopen(path, "r");
  if (!file)
    return false;
  char contents[8192];
  size_t length = fread(contents, 1, sizeof(contents) - 1, file);
  fclose(file);
  contents[length] = '\0';
  cJSON *root = cJSON_Parse(contents);
  if (!root)
    return false;
  const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
  const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "path");
  bool ours = cJSON_IsString(name) && cJSON_IsString(host) &&
              strcmp(name->valuestring, HOST_NAME) == 0;
  if (ours) {
    const char *base = strrchr(host->valuestring, '/');
    ours = base && strcmp(base + 1, "cdm_native_host") == 0;
  }
  cJSON_Delete(root);
  return ours;
}

int browser_install_main(int argc, char **argv) {
  if (argc < 3 || !manifest_dir(argv[2]))
    goto usage;
  bool install = strcmp(argv[1], "install") == 0;
  bool uninstall = strcmp(argv[1], "uninstall") == 0;
  if (!install && !uninstall)
    goto usage;
  const char *id = NULL;
  if (install) {
    if (argc != 5 || strcmp(argv[3], "--id") != 0)
      goto usage;
    id = argv[4];
    if (strcmp(argv[2], "--firefox") == 0) {
      if (strcmp(id, "browser@cdm.local") != 0) {
        fprintf(stderr, "Firefox ID must match browser@cdm.local.\n");
        return 1;
      }
    } else if (!valid_chromium_id(id)) {
      fprintf(stderr, "Chromium extension ID must be 32 letters a-p.\n");
      return 1;
    }
  } else if (argc != 3) {
    goto usage;
  }
  char manifest[1200];
  if (!get_manifest_path(argv[2], manifest, sizeof(manifest), install)) {
    fprintf(stderr, "Could not locate browser manifest directory.\n");
    return 1;
  }
  if (install) {
    char host_path[1024];
    if (!get_host_path(host_path, sizeof(host_path))) {
      fprintf(stderr, "cdm_native_host must be executable beside cdm.\n");
      return 1;
    }
    if (write_manifest(manifest, host_path, argv[2], id) != 0) {
      fprintf(stderr, "Could not write %s: %s\n", manifest, strerror(errno));
      return 1;
    }
    printf("Registered %s in %s\n", HOST_NAME, manifest);
    return 0;
  }
  if (access(manifest, F_OK) != 0) {
    printf("No cdm manifest found for %s.\n", argv[2]);
    return 0;
  }
  if (!is_our_manifest(manifest)) {
    fprintf(stderr, "Refusing to remove an unrecognized manifest: %s\n",
            manifest);
    return 1;
  }
  if (unlink(manifest) != 0) {
    fprintf(stderr, "Could not remove %s: %s\n", manifest, strerror(errno));
    return 1;
  }
  printf("Removed %s\n", manifest);
  return 0;
usage:
  fprintf(stderr,
          "Usage: cdm browser install [--chrome|--chromium|--firefox] --id ID\n"
          "       cdm browser uninstall [--chrome|--chromium|--firefox]\n");
  return 1;
}
