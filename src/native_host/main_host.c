// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "../platform/ipc_socket.h"
#include "../utils/config.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/**
 * Extract the first "url" field from a JSON object.
 *
 * Looks for the pattern `"url":"..."`. Writes the value (without quotes)
 * into `out`, truncated to `out_size - 1`. If not found, `out` is set to
 * an empty string.
 */
static void extract_url(const char *json, char *out, size_t out_size) {
  const char *key = "\"url\":\"";
  const char *start = strstr(json, key);
  if (!start) {
    out[0] = '\0';
    return;
  }
  start += strlen(key);

  const char *end = strchr(start, '"');
  if (!end) {
    out[0] = '\0';
    return;
  }

  size_t len = (size_t)(end - start);
  if (len >= out_size)
    len = out_size - 1;
  memcpy(out, start, len);
  out[len] = '\0';
}

/**
 * Derive a filename from the last path segment of a URL, stripping any
 * query string. Falls back to "download.bin" if the URL ends with a
 * slash.
 */
static void filename_from_url(const char *url, char *out, size_t out_size) {
  const char *slash = strrchr(url, '/');
  const char *name = slash ? slash + 1 : url;

  char temp[512];
  strncpy(temp, name, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = '\0';

  char *query = strchr(temp, '?');
  if (query)
    *query = '\0';

  if (temp[0] == '\0')
    strncpy(temp, "download.bin", sizeof(temp) - 1);

  strncpy(out, temp, out_size - 1);
  out[out_size - 1] = '\0';
}

/**
 * Join a directory and filename into a full path.
 */
static void join_path(const char *dir, const char *filename, char *out,
                      size_t out_size) {
  size_t dir_len = strlen(dir);
  if (dir_len > 0 && dir[dir_len - 1] == '/')
    snprintf(out, out_size, "%s%s", dir, filename);
  else
    snprintf(out, out_size, "%s/%s", dir, filename);
}

/**
 * Return the default downloads directory (creating it if necessary).
 *
 * The daemon's `is_safe_dest_path()` requires the directory to already
 * exist, so we pre‑create it here.
 */
static void default_downloads_dir(char *out, size_t out_size) {
  snprintf(out, out_size, "%s", config_get_default_download_dir());
  mkdir(out, 0755); /* harmless EEXIST if already present */
}

/* ------------------------------------------------------------------ */
/*  Native Messaging entry point                                      */
/* ------------------------------------------------------------------ */

/**
 * Browser Native Host main entry point.
 *
 * Reads a length‑prefixed JSON message from stdin, extracts the URL and
 * optional context (cookie, referrer, extra headers), and forwards the
 * download request to the daemon via IPC.
 */
int main(void) {
  config_init(NULL);

  /* Read 4‑byte length (native byte order). */
  uint32_t msg_len = 0;
  if (fread(&msg_len, sizeof(msg_len), 1, stdin) != 1)
    return 1;
  if (msg_len > 1024 * 1024) /* sanity cap */
    return 1;

  char *json = malloc(msg_len + 1);
  if (!json)
    return 1;
  if (fread(json, 1, msg_len, stdin) != msg_len) {
    free(json);
    return 1;
  }
  json[msg_len] = '\0';

  cJSON *root = cJSON_Parse(json);
  free(json);
  if (!root)
    return 1;

  cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
  if (!cJSON_IsString(url_item) || !url_item->valuestring ||
      url_item->valuestring[0] == '\0') {
    cJSON_Delete(root);
    return 1;
  }

  char url[IPC_MAX_URL_LEN];
  strncpy(url, url_item->valuestring, sizeof(url) - 1);
  url[sizeof(url) - 1] = '\0';

  /* Optional extra context that the browser extension may provide. */
  cJSON *v;
  v = cJSON_GetObjectItemCaseSensitive(root, "cookie");
  const char *cookie =
      (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
  v = cJSON_GetObjectItemCaseSensitive(root, "referrer");
  const char *referrer =
      (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
  v = cJSON_GetObjectItemCaseSensitive(root, "extra_headers");
  const char *extra_headers =
      (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;

  /* Build the destination path. */
  char dest_dir[IPC_MAX_PATH_LEN];
  char filename[512];
  char full_path[IPC_MAX_PATH_LEN];
  default_downloads_dir(dest_dir, sizeof(dest_dir));
  filename_from_url(url, filename, sizeof(filename));
  join_path(dest_dir, filename, full_path, sizeof(full_path));

  int sock = ipc_client_connect();
  if (sock >= 0) {
    IpcDownloadOptions opts = {
        .cookie = cookie,
        .referrer = referrer,
        .extra_headers = extra_headers,
    };
    bool has_options = cookie || referrer || extra_headers;
    ipc_send_add_download(sock, url, full_path, has_options ? &opts : NULL);
    ipc_client_disconnect(sock);
  }

  cJSON_Delete(root);
  return 0;
}
