// SPDX-License-Identifier: MIT
/* TODO(platform): use native private temporary-file and sync APIs on Windows. */
#include "export.h"
#include "db.h"
#include "../platform/ipc_socket.h"
#include "../utils/config.h"
#include "../utils/url.h"
#include "../vendor/cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool add_string(cJSON *object, const char *key, const char *value) {
  return cJSON_AddStringToObject(object, key, value) != NULL;
}
static bool add_number(cJSON *object, const char *key, double value) {
  return cJSON_AddNumberToObject(object, key, value) != NULL;
}
static bool add_bool(cJSON *object, const char *key, bool value) {
  return cJSON_AddBoolToObject(object, key, value) != NULL;
}
static cJSON *section(cJSON *parent, const char *name) {
  cJSON *child = cJSON_CreateObject();
  if (!child)
    return NULL;
  if (!cJSON_AddItemToObject(parent, name, child)) {
    cJSON_Delete(child);
    return NULL;
  }
  return child;
}
static bool add_u64(cJSON *object, const char *key, uint64_t value) {
  char text[32];
  int n = snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
  return n >= 0 && (size_t)n < sizeof(text) && add_string(object, key, text);
}
static cJSON *make_settings(bool secrets) {
  DownloadManagerConfig config;
  config_get(&config);
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return NULL;
  cJSON *downloads = section(root, "downloads");
  cJSON *retry = section(root, "retry");
  cJSON *throttle = section(root, "throttle");
  cJSON *timeouts = section(root, "timeouts");
  cJSON *actions = section(root, "post_actions");
  cJSON *ui = section(root, "ui");
  cJSON *sites = section(root, "sites");
  cJSON *proxy = section(root, "proxy");
  bool ok = downloads && retry && throttle && timeouts && actions && ui &&
            sites && proxy &&
            add_number(downloads, "max_concurrent", config.max_concurrent_downloads) &&
            add_string(downloads, "default_directory", config.default_download_dir) &&
            add_number(downloads, "max_connections_per_download",
                       config.max_connections_per_download) &&
            add_string(downloads, "user_agent", config.user_agent) &&
            add_number(retry, "max_attempts", config.retry_max_attempts) &&
            add_number(retry, "base_delay_sec", config.retry_base_delay_sec) &&
            add_number(retry, "max_delay_sec", config.retry_max_delay_sec) &&
            add_u64(throttle, "max_speed_bytes_per_sec",
                    config.max_speed_bytes_per_sec) &&
            add_number(timeouts, "connect_sec", config.connect_timeout_sec) &&
            add_number(timeouts, "transfer_sec", config.transfer_timeout_sec) &&
            add_bool(actions, "allow_shutdown", config.allow_shutdown) &&
            add_bool(actions, "allow_sleep", config.allow_sleep) &&
            add_bool(actions, "allow_command", config.allow_command) &&
            add_bool(ui, "clipboard_monitor", config.clipboard_monitor) &&
            add_string(ui, "theme", config.ui_theme) &&
            add_bool(sites, "use_yt_dlp", config.use_yt_dlp) &&
            add_string(sites, "yt_dlp_path", config.yt_dlp_path) &&
            add_string(sites, "yt_dlp_format", config.yt_dlp_format) &&
            add_number(proxy, "mode", config.proxy_mode);
  if (ok && secrets)
    ok = add_string(proxy, "url", config.proxy_url) &&
         add_string(proxy, "username", config.proxy_username) &&
         add_string(proxy, "password", config.proxy_password);
  if (!ok) {
    cJSON_Delete(root);
    return NULL;
  }
  return root;
}
static bool write_json(FILE *file, cJSON *object) {
  char *rendered = cJSON_PrintUnformatted(object);
  if (!rendered)
    return false;
  size_t length = strlen(rendered);
  bool ok = fwrite(rendered, 1, length, file) == length;
  cJSON_free(rendered);
  return ok;
}
static bool write_row(FILE *file, const IpcDownloadRecord *row, int sock,
                      bool secrets) {
  cJSON *item = cJSON_CreateObject();
  if (!item)
    return false;
  bool ok = add_number(item, "id", row->id) &&
            add_string(item, "url", row->url) &&
            add_string(item, "dest_path", row->dest_path) &&
            add_string(item, "status", row->status) &&
            add_number(item, "progress", isfinite(row->progress) ? row->progress : -1) &&
            add_u64(item, "total_size_bytes", row->total_size) &&
            add_number(item, "media_kind", row->media_kind) &&
            add_bool(item, "site_grab", row->site_grab != 0) &&
            add_bool(item, "requires_browser_context",
                     row->requires_browser_context != 0);
  if (ok && secrets) {
    IpcDownloadDetails details = {0};
    ok = ipc_send_get_details_v2(sock, row->id, &details) == 0 &&
         add_string(item, "cookie", details.cookie) &&
         add_string(item, "referrer", details.referrer) &&
         add_string(item, "extra_headers", details.extra_headers) &&
         add_string(item, "auth_user", details.auth_user);
  }
  if (ok)
    ok = write_json(file, item);
  cJSON_Delete(item);
  return ok;
}
static bool write_history(FILE *file, int sock, bool secrets) {
  IpcDownloadRecord *rows = calloc(IPC_LIST_PAGE_MAX, sizeof(*rows));
  if (!rows)
    return false;
  bool ok = fputs(",\"downloads\":[", file) != EOF;
  uint32_t offset = 0, total = 0;
  bool first = true;
  while (ok) {
    int count = ipc_send_export_page_v1(sock, offset, IPC_LIST_PAGE_MAX,
                                              rows, IPC_LIST_PAGE_MAX, &total);
    if (count < 0) {
      ok = false;
      break;
    }
    for (int i = 0; i < count && ok; i++) {
      if (!first)
        ok = fputc(',', file) != EOF;
      if (ok)
        ok = write_row(file, &rows[i], sock, secrets);
      first = false;
    }
    uint64_t next = (uint64_t)offset + (uint32_t)count;
    if (count == 0 || next >= total)
      break;
    if (next > UINT32_MAX) {
      ok = false;
      break;
    }
    offset = (uint32_t)next;
  }
  free(rows);
  return ok && fputc(']', file) != EOF;
}
int export_json_file(int sock, uint16_t daemon_version,
                     const char *destination, bool history, bool secrets) {
  if (sock < 0 || !destination || !destination[0] ||
      daemon_version < 2 || (history && daemon_version < 13) ||
      (history && secrets && daemon_version < 3))
    return -1;
  size_t length = strlen(destination);
  if (length > 4000)
    return -1;
  char temporary[4096];
  int n = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", destination);
  if (n < 0 || (size_t)n >= sizeof(temporary))
    return -1;
  int fd = mkstemp(temporary);
  if (fd < 0)
    return -1;
  bool ok = fchmod(fd, 0600) == 0;
  FILE *file = ok ? fdopen(fd, "wb") : NULL;
  if (!file)
    close(fd);
  cJSON *settings = file ? make_settings(secrets) : NULL;
  if (file && settings) {
    ok = fputs("{\"version\":1,\"settings\":", file) != EOF &&
         write_json(file, settings);
    if (ok)
      ok = history ? write_history(file, sock, secrets)
                   : fputs(",\"downloads\":[]", file) != EOF;
    if (ok)
      ok = fputs("}\n", file) != EOF && fflush(file) == 0 &&
           fsync(fileno(file)) == 0;
  } else
    ok = false;
  cJSON_Delete(settings);
  if (file && fclose(file) != 0)
    ok = false;
  if (ok)
    ok = rename(temporary, destination) == 0;
  if (!ok)
    unlink(temporary);
  return ok ? 0 : -1;
}

