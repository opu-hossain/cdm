// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "config.h"
#include "../platform/file_io.h"
#include "../platform/thread.h"
#include "../vendor/tomlc17.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

/* Defaults */
#define DEFAULT_MAX_CONCURRENT 3
#define DEFAULT_RETRY_MAX_ATTEMPTS 5
#define DEFAULT_RETRY_BASE_DELAY_SEC 2
#define DEFAULT_RETRY_MAX_DELAY_SEC 60
#define DEFAULT_MAX_SPEED_BPS 0
#define DEFAULT_MAX_CONNECTIONS 8
#define DEFAULT_CONNECT_TIMEOUT_SEC 10
#define DEFAULT_TRANSFER_TIMEOUT_SEC 30
#define DEFAULT_USER_AGENT "cdm/0.1"

/* Global state (initialised once) */
static int g_max_concurrent = DEFAULT_MAX_CONCURRENT;
static char g_default_dir[1024] = {0};
static int g_retry_max_attempts = DEFAULT_RETRY_MAX_ATTEMPTS;
static int g_retry_base_delay_sec = DEFAULT_RETRY_BASE_DELAY_SEC;
static int g_retry_max_delay_sec = DEFAULT_RETRY_MAX_DELAY_SEC;
static uint64_t g_max_speed_bps = DEFAULT_MAX_SPEED_BPS;
static ProxyMode g_proxy_mode = PROXY_NONE;
static char g_proxy_url[512];
static char g_proxy_username[128];
static char g_proxy_password[256];
static int g_max_connections = DEFAULT_MAX_CONNECTIONS;
static char g_user_agent[256] = DEFAULT_USER_AGENT;
static int g_connect_timeout_sec = DEFAULT_CONNECT_TIMEOUT_SEC;
static int g_transfer_timeout_sec = DEFAULT_TRANSFER_TIMEOUT_SEC;
/* Daemon scheduler and GUI config writes each run on their process main thread. */
static bool g_allow_shutdown;
static bool g_allow_sleep;
static bool g_allow_command;
/* GUI process reads and writes this only on its main thread. */
static bool g_clipboard_monitor;
static dm_mutex_t g_network_mutex;
static once_flag g_network_once = ONCE_FLAG_INIT;

static void init_network_mutex(void) { dm_mutex_init(&g_network_mutex); }
static void ensure_network_mutex(void) {
  call_once(&g_network_once, init_network_mutex);
}

static void reset_defaults(void) {
  g_max_concurrent = DEFAULT_MAX_CONCURRENT;
  g_retry_max_attempts = DEFAULT_RETRY_MAX_ATTEMPTS;
  g_retry_base_delay_sec = DEFAULT_RETRY_BASE_DELAY_SEC;
  g_retry_max_delay_sec = DEFAULT_RETRY_MAX_DELAY_SEC;
  g_max_speed_bps = DEFAULT_MAX_SPEED_BPS;
  g_allow_shutdown = g_allow_sleep = g_allow_command = false;
  g_clipboard_monitor = false;
  ensure_network_mutex();
  dm_mutex_lock(&g_network_mutex);
  g_proxy_mode = PROXY_NONE;
  g_proxy_url[0] = g_proxy_username[0] = g_proxy_password[0] = '\0';
  g_max_connections = DEFAULT_MAX_CONNECTIONS;
  memset(g_user_agent, 0, sizeof(g_user_agent));
  memcpy(g_user_agent, DEFAULT_USER_AGENT, sizeof(DEFAULT_USER_AGENT));
  g_connect_timeout_sec = DEFAULT_CONNECT_TIMEOUT_SEC;
  g_transfer_timeout_sec = DEFAULT_TRANSFER_TIMEOUT_SEC;
  dm_mutex_unlock(&g_network_mutex);
}

