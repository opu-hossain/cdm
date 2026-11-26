// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "../platform/ipc_socket.h"
#include "../platform/spawn.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/i18n.h"
#include "../utils/log.h"
#include "../vendor/cJSON.h"

#include <errno.h>
#include <ctype.h>
#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define NATIVE_MAX_MESSAGE (1024 * 1024)
#define HOST_MAX_OFFERS 64
#define HOST_MAX_EXCLUSIONS 64

/* Native host stdin/event loop is the sole owner of the exclusion policy. */
static char g_excluded_sites[HOST_MAX_EXCLUSIONS][256];
static size_t g_excluded_count;

typedef struct {
  char cookie[4097];
  char user_agent[257];
  char referer[2049];
  char youtube_sabr_url[2048];
  char youtube_request_b64[22000];
  uint32_t youtube_height;
} HostRequestContext;

typedef struct {
  IpcBrowserOffer offer;
  IpcBrowserOfferState reported_state;
  uint64_t reported_bytes;
  char reported_status[24];
} HostOffer;

static HostOffer g_offers[HOST_MAX_OFFERS];

static bool cdm_executable(char *out, size_t capacity) {
  get_self_exe_path(out, capacity);
  char *base = strrchr(out, '/');
  base = base ? base + 1 : out;
  if (strcmp(base, "cdm_native_host") != 0)
    return false;
  strcpy(base, "cdm");
  return access(out, X_OK) == 0;
}

static bool ensure_daemon_running(void) {
  if (ipc_server_is_running())
    return true;
  char executable[1024];
  if (!cdm_executable(executable, sizeof(executable)) ||
      spawn_daemon_detached(executable) != 0)
    return false;
  for (int i = 0; i < 50; i++) {
    if (ipc_server_is_running())
      return true;
    dm_thread_sleep_ms(100);
  }
  return false;
}

static bool native_send(cJSON *message) {
  char *json = cJSON_PrintUnformatted(message);
  if (!json)
    return false;
  size_t length = strlen(json);
  uint32_t wire_length = (uint32_t)length;
  bool ok = length <= NATIVE_MAX_MESSAGE &&
            fwrite(&wire_length, sizeof(wire_length), 1, stdout) == 1 &&
            fwrite(json, 1, length, stdout) == length && fflush(stdout) == 0;
  cJSON_free(json);
  return ok;
}

/* Returns 1 for a frame, 0 for EOF, and -1 for a malformed frame. */
static int native_read(char **out) {
  uint32_t length = 0;
  size_t prefix = fread(&length, 1, sizeof(length), stdin);
  if (prefix != sizeof(length))
    return prefix == 0 && feof(stdin) ? 0 : -1;
  if (length == 0 || length > NATIVE_MAX_MESSAGE)
    return -1;
  char *json = malloc((size_t)length + 1);
  if (!json)
    return -1;
  if (fread(json, 1, length, stdin) != length) {
    free(json);
    return -1;
  }
  if (memchr(json, '\0', length)) {
    free(json);
    return -1;
  }
  json[length] = '\0';
  *out = json;
  return 1;
}

static bool copy_field(const cJSON *root, const char *name, char *out,
                       size_t capacity, bool required) {
  const cJSON *field = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!field && !required) {
    out[0] = '\0';
    return true;
  }
  if (!cJSON_IsString(field) || !field->valuestring ||
      (required && !field->valuestring[0]) ||
      strlen(field->valuestring) >= capacity)
    return false;
  strcpy(out, field->valuestring);
  return true;
}

static bool valid_utf8(const unsigned char *p) {
  while (*p) {
    if (*p < 0x80) p++;
    else if (*p >= 0xC2 && *p <= 0xDF &&
             p[1] >= 0x80 && p[1] <= 0xBF) p += 2;
    else if (*p >= 0xE0 && *p <= 0xEF && p[1] && p[2] &&
             p[1] >= (*p == 0xE0 ? 0xA0 : 0x80) &&
             p[1] <= (*p == 0xED ? 0x9F : 0xBF) &&
             p[2] >= 0x80 && p[2] <= 0xBF) p += 3;
    else if (*p >= 0xF0 && *p <= 0xF4 && p[1] && p[2] && p[3] &&
             p[1] >= (*p == 0xF0 ? 0x90 : 0x80) &&
             p[1] <= (*p == 0xF4 ? 0x8F : 0xBF) &&
             p[2] >= 0x80 && p[2] <= 0xBF &&
             p[3] >= 0x80 && p[3] <= 0xBF) p += 4;
    else return false;
  }
  return true;
}

