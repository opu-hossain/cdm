// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "config.h"
#include "../platform/file_io.h"
#include "../vendor/tomlc17.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defaults */
#define DEFAULT_MAX_CONCURRENT 3
#define DEFAULT_RETRY_MAX_ATTEMPTS 5
#define DEFAULT_RETRY_BASE_DELAY_SEC 2
#define DEFAULT_RETRY_MAX_DELAY_SEC 60
#define DEFAULT_MAX_SPEED_BPS 0

/* Global state (initialised once) */
static int g_max_concurrent = DEFAULT_MAX_CONCURRENT;
static char g_default_dir[1024] = {0};
static int g_retry_max_attempts = DEFAULT_RETRY_MAX_ATTEMPTS;
static int g_retry_base_delay_sec = DEFAULT_RETRY_BASE_DELAY_SEC;
static int g_retry_max_delay_sec = DEFAULT_RETRY_MAX_DELAY_SEC;
static uint64_t g_max_speed_bps = DEFAULT_MAX_SPEED_BPS;

static void reset_defaults(void) {
  g_max_concurrent = DEFAULT_MAX_CONCURRENT;
  g_retry_max_attempts = DEFAULT_RETRY_MAX_ATTEMPTS;
  g_retry_base_delay_sec = DEFAULT_RETRY_BASE_DELAY_SEC;
  g_retry_max_delay_sec = DEFAULT_RETRY_MAX_DELAY_SEC;
  g_max_speed_bps = DEFAULT_MAX_SPEED_BPS;
}

static void get_config_path(char *out, size_t out_size) {
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
#else
  const char *home = getenv("HOME");
#endif
  snprintf(out, out_size, "%s/.local/share/downloadmgr/config.toml",
           home ? home : "/tmp");
}

/* Helpers */

static void set_default_dir(void) {
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
  const char sep = '\\';
#else
  const char *home = getenv("HOME");
  const char sep = '/';
#endif
  snprintf(g_default_dir, sizeof(g_default_dir), "%s%cDownloads",
           home ? home : ".", sep);
  file_ensure_directory(g_default_dir);
}

static void read_int(toml_datum_t tab, const char *key, int *out) {
  if (tab.type != TOML_TABLE)
    return;
  toml_datum_t d = toml_get(tab, key);
  if (d.type == TOML_INT64)
    *out = (int)d.u.int64;
}

static void read_u64(toml_datum_t tab, const char *key, uint64_t *out) {
  if (tab.type != TOML_TABLE)
    return;
  toml_datum_t d = toml_get(tab, key);
  if (d.type == TOML_INT64)
    *out = (uint64_t)d.u.int64;
}

static void read_string(toml_datum_t tab, const char *key, char *out,
                        size_t out_size) {
  if (tab.type != TOML_TABLE)
    return;
  toml_datum_t d = toml_get(tab, key);
  if (d.type == TOML_STRING) {
    strncpy(out, d.u.str.ptr, out_size - 1);
    out[out_size - 1] = '\0';
    /* tomlc17 owns the string memory; freed via toml_free() */
  }
}

static bool default_dir_is_usable(const char *path) {
  if (!path || path[0] == '\0')
    return false;

  const char *home = getenv("HOME");
  if (!home || home[0] == '\0')
    return false;

  size_t home_len = strlen(home);
  if (strncmp(path, home, home_len) != 0)
    return false;
  if (path[home_len] != '\0' && path[home_len] != '/')
    return false;

  if (file_ensure_directory(path) != 0)
    return false;

  char resolved[1024];
  return realpath(path, resolved) != NULL;
}

/* Public API */

