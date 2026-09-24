// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "../platform/ipc_socket.h"
#include "../platform/spawn.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../vendor/cJSON.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define NATIVE_MAX_MESSAGE (1024 * 1024)
#define HOST_MAX_OFFERS 64

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

static bool parse_offer(const cJSON *root, IpcBrowserOffer *offer) {
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (!cJSON_IsString(type) || strcmp(type->valuestring, "download_offer") != 0)
    return false;
  memset(offer, 0, sizeof(*offer));
  if (!copy_field(root, "request_id", offer->request_id,
                  sizeof(offer->request_id), true) ||
      !copy_field(root, "url", offer->url, sizeof(offer->url), true) ||
      !copy_field(root, "filename", offer->filename,
                  sizeof(offer->filename), false) ||
      !copy_field(root, "mime", offer->mime, sizeof(offer->mime), false) ||
      !copy_field(root, "referrer", offer->referrer,
                  sizeof(offer->referrer), false))
    return false;
  const cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "total_bytes");
  if (total && (!cJSON_IsNumber(total) || total->valuedouble < 0 ||
                total->valuedouble > 9007199254740991.0))
    return false;
  if (total)
    offer->total_bytes = (uint64_t)total->valuedouble;
  return true;
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
    else if (strcmp(progress->status, "CANCELED") == 0) state = "error";
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
  cJSON *root = cJSON_Parse(json);
  if (!root)
    return send_error(NULL, "invalid JSON");
  IpcBrowserOffer offered = {0};
  if (!parse_offer(root, &offered)) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    bool ok = send_error(cJSON_IsString(id) ? id->valuestring : NULL,
                         "invalid or unsupported download offer");
    cJSON_Delete(root);
    return ok;
  }
  cJSON_Delete(root);

  HostOffer *slot = find_offer_slot(offered.request_id);
  if (!slot)
    return send_error(offered.request_id, "too many pending downloads");
  if (!ensure_daemon_running())
    return send_error(offered.request_id, "cdm daemon could not start");
  int daemon = ipc_client_connect_compatible(1500, NULL);
  if (daemon < 0)
    return send_error(offered.request_id, "cdm daemon is unavailable");
  IpcBrowserOffer registered = {0};
  int result = ipc_browser_offer(daemon, &offered, &registered);
  ipc_client_disconnect(daemon);
  if (result != 0)
    return send_error(offered.request_id, "daemon rejected download offer");
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
      return send_error(offered.request_id, "cannot locate cdm popup binary");
    }
    if (spawn_browser_popup_detached(popup_path, registered.offer_id) != 0)
      return send_error(offered.request_id, "could not launch cdm popup");
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
                              "cdm daemon disconnected") && notified;
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
      ok = send_error(slot->offer.request_id, "download offer expired");
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
      free(json);
      if (!ok) return 1;
    }
    if (!poll_offers()) return 1;
  }
}