static bool copy_context_field(const cJSON *root, const char *key, char *out,
                               size_t capacity) {
  const cJSON *field = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!field) {
    out[0] = '\0';
    return true;
  }
  if (!cJSON_IsString(field) || !field->valuestring ||
      strlen(field->valuestring) >= capacity ||
      strchr(field->valuestring, '\r') || strchr(field->valuestring, '\n') ||
      !valid_utf8((const unsigned char *)field->valuestring))
    return false;
  for (const unsigned char *p = (const unsigned char *)field->valuestring;
       *p; p++)
    if (*p < 0x20 || *p == 0x7f) return false;
  strcpy(out, field->valuestring);
  return true;
}

static bool parse_offer(const cJSON *root, IpcBrowserOffer *offer,
                        HostRequestContext *context, uint32_t *media_kind,
                        uint32_t *youtube_itag) {
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (!cJSON_IsString(type)) return false;
  *media_kind = IPC_BROWSER_MEDIA_NONE;
  *youtube_itag = 0;
  const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
  if (strcmp(type->valuestring, "media_offer") == 0) {
    if (!cJSON_IsString(kind)) return false;
    if (strcmp(kind->valuestring, "hls") == 0) *media_kind = IPC_BROWSER_MEDIA_HLS;
    else if (strcmp(kind->valuestring, "dash") == 0) *media_kind = IPC_BROWSER_MEDIA_DASH;
    else if (strcmp(kind->valuestring, "video") == 0) *media_kind = IPC_BROWSER_MEDIA_VIDEO;
    else return false;
  } else if (strcmp(type->valuestring, "download_offer") != 0 || kind) {
    return false;
  }
  memset(offer, 0, sizeof(*offer));
  memset(context, 0, sizeof(*context));
  if (!copy_field(root, "request_id", offer->request_id,
                  sizeof(offer->request_id), true) ||
      !copy_field(root, "url", offer->url, sizeof(offer->url), true) ||
      !copy_field(root, "filename", offer->filename,
                  sizeof(offer->filename), false) ||
      !copy_field(root, "mime", offer->mime, sizeof(offer->mime), false) ||
      !copy_field(root, "referrer", offer->referrer,
                  sizeof(offer->referrer), false) ||
      !copy_context_field(root, "cookie", context->cookie,
                          sizeof(context->cookie)) ||
      !copy_context_field(root, "user_agent", context->user_agent,
                          sizeof(context->user_agent)) ||
      !copy_context_field(root, "referer", context->referer,
                          sizeof(context->referer)) ||
      !copy_context_field(root, "youtube_sabr_url", context->youtube_sabr_url,
                          sizeof(context->youtube_sabr_url)) ||
      !copy_context_field(root, "youtube_request", context->youtube_request_b64,
                          sizeof(context->youtube_request_b64)))
    return false;
  if (cJSON_GetObjectItemCaseSensitive(root, "site_format_id") ||
      cJSON_GetObjectItemCaseSensitive(root, "site_format_label") ||
      cJSON_GetObjectItemCaseSensitive(root, "site_format_has_audio") ||
      cJSON_GetObjectItemCaseSensitive(root, "site_public_consent"))
    return false;
  const cJSON *youtube = cJSON_GetObjectItemCaseSensitive(root, "youtube_itag");
  if (youtube) {
    if (!cJSON_IsNumber(youtube) || youtube->valuedouble < 1 ||
        youtube->valuedouble > 100000 ||
        youtube->valuedouble != (double)(uint32_t)youtube->valuedouble ||
        *media_kind != IPC_BROWSER_MEDIA_VIDEO ||
        strncmp(offer->url, "https://www.youtube.com/watch?v=", 32) != 0)
      return false;
    *youtube_itag = (uint32_t)youtube->valuedouble;
  }
  const cJSON *height = cJSON_GetObjectItemCaseSensitive(root,
                                                         "youtube_height");
  if (height) {
    if (!cJSON_IsNumber(height) || height->valuedouble < 1 ||
        height->valuedouble > 4320 ||
        height->valuedouble != (double)(uint32_t)height->valuedouble)
      return false;
    context->youtube_height = (uint32_t)height->valuedouble;
  }
  if (!!context->youtube_sabr_url[0] != !!context->youtube_request_b64[0] ||
      (!!context->youtube_sabr_url[0] != !!context->youtube_height) ||
      (context->youtube_height && !*youtube_itag)) return false;
  const cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "total_bytes");
  if (total && (!cJSON_IsNumber(total) || total->valuedouble < 0 ||
                total->valuedouble > 9007199254740991.0))
    return false;
  if (total)
    offer->total_bytes = (uint64_t)total->valuedouble;
  if (strncmp(offer->url, "https://", 8) != 0 &&
      strncmp(offer->url, "http://", 7) != 0)
    return false;
  return true;
}