static bool known_keys(const cJSON *object, const char *const *names,
                       size_t count) {
  if (!cJSON_IsObject(object))
    return false;
  for (const cJSON *item = object->child; item; item = item->next) {
    bool allowed = false;
    for (size_t i = 0; i < count; i++)
      allowed |= strcmp(item->string, names[i]) == 0;
    if (!allowed)
      return false;
    for (const cJSON *prior = object->child; prior != item; prior = prior->next)
      if (strcmp(prior->string, item->string) == 0)
        return false;
  }
  return true;
}
static const cJSON *field(const cJSON *object, const char *name) {
  return cJSON_GetObjectItemCaseSensitive(object, name);
}
static bool json_int(const cJSON *object, const char *name, int min, int max,
                     int *out) {
  const cJSON *value = field(object, name);
  if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
      value->valuedouble != floor(value->valuedouble) ||
      value->valuedouble < min || value->valuedouble > max)
    return false;
  *out = (int)value->valuedouble;
  return true;
}
static bool json_bool(const cJSON *object, const char *name, bool *out) {
  const cJSON *value = field(object, name);
  if (!cJSON_IsBool(value))
    return false;
  *out = cJSON_IsTrue(value);
  return true;
}
static bool json_text(const cJSON *object, const char *name, char *out,
                      size_t capacity, bool required) {
  const cJSON *value = field(object, name);
  if (!value)
    return !required;
  if (!cJSON_IsString(value) || !value->valuestring)
    return false;
  size_t length = strlen(value->valuestring);
  if (length >= capacity)
    return false;
  memcpy(out, value->valuestring, length + 1);
  return true;
}
static bool json_u64(const cJSON *object, const char *name, uint64_t max,
                     uint64_t *out) {
  const cJSON *value = field(object, name);
  if (!cJSON_IsString(value) || !value->valuestring ||
      !value->valuestring[0])
    return false;
  uint64_t number = 0;
  for (const unsigned char *p = (const unsigned char *)value->valuestring; *p;
       p++) {
    if (*p < '0' || *p > '9' || number > (max - (*p - '0')) / 10)
      return false;
    number = number * 10 + (*p - '0');
  }
  *out = number;
  return true;
}
static bool parse_settings(const cJSON *settings, DownloadManagerConfig *config,
                           char directory[1024]) {
  static const char *const sections[] = {
      "downloads", "retry", "throttle", "timeouts", "post_actions", "ui",
      "sites", "proxy"};
  if (!known_keys(settings, sections, 8))
    return false;
  const cJSON *d = field(settings, "downloads"), *r = field(settings, "retry"),
              *t = field(settings, "throttle"), *time = field(settings, "timeouts"),
              *a = field(settings, "post_actions"), *ui = field(settings, "ui"),
              *s = field(settings, "sites"), *p = field(settings, "proxy");
  static const char *const dkeys[] = {"max_concurrent", "default_directory",
      "max_connections_per_download", "user_agent"};
  static const char *const rkeys[] = {"max_attempts", "base_delay_sec", "max_delay_sec"};
  static const char *const tkeys[] = {"max_speed_bytes_per_sec"};
  static const char *const timekeys[] = {"connect_sec", "transfer_sec"};
  static const char *const akeys[] = {"allow_shutdown", "allow_sleep", "allow_command"};
  static const char *const uikeys[] = {"clipboard_monitor", "theme"};
  static const char *const skeys[] = {"use_yt_dlp", "yt_dlp_path", "yt_dlp_format"};
  static const char *const pkeys[] = {"mode", "url", "username", "password"};
  if (!known_keys(d, dkeys, 4) || !known_keys(r, rkeys, 3) ||
      !known_keys(t, tkeys, 1) || !known_keys(time, timekeys, 2) ||
      !known_keys(a, akeys, 3) || !known_keys(ui, uikeys, 2) ||
      !known_keys(s, skeys, 3) || !known_keys(p, pkeys, 4))
    return false;
  config_get(config);
  memcpy(config->ui_theme, "system", sizeof("system"));
  int proxy_mode = 0;
  bool ok = json_int(d, "max_concurrent", 1, 64,
                     &config->max_concurrent_downloads) &&
            json_text(d, "default_directory", directory, 1024, true) &&
            directory[0] &&
            json_int(d, "max_connections_per_download", 1, 16,
                     &config->max_connections_per_download) &&
            json_text(d, "user_agent", config->user_agent,
                      sizeof(config->user_agent), true) &&
            config->user_agent[0] &&
            json_int(r, "max_attempts", 0, 1000,
                     &config->retry_max_attempts) &&
            json_int(r, "base_delay_sec", 1, 86400,
                     &config->retry_base_delay_sec) &&
            json_int(r, "max_delay_sec", 1, 86400,
                     &config->retry_max_delay_sec) &&
            config->retry_max_delay_sec >= config->retry_base_delay_sec &&
            json_u64(t, "max_speed_bytes_per_sec", UINT64_MAX,
                     &config->max_speed_bytes_per_sec) &&
            json_int(time, "connect_sec", 1, 600,
                     &config->connect_timeout_sec) &&
            json_int(time, "transfer_sec", 1, 3600,
                     &config->transfer_timeout_sec) &&
            json_bool(a, "allow_shutdown", &config->allow_shutdown) &&
            json_bool(a, "allow_sleep", &config->allow_sleep) &&
            json_bool(a, "allow_command", &config->allow_command) &&
            json_bool(ui, "clipboard_monitor", &config->clipboard_monitor) &&
            json_text(ui, "theme", config->ui_theme,
                      sizeof(config->ui_theme), false) &&
            json_bool(s, "use_yt_dlp", &config->use_yt_dlp) &&
            json_text(s, "yt_dlp_path", config->yt_dlp_path,
                      sizeof(config->yt_dlp_path), true) &&
            config->yt_dlp_path[0] &&
            json_text(s, "yt_dlp_format", config->yt_dlp_format,
                      sizeof(config->yt_dlp_format), true) &&
            config->yt_dlp_format[0] &&
            json_int(p, "mode", 0, 2, &proxy_mode) &&
            json_text(p, "url", config->proxy_url, sizeof(config->proxy_url),
                      false) &&
            json_text(p, "username", config->proxy_username,
                      sizeof(config->proxy_username), false) &&
            json_text(p, "password", config->proxy_password,
                      sizeof(config->proxy_password), false);
  if (!ok)
    return false;
  config->proxy_mode = field(p, "url") ? (ProxyMode)proxy_mode : PROXY_NONE;
  config->default_download_dir = directory;
  return true;
}
static bool safe_import_path(const char *path) {
  if (path[0] != '/' || strlen(path) >= 900)
    return false;
  const char *basename = strrchr(path, '/') + 1;
  if (!basename[0] || strcmp(basename, ".") == 0 ||
      strcmp(basename, "..") == 0)
    return false;
  const char *root = getenv("DOWNLOADMGR_ROOT");
  if (!root)
    root = getenv("HOME");
  if (!root)
    return false;
  char root_resolved[4096], parent[1024], parent_resolved[4096];
  if (!realpath(root, root_resolved))
    return false;
  strcpy(parent, path);
  char *slash = strrchr(parent, '/');
  if (!slash)
    return false;
  if (slash == parent)
    slash[1] = 0;
  else
    *slash = 0;
  if (!realpath(parent, parent_resolved))
    return false;
  size_t length = strlen(root_resolved);
  return strncmp(parent_resolved, root_resolved, length) == 0 &&
         (parent_resolved[length] == 0 || parent_resolved[length] == '/');
}
static int compare_rows(const void *left, const void *right) {
  const DbImportRow *a = left, *b = right;
  return (a->id > b->id) - (a->id < b->id);
}
static bool parse_rows(const cJSON *array, DbImportRow **out, size_t *count) {
  if (!cJSON_IsArray(array))
    return false;
  int length = cJSON_GetArraySize(array);
  if (length < 0 || length > 100000)
    return false;
  DbImportRow *rows = calloc(length ? (size_t)length : 1, sizeof(*rows));
  if (!rows)
    return false;
  static const char *const keys[] = {
      "id", "url", "dest_path", "status", "progress", "total_size_bytes",
      "cookie", "referrer", "extra_headers", "auth_user", "media_kind",
      "site_grab", "requires_browser_context"};
  bool ok = true;
  for (int i = 0; i < length && ok; i++) {
    const cJSON *item = cJSON_GetArrayItem(array, i);
    uint64_t size = 0;
    const cJSON *id = field(item, "id"),
                *url = field(item, "url"), *path = field(item, "dest_path"),
                *status = field(item, "status"), *progress = field(item, "progress");
    ok = known_keys(item, keys, 13) &&
         cJSON_IsNumber(id) && isfinite(id->valuedouble) &&
         id->valuedouble == floor(id->valuedouble) &&
         id->valuedouble >= 1 && id->valuedouble < UINT32_MAX &&
         cJSON_IsString(url) && url->valuestring &&
         strlen(url->valuestring) < 2048 &&
         cJSON_IsString(path) && path->valuestring &&
         strlen(path->valuestring) < 1024 &&
         cJSON_IsString(status) && status->valuestring &&
         cJSON_IsNumber(progress) && isfinite(progress->valuedouble) &&
         progress->valuedouble >= -1 && progress->valuedouble <= 1 &&
         json_u64(item, "total_size_bytes", INT64_MAX, &size);
    int media_kind = 0;
    bool site_grab = false, requires_browser_context = false;
    const cJSON *media = field(item, "media_kind");
    const cJSON *site = field(item, "site_grab");
    const cJSON *context = field(item, "requires_browser_context");
    if (ok && media)
      ok = json_int(item, "media_kind", 0, 3, &media_kind);
    if (ok && site)
      ok = json_bool(item, "site_grab", &site_grab);
    if (ok && context)
      ok = json_bool(item, "requires_browser_context", &requires_browser_context);
    if (ok && site_grab && media_kind != 0)
      ok = false;
    if (!ok)
      break;
    char normalized[2048];
    const char *state = status->valuestring;
    ok = url_normalize(url->valuestring, normalized, sizeof(normalized)) &&
         (strcmp(state, "DONE") == 0 || strcmp(state, "ERROR") == 0 ||
          strcmp(state, "CANCELED") == 0 || strcmp(state, "PAUSED") == 0 ||
          strcmp(state, "QUEUED") == 0 || strcmp(state, "ACTIVE") == 0) &&
         safe_import_path(path->valuestring);
    const char *const secret_keys[] = {"cookie", "referrer", "extra_headers", "auth_user"};
    const size_t caps[] = {1024, 2048, 4096, 128};
    for (size_t j = 0; j < 4 && ok; j++) {
      const cJSON *secret = field(item, secret_keys[j]);
      ok = !secret || (cJSON_IsString(secret) && secret->valuestring &&
                       strlen(secret->valuestring) < caps[j]);
    }
    if (!ok)
      break;
    rows[i] = (DbImportRow){.id = (uint32_t)id->valuedouble,
                            .url = url->valuestring,
                            .dest_path = path->valuestring,
                            .status = strcmp(state, "QUEUED") == 0 ||
                                              strcmp(state, "ACTIVE") == 0
                                          ? "PAUSED" : state,
                            .total_size = size};
    rows[i].media_kind = (uint32_t)media_kind;
    rows[i].site_grab = site_grab;
    rows[i].requires_browser_context = requires_browser_context;
    rows[i].cookie = field(item, "cookie") ? field(item, "cookie")->valuestring : NULL;
    rows[i].referrer = field(item, "referrer") ? field(item, "referrer")->valuestring : NULL;
    rows[i].extra_headers = field(item, "extra_headers") ? field(item, "extra_headers")->valuestring : NULL;
    rows[i].auth_user = field(item, "auth_user") ? field(item, "auth_user")->valuestring : NULL;
  }
  if (ok) {
    qsort(rows, (size_t)length, sizeof(*rows), compare_rows);
    for (int i = 1; i < length; i++)
      if (rows[i - 1].id == rows[i].id)
        ok = false;
  }
  if (!ok) {
    free(rows);
    return false;
  }
  *out = rows;
  *count = (size_t)length;
  return true;
}
int import_json_file(const char *source, bool replace) {
  if (!source)
    return -1;
  int fd = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) {
    if (fd >= 0)
      close(fd);
    return -1;
  }
  size_t length = (size_t)st.st_size;
  char *buffer = malloc(length + 1);
  if (!buffer) { close(fd); return -3; }
  size_t used = 0;
  while (used < length) {
    ssize_t got = read(fd, buffer + used, length - used);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0) { free(buffer); close(fd); return -1; }
    used += (size_t)got;
  }
  close(fd);
  buffer[length] = 0;
  cJSON *root = cJSON_ParseWithLengthOpts(buffer, length + 1, NULL, true);
  free(buffer);
  static const char *const keys[] = {"version", "settings", "downloads"};
  int version = 0;
  DownloadManagerConfig config;
  char directory[1024];
  DbImportRow *rows = NULL;
  size_t count = 0;
  int result = -1;
  if (!known_keys(root, keys, 3) ||
      !json_int(root, "version", 1, 1, &version) ||
      !parse_settings(field(root, "settings"), &config, directory) ||
      !config_validate(&config) ||
      !parse_rows(field(root, "downloads"), &rows, &count))
    goto finish;
  if (replace) {
    if (queue_manager_count_by_status(DOWNLOAD_ACTIVE) > 0) {
      result = -2;
      goto finish;
    }
    DownloadManagerConfig previous;
    char old_directory[1024];
    config_get(&previous);
    int n = snprintf(old_directory, sizeof(old_directory), "%s",
                     previous.default_download_dir);
    if (n < 0 || (size_t)n >= sizeof(old_directory)) {
      result = -3;
      goto finish;
    }
    previous.default_download_dir = old_directory;
    if (!config_save(&config)) {
      config_save(&previous);
      result = -3;
      goto finish;
    }
    result = db_import_history(rows, count, true);
    if (result != 0 && !config_save(&previous))
      result = -3;
  } else
    result = db_import_history(rows, count, false);
  if (result == -1)
    result = -3;
finish:
  free(rows);
  cJSON_Delete(root);
  return result;
}
