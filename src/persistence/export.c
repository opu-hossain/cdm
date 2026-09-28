// SPDX-License-Identifier: MIT
/* TODO(platform): use native private temporary-file and sync APIs on Windows. */
#include "export.h"
#include "../platform/ipc_socket.h"
#include "../utils/config.h"
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
            add_u64(item, "total_size_bytes", row->total_size);
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
    int count = ipc_send_list_page_with_size(sock, offset, IPC_LIST_PAGE_MAX,
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
      daemon_version < 2 || (history && secrets && daemon_version < 3))
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