static void clear_context(HostRequestContext *context) {
  volatile unsigned char *bytes = (volatile unsigned char *)context;
  for (size_t i = 0; i < sizeof(*context); i++) bytes[i] = 0;
}

static void clear_context_fields(cJSON *root) {
  const char *keys[] = {"cookie", "user_agent", "referer",
                        "youtube_sabr_url", "youtube_request"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    cJSON *field = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
    if (cJSON_IsString(field) && field->valuestring) {
      size_t length = strlen(field->valuestring);
      volatile unsigned char *bytes =
          (volatile unsigned char *)field->valuestring;
      for (size_t j = 0; j < length; j++) bytes[j] = 0;
    }
  }
}

static char *context_offer_json(const IpcBrowserOffer *offer,
                                const HostRequestContext *context,
                                uint32_t media_kind, uint32_t youtube_itag) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;
  bool ok = cJSON_AddStringToObject(root, "request_id", offer->request_id) &&
            cJSON_AddStringToObject(root, "url", offer->url) &&
            cJSON_AddStringToObject(root, "filename", offer->filename) &&
            cJSON_AddStringToObject(root, "mime", offer->mime) &&
            cJSON_AddStringToObject(root, "referrer", offer->referrer) &&
            cJSON_AddNumberToObject(root, "total_bytes", (double)offer->total_bytes) &&
            cJSON_AddStringToObject(root, "cookie", context->cookie) &&
            cJSON_AddStringToObject(root, "user_agent", context->user_agent) &&
            cJSON_AddStringToObject(root, "referer", context->referer);
  if (media_kind) {
    const char *kind = media_kind == IPC_BROWSER_MEDIA_HLS ? "hls" :
                       media_kind == IPC_BROWSER_MEDIA_DASH ? "dash" : "video";
    ok = ok && cJSON_AddStringToObject(root, "kind", kind);
  }
  if (youtube_itag)
    ok = ok && cJSON_AddNumberToObject(root, "youtube_itag",
                                       youtube_itag);
  if (context->youtube_height)
    ok = ok && cJSON_AddNumberToObject(root, "youtube_height",
                                       context->youtube_height) &&
         cJSON_AddStringToObject(root, "youtube_sabr_url",
                                 context->youtube_sabr_url) &&
         cJSON_AddStringToObject(root, "youtube_request",
                                 context->youtube_request_b64);
  char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
  clear_context_fields(root);
  cJSON_Delete(root);
  return json;
}

static void clear_json(char *json) {
  if (!json) return;
  size_t length = strlen(json);
  volatile unsigned char *bytes = (volatile unsigned char *)json;
  for (size_t i = 0; i < length; i++) bytes[i] = 0;
  cJSON_free(json);
}

static int forward_context_offer(int daemon, MsgType type, const char *json,
                                 IpcBrowserOffer *registered) {
  size_t length = strlen(json);
  int result = -1;
  if (length <= IPC_MAX_FRAME_SIZE) {
    MsgHeader header = {.length = (uint32_t)length, .type = type};
    if (ipc_write_exact(daemon, &header, sizeof(header)) == 0 &&
        ipc_write_exact(daemon, json, length) == 0 &&
        ipc_read_exact(daemon, registered, sizeof(*registered)) == 0 &&
        registered->offer_id)
      result = 0;
  }
  return result;
}