void config_init(const char *path) {
  reset_defaults();
  set_default_dir();

  char resolved_path[1024];
  if (!path) {
    get_config_path(resolved_path, sizeof(resolved_path));
    path = resolved_path;
  }

  FILE *fp = fopen(path, "r");
  if (!fp) {
    LOG_INFO("No config file at %s, using defaults", path);
    return;
  }

  toml_result_t result = toml_parse_file(fp);
  fclose(fp);

  if (!result.ok) {
    LOG_WARN("Config file %s failed to parse (%s), using defaults", path,
             result.errmsg);
    return;
  }

  toml_datum_t root = result.toptab;

  toml_datum_t downloads = toml_get(root, "downloads");
  read_int(downloads, "max_concurrent", &g_max_concurrent);
  char configured_default_dir[sizeof(g_default_dir)] = {0};
  read_string(downloads, "default_directory", configured_default_dir,
              sizeof(configured_default_dir));
  if (configured_default_dir[0] != '\0' &&
      default_dir_is_usable(configured_default_dir)) {
    strncpy(g_default_dir, configured_default_dir, sizeof(g_default_dir) - 1);
    g_default_dir[sizeof(g_default_dir) - 1] = '\0';
  } else if (configured_default_dir[0] != '\0') {
    LOG_WARN("Ignoring invalid default_directory '%s'; using '%s'",
             configured_default_dir, g_default_dir);
  }

  toml_datum_t retry = toml_get(root, "retry");
  read_int(retry, "max_attempts", &g_retry_max_attempts);
  read_int(retry, "base_delay_sec", &g_retry_base_delay_sec);
  read_int(retry, "max_delay_sec", &g_retry_max_delay_sec);

  toml_datum_t throttle = toml_get(root, "throttle");
  read_u64(throttle, "max_speed_bytes_per_sec", &g_max_speed_bps);

  toml_free(result);

  /* Defensive clamps — prevent a bad config from breaking the scheduler. */
  if (g_max_concurrent < 1)
    g_max_concurrent = 1;
  if (g_retry_max_attempts < 0)
    g_retry_max_attempts = 0;
  if (g_retry_base_delay_sec < 1)
    g_retry_base_delay_sec = 1;
  if (g_retry_max_delay_sec < g_retry_base_delay_sec)
    g_retry_max_delay_sec = g_retry_base_delay_sec;

  LOG_INFO("Loaded config from %s", path);
}

int config_get_max_concurrent_downloads(void) { return g_max_concurrent; }
const char *config_get_default_download_dir(void) { return g_default_dir; }
int config_get_retry_max_attempts(void) { return g_retry_max_attempts; }
int config_get_retry_base_delay_sec(void) { return g_retry_base_delay_sec; }
int config_get_retry_max_delay_sec(void) { return g_retry_max_delay_sec; }
uint64_t config_get_max_speed_bytes_per_sec(void) { return g_max_speed_bps; }

void config_get(DownloadManagerConfig *out) {
  if (!out)
    return;
  out->max_concurrent_downloads = g_max_concurrent;
  out->default_download_dir = g_default_dir;
  out->retry_max_attempts = g_retry_max_attempts;
  out->retry_base_delay_sec = g_retry_base_delay_sec;
  out->retry_max_delay_sec = g_retry_max_delay_sec;
  out->max_speed_bytes_per_sec = g_max_speed_bps;
}

bool config_save(const DownloadManagerConfig *config) {
  if (!config || !config->default_download_dir ||
      config->max_concurrent_downloads < 1 || config->retry_max_attempts < 0 ||
      config->retry_base_delay_sec < 1 ||
      config->retry_max_delay_sec < config->retry_base_delay_sec ||
      config->default_download_dir[0] == '\0' ||
      !default_dir_is_usable(config->default_download_dir))
    return false;

  char path[1024];
  get_config_path(path, sizeof(path));
  char directory[1024];
  snprintf(directory, sizeof(directory), "%s/.local/share/downloadmgr",
           getenv("HOME") ? getenv("HOME") : "/tmp");
  if (file_ensure_directory(directory) != 0)
    return false;

  FILE *fp = fopen(path, "w");
  if (!fp)
    return false;
  int rc = fprintf(
      fp,
      "[downloads]\nmax_concurrent = %d\ndefault_directory = \"%s\"\n\n"
      "[retry]\nmax_attempts = %d\nbase_delay_sec = %d\n"
      "max_delay_sec = %d\n\n[throttle]\nmax_speed_bytes_per_sec = %llu\n",
      config->max_concurrent_downloads, config->default_download_dir,
      config->retry_max_attempts, config->retry_base_delay_sec,
      config->retry_max_delay_sec,
      (unsigned long long)config->max_speed_bytes_per_sec);
  bool saved = rc >= 0 && fclose(fp) == 0;
  if (saved)
    config_init(path);
  else
    fclose(fp);
  return saved;
}