static void get_config_path(char *out, size_t out_size) {
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
#else
  const char *home = getenv("HOME");
#endif
  snprintf(out, out_size, "%s/.local/share/cdm/config.toml",
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

static int read_clamped_int(toml_datum_t tab, const char *key, int fallback,
                            int min, int max) {
  if (tab.type != TOML_TABLE)
    return fallback;
  toml_datum_t value = toml_get(tab, key);
  if (value.type != TOML_INT64)
    return fallback;
  if (value.u.int64 < min)
    return min;
  if (value.u.int64 > max)
    return max;
  return (int)value.u.int64;
}

static void read_u64(toml_datum_t tab, const char *key, uint64_t *out) {
  if (tab.type != TOML_TABLE)
    return;
  toml_datum_t d = toml_get(tab, key);
  if (d.type == TOML_INT64)
    *out = (uint64_t)d.u.int64;
}

static void read_bool(toml_datum_t tab, const char *key, bool *out) {
  if (tab.type != TOML_TABLE)
    return;
  toml_datum_t value = toml_get(tab, key);
  if (value.type == TOML_BOOLEAN)
    *out = value.u.boolean;
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

static bool proxy_url_valid(const char *url) {
  if (!url || !isalpha((unsigned char)url[0]))
    return false;
  const char *separator = strstr(url, "://");
  if (!separator || separator == url)
    return false;
  for (const char *p = url; p < separator; p++)
    if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.')
      return false;
  const char *host = separator + 3;
  if (!*host || *host == ':' || *host == '/' || *host == '?' || *host == '#')
    return false;
  for (const char *p = host; *p; p++) {
    if (isspace((unsigned char)*p) || (unsigned char)*p < 32 || *p == '@')
      return false;
    if (*p == '/' || *p == '?' || *p == '#')
      break;
  }
  return true;
}

static bool write_toml_string(FILE *fp, const char *value) {
  if (fputc('"', fp) == EOF)
    return false;
  for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
    const char *escape = NULL;
    if (*p == '"')
      escape = "\\\"";
    else if (*p == '\\')
      escape = "\\\\";
    else if (*p == '\n')
      escape = "\\n";
    else if (*p == '\r')
      escape = "\\r";
    else if (*p == '\t')
      escape = "\\t";
    if (escape) {
      if (fputs(escape, fp) == EOF)
        return false;
    } else if (*p < 32 || fputc(*p, fp) == EOF) {
      return false;
    }
  }
  return fputc('"', fp) != EOF;
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
  int max_connections = read_clamped_int(
      downloads, "max_connections_per_download", DEFAULT_MAX_CONNECTIONS, 1,
      16);
  char user_agent[sizeof(g_user_agent)] = DEFAULT_USER_AGENT;
  read_string(downloads, "user_agent", user_agent, sizeof(user_agent));
  if (!user_agent[0])
    memcpy(user_agent, DEFAULT_USER_AGENT, sizeof(DEFAULT_USER_AGENT));
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

  toml_datum_t actions = toml_get(root, "post_actions");
  read_bool(actions, "allow_shutdown", &g_allow_shutdown);
  read_bool(actions, "allow_sleep", &g_allow_sleep);
  read_bool(actions, "allow_command", &g_allow_command);

  toml_datum_t ui = toml_get(root, "ui");
  read_bool(ui, "clipboard_monitor", &g_clipboard_monitor);

  toml_datum_t timeouts = toml_get(root, "timeouts");
  int connect_timeout = read_clamped_int(
      timeouts, "connect_sec", DEFAULT_CONNECT_TIMEOUT_SEC, 1, 600);
  int transfer_timeout = read_clamped_int(
      timeouts, "transfer_sec", DEFAULT_TRANSFER_TIMEOUT_SEC, 1, 3600);

  toml_datum_t proxy = toml_get(root, "proxy");
  ProxyMode proxy_mode = PROXY_NONE;
  char proxy_url[sizeof(g_proxy_url)] = {0};
  char proxy_username[sizeof(g_proxy_username)] = {0};
  char proxy_password[sizeof(g_proxy_password)] = {0};
  if (proxy.type == TOML_TABLE) {
    toml_datum_t mode = toml_get(proxy, "mode");
    if (mode.type == TOML_INT64) {
      if (mode.u.int64 >= PROXY_NONE && mode.u.int64 <= PROXY_SOCKS5)
        proxy_mode = (ProxyMode)mode.u.int64;
      else
        LOG_WARN("Invalid proxy mode; using no proxy");
    }
    read_string(proxy, "url", proxy_url, sizeof(proxy_url));
    read_string(proxy, "username", proxy_username, sizeof(proxy_username));
    read_string(proxy, "password", proxy_password, sizeof(proxy_password));
    if (proxy_mode != PROXY_NONE && !proxy_url_valid(proxy_url)) {
      LOG_WARN("Proxy URL needs a scheme and host; using no proxy");
      proxy_mode = PROXY_NONE;
    }
  }
  ensure_network_mutex();
  dm_mutex_lock(&g_network_mutex);
  g_proxy_mode = proxy_mode;
  memcpy(g_proxy_url, proxy_url, sizeof(g_proxy_url));
  memcpy(g_proxy_username, proxy_username, sizeof(g_proxy_username));
  memcpy(g_proxy_password, proxy_password, sizeof(g_proxy_password));
  g_max_connections = max_connections;
  memcpy(g_user_agent, user_agent, sizeof(g_user_agent));
  g_connect_timeout_sec = connect_timeout;
  g_transfer_timeout_sec = transfer_timeout;
  dm_mutex_unlock(&g_network_mutex);

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

bool config_post_action_enabled(const char *action) {
  if (!action)
    return false;
  if (strcmp(action, "shutdown") == 0)
    return g_allow_shutdown;
  if (strcmp(action, "sleep") == 0)
    return g_allow_sleep;
  if (strcmp(action, "command") == 0)
    return g_allow_command;
  return false;
}

void config_get(DownloadManagerConfig *out) {
  if (!out)
    return;
  out->max_concurrent_downloads = g_max_concurrent;
  out->default_download_dir = g_default_dir;
  out->retry_max_attempts = g_retry_max_attempts;
  out->retry_base_delay_sec = g_retry_base_delay_sec;
  out->retry_max_delay_sec = g_retry_max_delay_sec;
  out->max_speed_bytes_per_sec = g_max_speed_bps;
  out->allow_shutdown = g_allow_shutdown;
  out->allow_sleep = g_allow_sleep;
  out->allow_command = g_allow_command;
  out->clipboard_monitor = g_clipboard_monitor;
  ensure_network_mutex();
  dm_mutex_lock(&g_network_mutex);
  out->proxy_mode = g_proxy_mode;
  memcpy(out->proxy_url, g_proxy_url, sizeof(out->proxy_url));
  memcpy(out->proxy_username, g_proxy_username, sizeof(out->proxy_username));
  memcpy(out->proxy_password, g_proxy_password, sizeof(out->proxy_password));
  out->max_connections_per_download = g_max_connections;
  memcpy(out->user_agent, g_user_agent, sizeof(out->user_agent));
  out->connect_timeout_sec = g_connect_timeout_sec;
  out->transfer_timeout_sec = g_transfer_timeout_sec;
  dm_mutex_unlock(&g_network_mutex);
}

bool config_save(const DownloadManagerConfig *config) {
  if (!config || !config->default_download_dir ||
      config->max_concurrent_downloads < 1 || config->retry_max_attempts < 0 ||
      config->retry_base_delay_sec < 1 ||
      config->retry_max_delay_sec < config->retry_base_delay_sec ||
      config->default_download_dir[0] == '\0' ||
      !default_dir_is_usable(config->default_download_dir) ||
      config->proxy_mode < PROXY_NONE || config->proxy_mode > PROXY_SOCKS5 ||
      !memchr(config->proxy_url, '\0', sizeof(config->proxy_url)) ||
      !memchr(config->proxy_username, '\0', sizeof(config->proxy_username)) ||
      !memchr(config->proxy_password, '\0', sizeof(config->proxy_password)) ||
      (config->proxy_mode != PROXY_NONE &&
       !proxy_url_valid(config->proxy_url)) ||
      config->max_connections_per_download < 1 ||
      config->max_connections_per_download > 16 ||
      !memchr(config->user_agent, '\0', sizeof(config->user_agent)) ||
      !config->user_agent[0] || config->connect_timeout_sec < 1 ||
      config->connect_timeout_sec > 600 ||
      config->transfer_timeout_sec < 1 ||
      config->transfer_timeout_sec > 3600)
    return false;

  char path[1024];
  get_config_path(path, sizeof(path));
  char directory[1024];
  snprintf(directory, sizeof(directory), "%s/.local/share/cdm",
           getenv("HOME") ? getenv("HOME") : "/tmp");
  if (file_ensure_directory(directory) != 0)
    return false;

  FILE *fp = fopen(path, "w");
  if (!fp)
    return false;
  int rc = fprintf(
      fp,
      "[downloads]\nmax_concurrent = %d\ndefault_directory = \"%s\"\n"
      "max_connections_per_download = %d\nuser_agent = ",
      config->max_concurrent_downloads, config->default_download_dir,
      config->max_connections_per_download);
  bool written = rc >= 0 && write_toml_string(fp, config->user_agent);
  if (written)
    written = fprintf(
                  fp,
                  "\n\n[retry]\nmax_attempts = %d\nbase_delay_sec = %d\n"
                  "max_delay_sec = %d\n\n[throttle]\nmax_speed_bytes_per_sec = %llu\n"
                  "\n[timeouts]\nconnect_sec = %d\ntransfer_sec = %d\n",
                  config->retry_max_attempts, config->retry_base_delay_sec,
                  config->retry_max_delay_sec,
                  (unsigned long long)config->max_speed_bytes_per_sec,
                  config->connect_timeout_sec,
                  config->transfer_timeout_sec) >= 0;
  if (written)
    written = fprintf(fp,
                      "\n[post_actions]\nallow_shutdown = %s\n"
                      "allow_sleep = %s\nallow_command = %s\n",
                      config->allow_shutdown ? "true" : "false",
                      config->allow_sleep ? "true" : "false",
                      config->allow_command ? "true" : "false") >= 0;
  if (written)
    written = fprintf(fp, "\n[ui]\nclipboard_monitor = %s\n",
                      config->clipboard_monitor ? "true" : "false") >= 0;
  if (written)
    written = fputs("\n[proxy]\nmode = ", fp) != EOF &&
              fprintf(fp, "%d\nurl = ", (int)config->proxy_mode) >= 0 &&
              write_toml_string(fp, config->proxy_url) &&
              fputs("\nusername = ", fp) != EOF &&
              write_toml_string(fp, config->proxy_username) &&
              fputs("\npassword = ", fp) != EOF &&
              write_toml_string(fp, config->proxy_password) &&
              fputc('\n', fp) != EOF;
  bool saved = written;
  if (fclose(fp) != 0)
    saved = false;
  if (saved)
    config_init(path);
  return saved;
}