static bool send_error(const char *request_id, const char *message) {
  cJSON *reply = cJSON_CreateObject();
  if (!reply)
    return false;
  cJSON_AddStringToObject(reply, "type", "error");
  cJSON_AddStringToObject(reply, "request_id", request_id ? request_id : "");
  cJSON_AddStringToObject(reply, "error", message);
  bool ok = native_send(reply);
  cJSON_Delete(reply);
  return ok;
}

static bool normalize_site(const char *input, char out[256]) {
  size_t length = strlen(input);
  if (!length || length >= 256) return false;
  for (size_t i = 0; i <= length; i++)
    out[i] = (char)tolower((unsigned char)input[i]);
  if (out[length - 1] == '.') out[--length] = '\0';
  const char *host = strncmp(out, "*.", 2) == 0 ? out + 2 : out;
  if (!*host || strlen(host) > 253) return false;
  size_t label_length = 0;
  for (const char *p = host;; p++) {
    if (*p == '.' || !*p) {
      if (!label_length || label_length > 63 || p[-1] == '-') return false;
      label_length = 0;
      if (!*p) return true;
    } else {
      if ((*p < 'a' || *p > 'z') && (*p < '0' || *p > '9') && *p != '-')
        return false;
      if (!label_length && *p == '-') return false;
      label_length++;
    }
  }
}

static bool configure_exclusions(const cJSON *root) {
  const cJSON *sites = cJSON_GetObjectItemCaseSensitive(root, "sites");
  int count = cJSON_GetArraySize(sites);
  if (!cJSON_IsArray(sites) || count > HOST_MAX_EXCLUSIONS)
    return send_error(NULL, tr("host.error.invalid_site_exclusions"));
  char replacement[HOST_MAX_EXCLUSIONS][256] = {{0}};
  for (int i = 0; i < count; i++) {
    const cJSON *site = cJSON_GetArrayItem(sites, i);
    if (!cJSON_IsString(site) || !site->valuestring ||
        !normalize_site(site->valuestring, replacement[i]))
      return send_error(NULL, tr("host.error.invalid_excluded_hostname"));
  }
  memcpy(g_excluded_sites, replacement, sizeof(replacement));
  g_excluded_count = (size_t)count;
  cJSON *reply = cJSON_CreateObject();
  if (!reply) return false;
  cJSON_AddStringToObject(reply, "type", "site_exclusions_set");
  bool ok = native_send(reply);
  cJSON_Delete(reply);
  return ok;
}

/* -1 invalid URL/unsupported IDN, 0 allowed, 1 excluded. */
static int site_excluded(const char *url) {
  CURLU *parsed = curl_url();
  if (!parsed) return -1;
  char *hostname = NULL;
  unsigned int flags = 0;
#if LIBCURL_VERSION_NUM >= 0x075800
  flags = CURLU_PUNYCODE;
#endif
  bool valid = curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
               curl_url_get(parsed, CURLUPART_HOST, &hostname, flags) == CURLUE_OK;
  int result = valid ? 0 : -1;
  char host[256] = {0};
  if (valid) {
    size_t length = strlen(hostname);
    if (length >= sizeof(host)) result = -1;
    else {
      for (size_t i = 0; i <= length; i++)
        host[i] = (char)tolower((unsigned char)hostname[i]);
      if (length && host[length - 1] == '.') host[--length] = '\0';
      for (size_t i = 0; i < g_excluded_count; i++) {
        const char *pattern = g_excluded_sites[i];
        bool wildcard = strncmp(pattern, "*.", 2) == 0;
        const char *base = wildcard ? pattern + 2 : pattern;
        size_t base_length = strlen(base);
        if (strcmp(host, base) == 0 ||
            (wildcard && length > base_length &&
             host[length - base_length - 1] == '.' &&
             strcmp(host + length - base_length, base) == 0)) {
          result = 1;
          break;
        }
      }
    }
  }
  curl_free(hostname);
  curl_url_cleanup(parsed);
  return result;
}

static bool send_skipped(const char *request_id) {
  cJSON *reply = cJSON_CreateObject();
  if (!reply) return false;
  cJSON_AddStringToObject(reply, "type", "offer_skipped");
  cJSON_AddStringToObject(reply, "request_id", request_id);
  bool ok = native_send(reply);
  cJSON_Delete(reply);
  return ok;
}

static bool send_registered(const IpcBrowserOffer *offer) {
  cJSON *reply = cJSON_CreateObject();
  if (!reply)
    return false;
  cJSON_AddStringToObject(reply, "type", "offer_registered");
  cJSON_AddStringToObject(reply, "request_id", offer->request_id);
  cJSON_AddNumberToObject(reply, "offer_id", offer->offer_id);
  bool ok = native_send(reply);
  cJSON_Delete(reply);
  return ok;
}

static const char *state_name(IpcBrowserOfferState state) {
  switch (state) {
  case IPC_BROWSER_WAITING: return "waiting";
  case IPC_BROWSER_CONFIRMED: return "confirmed";
  case IPC_BROWSER_DISMISSED: return "dismissed";
  }
  return "error";
}

static bool send_state(const IpcBrowserOffer *offer,
                       const IpcBrowserProgress *progress) {
  cJSON *reply = cJSON_CreateObject();
  if (!reply)
    return false;
  const char *state = state_name(offer->state);
  if (progress) {
    if (strcmp(progress->status, "DONE") == 0) state = "complete";
    else if (strcmp(progress->status, "ERROR") == 0) state = "error";
    else if (strcmp(progress->status, "CANCELED") == 0) state = "canceled";
    else if (strcmp(progress->status, "ACTIVE") == 0) state = "started";
  }
  cJSON_AddStringToObject(reply, "type", "offer_state");
  cJSON_AddStringToObject(reply, "request_id", offer->request_id);
  cJSON_AddStringToObject(reply, "state", state);
  cJSON_AddNumberToObject(reply, "offer_id", offer->offer_id);
  cJSON_AddNumberToObject(reply, "download_id", offer->download_id);
  if (progress) {
    cJSON_AddNumberToObject(reply, "progress", progress->progress);
    cJSON_AddNumberToObject(reply, "bytes_received",
                            (double)progress->bytes_received);
    cJSON_AddNumberToObject(reply, "total_bytes", (double)progress->total_bytes);
    if (progress->error[0])
      cJSON_AddStringToObject(reply, "error", progress->error);
  }
  bool ok = native_send(reply);
  cJSON_Delete(reply);
  return ok;
}

static HostOffer *find_offer_slot(const char *request_id) {
  for (size_t i = 0; i < HOST_MAX_OFFERS; i++)
    if (g_offers[i].offer.offer_id &&
        strcmp(g_offers[i].offer.request_id, request_id) == 0)
      return &g_offers[i];
  for (size_t i = 0; i < HOST_MAX_OFFERS; i++)
    if (!g_offers[i].offer.offer_id)
      return &g_offers[i];
  return NULL;
}

static bool handle_message(const char *json) {
  /* cJSON string values have no separate length; reject escaped NUL first. */
  for (const char *p = json; *p; p++) {
    if (*p == '\\' && p[1]) {
      if (p[1] == 'u' && strncmp(p + 2, "0000", 4) == 0)
        return send_error(NULL, tr("host.error.invalid_offer"));
      p++; // An escaped backslash does not start a Unicode escape.
    }
  }
  cJSON *root = cJSON_Parse(json);
  if (!root)
    return send_error(NULL, tr("host.error.invalid_json"));
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (cJSON_IsString(type) && strcmp(type->valuestring, "set_site_exclusions") == 0) {
    bool ok = configure_exclusions(root);
    cJSON_Delete(root);
    return ok;
  }
  if (cJSON_IsString(type) && strcmp(type->valuestring, "site_probe") == 0) {
    char request_id[128] = {0};
    bool valid_id = copy_field(root, "request_id", request_id,
                               sizeof(request_id), true);
    for (const unsigned char *p = (const unsigned char *)request_id; *p; p++)
      if (*p < 0x20 || *p == 0x7f) valid_id = false;
    bool ok = send_error(valid_id ? request_id : NULL,
                         "site downloads are not supported");
    cJSON_Delete(root);
    return ok;
  }
  IpcBrowserOffer offered = {0};
  HostRequestContext context = {0};
  uint32_t media_kind = IPC_BROWSER_MEDIA_NONE;
  uint32_t youtube_itag = 0;
  if (!parse_offer(root, &offered, &context, &media_kind,
                   &youtube_itag)) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    bool ok = send_error(cJSON_IsString(id) ? id->valuestring : NULL,
                         tr("host.error.invalid_offer"));
    clear_context(&context);
    clear_context_fields(root);
    cJSON_Delete(root);
    return ok;
  }
  const cJSON *automatic = cJSON_GetObjectItemCaseSensitive(root, "automatic");
  int excluded = site_excluded(offered.url);
  if ((automatic && !cJSON_IsBool(automatic)) || excluded < 0 ||
      (excluded && (!automatic || cJSON_IsTrue(automatic)))) {
    bool ok = excluded == 1 && (!automatic || cJSON_IsTrue(automatic))
                  ? send_skipped(offered.request_id)
                  : send_error(offered.request_id, tr("host.error.invalid_url_or_mode"));
    clear_context(&context);
    clear_context_fields(root);
    cJSON_Delete(root);
    return ok;
  }
  clear_context_fields(root);
  cJSON_Delete(root);

  bool has_context = context.cookie[0] || context.user_agent[0] ||
                     context.referer[0];
  bool has_youtube_session = context.youtube_height != 0;
  bool use_json = has_context || media_kind || youtube_itag;
  char *context_json = use_json
      ? context_offer_json(&offered, &context, media_kind,
                           youtube_itag) : NULL;
  clear_context(&context);
  if (use_json) {
    /* Check the bounded daemon frame before starting or contacting it. */
    if (!context_json || strlen(context_json) > IPC_MAX_FRAME_SIZE) {
      clear_json(context_json);
      return send_error(offered.request_id, tr("host.error.invalid_or_oversized_offer"));
    }
  }

  HostOffer *slot = find_offer_slot(offered.request_id);
  if (!slot) {
    clear_json(context_json);
    return send_error(offered.request_id, tr("host.error.too_many_pending"));
  }
  if (!ensure_daemon_running()) {
    clear_json(context_json);
    return send_error(offered.request_id, tr("host.error.daemon_start"));
  }
  uint16_t daemon_version = 1;
  int daemon = ipc_client_connect_compatible(1500, &daemon_version);
  if (daemon < 0) {
    clear_json(context_json);
    return send_error(offered.request_id, tr("host.error.daemon_unavailable"));
  }
  if (media_kind && daemon_version < 10) {
    ipc_client_disconnect(daemon);
    clear_json(context_json);
    return send_error(offered.request_id, tr("host.error.media_unsupported"));
  }
  if (youtube_itag && daemon_version < 16) {
    ipc_client_disconnect(daemon);
    clear_json(context_json);
    return send_error(offered.request_id, "Update cdm daemon for YouTube formats");
  }
  if (has_youtube_session && daemon_version < 17) {
    ipc_client_disconnect(daemon);
    clear_json(context_json);
    return send_error(offered.request_id, "Update cdm daemon for browser playback");
  }
  if (has_context && daemon_version < 8) {
    ipc_client_disconnect(daemon);
    clear_json(context_json);
    return send_error(offered.request_id, tr("host.error.context_unsupported"));
  }
  IpcBrowserOffer registered = {0};
  int result = use_json ? forward_context_offer(daemon,
      has_youtube_session ? MSG_BROWSER_OFFER_YOUTUBE_V2 :
      youtube_itag ? MSG_BROWSER_OFFER_YOUTUBE_V1 : MSG_BROWSER_OFFER_V2,
      context_json, &registered)
                           : ipc_browser_offer(daemon, &offered, &registered);
  clear_json(context_json);
  ipc_client_disconnect(daemon);
  if (result != 0)
    return send_error(offered.request_id, tr("host.error.offer_rejected"));
  bool launch = slot->offer.offer_id != registered.offer_id &&
                registered.state == IPC_BROWSER_WAITING;
  slot->offer = registered;
  slot->reported_state = registered.state;
  slot->reported_bytes = 0;
  slot->reported_status[0] = '\0';
  if (!send_registered(&registered))
    return false;
  if (launch) {
    char popup_path[1024];
    if (!cdm_executable(popup_path, sizeof(popup_path))) {
      return send_error(offered.request_id, tr("host.error.popup_missing"));
    }
    if (spawn_browser_popup_detached(popup_path, registered.offer_id) != 0)
      return send_error(offered.request_id, tr("host.error.popup_launch"));
  }
  return true;
}

static bool poll_offers(void) {
  bool has_offers = false;
  for (size_t i = 0; i < HOST_MAX_OFFERS; i++)
    has_offers |= g_offers[i].offer.offer_id != 0;
  if (!has_offers)
    return true;
  int daemon = ipc_client_connect_compatible(500, NULL);
  if (daemon < 0) {
    bool notified = true;
    for (size_t i = 0; i < HOST_MAX_OFFERS; i++) {
      if (g_offers[i].offer.offer_id) {
        notified = send_error(g_offers[i].offer.request_id,
                              tr("host.error.daemon_disconnected")) && notified;
        memset(&g_offers[i], 0, sizeof(g_offers[i]));
      }
    }
    return notified;
  }
  bool ok = true;
  for (size_t i = 0; i < HOST_MAX_OFFERS && ok; i++) {
    HostOffer *slot = &g_offers[i];
    if (!slot->offer.offer_id)
      continue;
    IpcBrowserOffer current = {0};
    if (ipc_browser_get_offer(daemon, slot->offer.offer_id, &current) != 0) {
      ok = send_error(slot->offer.request_id, tr("host.error.offer_expired"));
      memset(slot, 0, sizeof(*slot));
      continue;
    }
    if (current.state != slot->reported_state) {
      ok = send_state(&current, NULL);
      slot->reported_state = current.state;
    }
    slot->offer = current;
    if (current.state == IPC_BROWSER_DISMISSED) {
      memset(slot, 0, sizeof(*slot));
      continue;
    }
    if (current.state != IPC_BROWSER_CONFIRMED || !current.download_id)
      continue;
    IpcBrowserProgress snapshot = {0};
    int progress_sock = ipc_client_connect_compatible(500, NULL);
    if (progress_sock < 0)
      continue;
    int progress_result = ipc_browser_subscribe_progress(
        progress_sock, current.download_id, &snapshot);
    ipc_client_disconnect(progress_sock);
    if (progress_result != 0)
      continue;
    if (snapshot.bytes_received != slot->reported_bytes ||
        strcmp(snapshot.status, slot->reported_status) != 0) {
      ok = send_state(&current, &snapshot);
      slot->reported_bytes = snapshot.bytes_received;
      snprintf(slot->reported_status, sizeof(slot->reported_status), "%s",
               snapshot.status);
    }
    if (strcmp(snapshot.status, "DONE") == 0 ||
        strcmp(snapshot.status, "CANCELED") == 0 ||
        strcmp(snapshot.status, "ERROR") == 0)
      memset(slot, 0, sizeof(*slot));
  }
  ipc_client_disconnect(daemon);
  return ok;
}

int main(void) {
  log_init(NULL, LOG_WARN); /* Native Messaging reserves stdout for JSON. */
  config_init(NULL);
  DownloadManagerConfig locale_config;
  config_get(&locale_config);
  tr_load_locale(locale_config.ui_locale);
  setvbuf(stdin, NULL, _IONBF, 0);
  for (;;) {
    fd_set input;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    int ready = select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return 1;
    }
    if (ready > 0 && FD_ISSET(STDIN_FILENO, &input)) {
      char *json = NULL;
      int read_result = native_read(&json);
      if (read_result == 0) return 0;
      if (read_result < 0) {
        LOG_WARN("Malformed or truncated native messaging frame");
        return 1;
      }
      bool ok = handle_message(json);
      size_t length = strlen(json);
      volatile unsigned char *bytes = (volatile unsigned char *)json;
      for (size_t i = 0; i < length; i++) bytes[i] = 0;
      free(json);
      if (!ok) return 1;
    }
    if (!poll_offers()) return 1;
  }
}
