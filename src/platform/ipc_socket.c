// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "ipc_socket.h"

#include "../core/queue_manager.h"
#include "../persistence/db.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../utils/path.h"
#include "../vendor/cJSON.h"
#include "file_io.h"
#include "thread.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

/* Constants */
#define MAX_CLIENTS 16
#define IPC_POLL_TIMEOUT_US 20000 /* 20 ms */
#define MAX_BROWSER_OFFERS 64
#define BROWSER_OFFER_TTL_SECONDS 600

static bool join_path(char *out, size_t out_size, const char *base,
                      const char *suffix) {
  size_t base_len = strlen(base);
  size_t suffix_len = strlen(suffix);
  if (base_len + suffix_len + 1 > out_size)
    return false;
  memcpy(out, base, base_len);
  memcpy(out + base_len, suffix, suffix_len + 1);
  return true;
}

typedef enum { IPC_BASE_RUNTIME, IPC_BASE_HOME, IPC_BASE_TMP } IpcBaseKind;

static bool socket_path_fits(const char *base, const char *suffix) {
  size_t capacity = sizeof(((struct sockaddr_un *)0)->sun_path);
  size_t base_len = strlen(base);
  return base_len < capacity && strlen(suffix) < capacity - base_len;
}

static bool format_tmp_path(char *out, size_t size, const char *extension) {
  int written = snprintf(out, size, "/tmp/cdm_%u.%s", (unsigned)getuid(),
                         extension);
  return written >= 0 && (size_t)written < size;
}

/* Socket and lock must make the same runtime/home/tmp choice. */
static int get_ipc_base(char *out, size_t size, IpcBaseKind *kind) {
  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
  const char *home = getenv("HOME");
  if (runtime_dir && runtime_dir[0] == '/' &&
      socket_path_fits(runtime_dir, "/cdm.sock")) {
    *kind = IPC_BASE_RUNTIME;
    int written = snprintf(out, size, "%s", runtime_dir);
    return written >= 0 && (size_t)written < size ? 0 : -1;
  }
  if (home && home[0] == '/' &&
      socket_path_fits(home, "/.local/share/cdm/ipc.sock")) {
    *kind = IPC_BASE_HOME;
    return join_path(out, size, home, "/.local/share") ? 0 : -1;
  }
  *kind = IPC_BASE_TMP;
  return join_path(out, size, "", "/tmp") ? 0 : -1;
}

/** Get user-isolated IPC socket path (XDG_RUNTIME_DIR,
 * ~/.local/share/cdm, or /tmp/cdm_UID.sock). */
static void get_socket_path(char *out, size_t out_size) {
  static char cached_path[1024] = {0};
  if (cached_path[0] != '\0') {
    strncpy(out, cached_path, out_size - 1);
    out[out_size - 1] = '\0';
    return;
  }

  char base[2048];
  IpcBaseKind kind;
  if (get_ipc_base(base, sizeof(base), &kind) != 0) {
    out[0] = '\0';
    return;
  }
  bool formatted = false;
  if (kind == IPC_BASE_RUNTIME)
    formatted = join_path(cached_path, sizeof(cached_path), base, "/cdm.sock");
  else if (kind == IPC_BASE_HOME)
    /* Probing clients must not create the new directory before migration. */
    formatted = join_path(cached_path, sizeof(cached_path), base,
                          "/cdm/ipc.sock");
  else
    formatted = format_tmp_path(cached_path, sizeof(cached_path), "sock");

  if (!formatted) {
    out[0] = '\0';
    return;
  }

  strncpy(out, cached_path, out_size - 1);
  out[out_size - 1] = '\0';
}

int ipc_daemon_lock_acquire(int *fd_out) {
  if (!fd_out)
    return -1;
  *fd_out = -1;
  char path[2048];
  char base[2048];
  IpcBaseKind kind;
  if (get_ipc_base(base, sizeof(base), &kind) != 0 ||
      (kind == IPC_BASE_HOME && file_ensure_directory(base) != 0))
    return -1;
  if (kind == IPC_BASE_TMP) {
    if (!format_tmp_path(path, sizeof(path), "lock"))
      return -1;
  }
  else if (!join_path(path, sizeof(path), base, "/cdm.lock"))
    return -1;
  int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    LOG_ERROR("Daemon lock: open %s: %s", path, strerror(errno));
    return -1;
  }
  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_uid != getuid()) {
    LOG_ERROR("Daemon lock: unsafe file at %s", path);
    close(fd);
    return -1;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int saved_errno = errno;
    close(fd);
    if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN)
      return 1;
    LOG_ERROR("Daemon lock: flock %s: %s", path, strerror(saved_errno));
    return -1;
  }
  if (fchmod(fd, 0600) != 0) {
    LOG_ERROR("Daemon lock: chmod %s: %s", path, strerror(errno));
    close(fd);
    return -1;
  }
  *fd_out = fd;
  return 0;
}

/* Server state */
static int g_listen_fd = -1;
static int g_client_fds[MAX_CLIENTS];
static int g_client_count = 0;
static bool g_client_subscribed[MAX_CLIENTS];
static bool g_client_v2_subscribed[MAX_CLIENTS];
static uint32_t g_client_browser_download[MAX_CLIENTS];
static unsigned char g_client_header[MAX_CLIENTS][sizeof(MsgHeader)];
static size_t g_client_header_bytes[MAX_CLIENTS];
static dm_mutex_t g_client_mutex;
static once_flag g_client_mutex_once = ONCE_FLAG_INIT;

typedef struct {
  IpcBrowserOffer offer;
  time_t touched_at;
} BrowserOfferSlot;

/* Only the daemon's IPC poll thread reads or mutates these slots. */
static BrowserOfferSlot g_browser_offers[MAX_BROWSER_OFFERS];
static const char *status_to_string(DownloadStatus s);
static float snapshot_progress(const DownloadRuntimeSnapshot *snapshot);

static void browser_expire_offers(void) {
  time_t now = time(NULL);
  for (size_t i = 0; i < MAX_BROWSER_OFFERS; i++) {
    BrowserOfferSlot *slot = &g_browser_offers[i];
    if (!slot->offer.offer_id)
      continue;
    if (now - slot->touched_at <= BROWSER_OFFER_TTL_SECONDS)
      continue;
    if (slot->offer.state == IPC_BROWSER_CONFIRMED) {
      DownloadStatus status;
      if (queue_manager_get_status(slot->offer.download_id, &status) &&
          status != DOWNLOAD_DONE && status != DOWNLOAD_ERROR &&
          status != DOWNLOAD_CANCELED)
        continue;
    }
    memset(slot, 0, sizeof(*slot));
  }
}

static BrowserOfferSlot *browser_find_offer(uint32_t id) {
  for (size_t i = 0; i < MAX_BROWSER_OFFERS; i++)
    if (id && g_browser_offers[i].offer.offer_id == id)
      return &g_browser_offers[i];
  return NULL;
}

static BrowserOfferSlot *browser_find_request(const char *request_id) {
  for (size_t i = 0; i < MAX_BROWSER_OFFERS; i++)
    if (g_browser_offers[i].offer.offer_id &&
        strcmp(g_browser_offers[i].offer.request_id, request_id) == 0)
      return &g_browser_offers[i];
  return NULL;
}

static uint32_t browser_random_id(void) {
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    return 0;
  uint32_t id = 0;
  ssize_t n = read(fd, &id, sizeof(id));
  close(fd);
  return n == (ssize_t)sizeof(id) ? id : 0;
}

static bool browser_json_string(const cJSON *root, const char *key,
                                char *out, size_t capacity, bool required) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!value && !required) {
    out[0] = '\0';
    return true;
  }
  if (!cJSON_IsString(value) || !value->valuestring ||
      (required && !value->valuestring[0]) ||
      strlen(value->valuestring) >= capacity)
    return false;
  strcpy(out, value->valuestring);
  return true;
}

static bool browser_parse_offer(const char *json, IpcBrowserOffer *out) {
  cJSON *root = cJSON_Parse(json);
  if (!root)
    return false;
  memset(out, 0, sizeof(*out));
  bool valid = browser_json_string(root, "request_id", out->request_id,
                                   sizeof(out->request_id), true) &&
               browser_json_string(root, "url", out->url,
                                   sizeof(out->url), true) &&
               browser_json_string(root, "filename", out->filename,
                                   sizeof(out->filename), false) &&
               browser_json_string(root, "mime", out->mime,
                                   sizeof(out->mime), false) &&
               browser_json_string(root, "referrer", out->referrer,
                                   sizeof(out->referrer), false);
  const cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "total_bytes");
  if (total) {
    if (!cJSON_IsNumber(total) || total->valuedouble < 0 ||
        total->valuedouble > 9007199254740991.0)
      valid = false;
    else
      out->total_bytes = (uint64_t)total->valuedouble;
  }
  if (valid && strncmp(out->url, "https://", 8) != 0 &&
      strncmp(out->url, "http://", 7) != 0)
    valid = false;
  if (valid && out->filename[0] == '\0')
    path_filename_from_url(out->url, out->filename, sizeof(out->filename));
  if (valid && (strchr(out->filename, '/') || strchr(out->filename, '\\') ||
                strcmp(out->filename, ".") == 0 ||
                strcmp(out->filename, "..") == 0))
    valid = false;
  cJSON_Delete(root);
  return valid;
}

static IpcBrowserProgress browser_progress_snapshot(uint32_t id,
                                                    const char *error) {
  IpcBrowserProgress event = {.download_id = id};
  DownloadRuntimeSnapshot snapshot = {0};
  if (!queue_manager_get_runtime_snapshot(id, &snapshot)) {
    snprintf(event.status, sizeof(event.status), "NOT_FOUND");
    return event;
  }
  snprintf(event.status, sizeof(event.status), "%s",
           status_to_string(snapshot.status));
  event.total_bytes = snapshot.total_size;
  memcpy(event.dest_path, snapshot.dest_path, sizeof(event.dest_path));
  if (snapshot.status == DOWNLOAD_ACTIVE)
    event.bytes_received = snapshot.bytes_downloaded;
  else if (snapshot.status == DOWNLOAD_DONE)
    event.bytes_received = snapshot.total_size ? snapshot.total_size
                                               : snapshot.bytes_downloaded;
  else {
    for (int i = 0; i < snapshot.chunk_count; i++)
      event.bytes_received += snapshot.chunks[i].bytes_done;
  }
  event.progress = snapshot_progress(&snapshot);
  if (snapshot.status == DOWNLOAD_DONE)
    event.progress = 1.0f;
  if (error)
    snprintf(event.error, sizeof(event.error), "%s", error);
  return event;
}

static void initialize_client_mutex(void) { dm_mutex_init(&g_client_mutex); }

/* Internal helpers */

static const char *status_to_string(DownloadStatus s) {
  switch (s) {
  case DOWNLOAD_QUEUED:
    return "QUEUED";
  case DOWNLOAD_ACTIVE:
    return "ACTIVE";
  case DOWNLOAD_PAUSED:
    return "PAUSED";
  case DOWNLOAD_DONE:
    return "DONE";
  case DOWNLOAD_ERROR:
    return "ERROR";
  case DOWNLOAD_CANCELED:
    return "CANCELED";
  default:
    return "UNKNOWN";
  }
}

/** Send a length‑prefixed string. */
static void write_string(int fd, const char *s) {
  uint32_t len = s ? (uint32_t)strlen(s) : 0;
  ipc_write_exact(fd, &len, sizeof(len));
  if (len > 0)
    ipc_write_exact(fd, s, len);
}

/**
 * Receive a length‑prefixed string (max `max_len` bytes including NUL).
 *
 * If the incoming string length exceeds `max_len - 1`, all bytes are drained
 * from the wire so that subsequent messages remain synchronized, but the
 * field is rejected instead of silently truncated.
 *
 * @return 0 on success, -1 on read error or connection closed.
 */
static int read_string(int fd, char *out, size_t max_len) {
  if (!out || max_len == 0)
    return -1;

  uint32_t len = 0;
  if (ipc_read_exact(fd, &len, sizeof(len)) != 0) {
    out[0] = '\0';
    return -1;
  }

  size_t to_read = (len >= max_len) ? (max_len - 1) : (size_t)len;
  if (to_read > 0) {
    if (ipc_read_exact(fd, out, to_read) != 0) {
      out[0] = '\0';
      return -1;
    }
  }
  out[to_read] = '\0';

  bool oversized = (size_t)len >= max_len;
  if (oversized) {
    size_t excess = (size_t)len - to_read;
    char dummy[256];
    while (excess > 0) {
      size_t chunk = (excess < sizeof(dummy)) ? excess : sizeof(dummy);
      if (ipc_read_exact(fd, dummy, chunk) != 0)
        return -1;
      excess -= chunk;
    }
  }

  return oversized ? -1 : 0;
}

/** Remove a client from the array and compact. */
static void remove_client(int index) {
  close(g_client_fds[index]);
  for (int i = index; i < g_client_count - 1; i++) {
    g_client_fds[i] = g_client_fds[i + 1];
    g_client_subscribed[i] = g_client_subscribed[i + 1];
    g_client_v2_subscribed[i] = g_client_v2_subscribed[i + 1];
    g_client_browser_download[i] = g_client_browser_download[i + 1];
    g_client_header_bytes[i] = g_client_header_bytes[i + 1];
    memcpy(g_client_header[i], g_client_header[i + 1], sizeof(MsgHeader));
  }
  g_client_fds[g_client_count - 1] = -1;
  g_client_subscribed[g_client_count - 1] = false;
  g_client_v2_subscribed[g_client_count - 1] = false;
  g_client_browser_download[g_client_count - 1] = 0;
  g_client_header_bytes[g_client_count - 1] = 0;
  g_client_count--;
}

static bool valid_message_header(const MsgHeader *header) {
  if (header->length > IPC_MAX_FRAME_SIZE)
    return false;

  switch (header->type) {
  case MSG_ADD_DOWNLOAD:
  case MSG_ADD_DOWNLOAD_AUTO:
  case MSG_BROWSER_OFFER:
    return header->length <= IPC_MAX_FRAME_SIZE;
  case MSG_PAUSE:
  case MSG_RESUME:
  case MSG_CANCEL:
  case MSG_GET_DETAILS:
  case MSG_BROWSER_GET_OFFER:
  case MSG_BROWSER_DISMISS:
  case MSG_BROWSER_SUBSCRIBE_PROGRESS:
    return header->length == sizeof(uint32_t);
  case MSG_BROWSER_CONFIRM:
    return header->length >= sizeof(uint32_t) * 2 &&
           header->length <= sizeof(uint32_t) * 2 + IPC_MAX_PATH_LEN - 1;
  case MSG_LIST_PAGE:
    return header->length == sizeof(uint32_t) * 2;
  case MSG_LIST:
  case MSG_LIST_ALL:
  case MSG_SUBSCRIBE:
  case MSG_SUBSCRIBE_V2:
  case MSG_RELOAD_CONFIG:
  case MSG_HELLO:
    return header->length == 0;
  default:
    return false;
  }
}

typedef struct {
  int client_fd;
} ListResponseContext;

typedef struct {
  DbDownloadRow *rows;
  int count;
} PageCollectContext;

static int collect_download_row(const DbDownloadRow *row, void *ctx) {
  PageCollectContext *page = ctx;
  page->rows[page->count++] = *row;
  return 0;
}

static float snapshot_progress(const DownloadRuntimeSnapshot *snapshot) {
  if (snapshot->total_size == 0)
    return 0.0f;

  uint64_t done =
      snapshot->status == DOWNLOAD_ACTIVE ? snapshot->bytes_downloaded : 0;
  if (snapshot->status != DOWNLOAD_ACTIVE) {
    for (int i = 0; i < snapshot->chunk_count; i++)
      done += snapshot->chunks[i].bytes_done;
  }
  double fraction = (double)done / (double)snapshot->total_size;
  return (float)(fraction > 1.0 ? 1.0 : fraction);
}

static int send_download_row(const DbDownloadRow *row, void *ctx) {
  ListResponseContext *response = (ListResponseContext *)ctx;
  char status[16];
  float progress = 0.0f;
  DownloadRuntimeSnapshot snapshot;
  bool live = queue_manager_get_runtime_snapshot(row->id, &snapshot);

  if (live) {
    strncpy(status, status_to_string(snapshot.status), sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
    progress = snapshot_progress(&snapshot);
  } else {
    strncpy(status, row->status, sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
  }

  if (strcmp(status, "DONE") == 0)
    progress = 1.0f;

  if (ipc_write_exact(response->client_fd, &row->id, sizeof(row->id)) != 0)
    return -1;
  write_string(response->client_fd, row->url);
  write_string(response->client_fd, row->dest_path);
  write_string(response->client_fd, status);
  return ipc_write_exact(response->client_fd, &progress, sizeof(progress));
}

static void send_command_result(int client_fd, IpcResult result) {
  uint8_t wire_result = (uint8_t)result;
  ipc_write_exact(client_fd, &wire_result, sizeof(wire_result));
}

static uint32_t reserve_download(const char *url, const char *dest,
                                 const RequestOptions *opts,
                                 bool auto_filename) {
  char unique_dest[IPC_MAX_PATH_LEN];
  uint32_t id = 0;
  for (unsigned int attempt = 0; attempt < 1000000; attempt++) {
    if (!path_make_unique(dest, unique_dest, sizeof(unique_dest)))
      break;
    id = auto_filename ? queue_manager_add_auto(url, unique_dest, opts)
                       : queue_manager_add(url, unique_dest, opts);
    if (id == 0)
      break;
    int claim = file_preallocate(unique_dest, 0);
    if (claim == 0) {
      Download *download = queue_manager_find_by_id(id);
      if (download)
        download->reserved_file = true;
      int saved = auto_filename
                      ? db_insert_reserved_download_auto(id, url, unique_dest,
                                                         opts)
                      : db_insert_reserved_download(id, url, unique_dest, opts);
      if (saved == 0)
        break;
      unlink(unique_dest);
    }
    queue_manager_remove(id);
    id = 0;
    if (claim != -2)
      break;
  }
  if (id != 0 && strcmp(unique_dest, dest) != 0)
    LOG_INFO("Selected unique destination '%s'", unique_dest);
  return id;
}

/** Dispatch an incoming message. */
static void handle_message(int client_fd, MsgHeader *hdr) {
  switch (hdr->type) {
  case MSG_HELLO: {
    uint16_t version = IPC_PROTOCOL_VERSION;
    ipc_write_exact(client_fd, &version, sizeof(version));
    break;
  }
  case MSG_ADD_DOWNLOAD:
  case MSG_ADD_DOWNLOAD_AUTO: {
    char url[IPC_MAX_URL_LEN];
    char dest[IPC_MAX_PATH_LEN];
    char options_json[8192];
    if (read_string(client_fd, url, sizeof(url)) != 0 ||
        read_string(client_fd, dest, sizeof(dest)) != 0 ||
        read_string(client_fd, options_json, sizeof(options_json)) != 0)
      return;
    LOG_INFO("MSG_ADD_DOWNLOAD received: url='%s' dest='%s'", url, dest);

    RequestOptions opts = {0};
    if (options_json[0] != '\0') {
      cJSON *root = cJSON_Parse(options_json);
      if (root) {
        cJSON *v;
        v = cJSON_GetObjectItemCaseSensitive(root, "cookie");
        if (cJSON_IsString(v) && v->valuestring)
          strncpy(opts.cookie, v->valuestring, sizeof(opts.cookie) - 1);
        v = cJSON_GetObjectItemCaseSensitive(root, "referrer");
        if (cJSON_IsString(v) && v->valuestring)
          strncpy(opts.referrer, v->valuestring, sizeof(opts.referrer) - 1);
        v = cJSON_GetObjectItemCaseSensitive(root, "extra_headers");
        if (cJSON_IsString(v) && v->valuestring)
          strncpy(opts.extra_headers, v->valuestring,
                  sizeof(opts.extra_headers) - 1);
        v = cJSON_GetObjectItemCaseSensitive(root, "expected_sha256");
        if (cJSON_IsString(v) && v->valuestring)
          strncpy(opts.expected_sha256, v->valuestring,
                  sizeof(opts.expected_sha256) - 1);
        v = cJSON_GetObjectItemCaseSensitive(root, "speed_limit_bps");
        if (cJSON_IsNumber(v) && v->valuedouble > 0)
          opts.speed_limit_bps = (uint64_t)v->valuedouble;
        cJSON_Delete(root);
      } else {
        LOG_WARN("MSG_ADD_DOWNLOAD: malformed options JSON, ignoring");
      }
    }

    uint32_t id = reserve_download(url, dest, &opts,
                                   hdr->type == MSG_ADD_DOWNLOAD_AUTO);
    LOG_INFO("MSG_ADD_DOWNLOAD: queue_manager_add returned id=%u", id);

    ipc_write_exact(client_fd, &id, sizeof(id));
    break;
  }
  case MSG_BROWSER_OFFER: {
    char json[IPC_MAX_FRAME_SIZE + 1];
    IpcBrowserOffer response = {0};
    if (ipc_read_exact(client_fd, json, hdr->length) != 0)
      return;
    json[hdr->length] = '\0';
    IpcBrowserOffer proposed = {0};
    if (browser_parse_offer(json, &proposed)) {
      BrowserOfferSlot *slot = browser_find_request(proposed.request_id);
      if (slot && strcmp(slot->offer.url, proposed.url) != 0) {
        LOG_WARN("Browser request ID reused with a different URL");
        slot = NULL;
      } else if (!slot) {
        for (size_t i = 0; i < MAX_BROWSER_OFFERS; i++) {
          if (!g_browser_offers[i].offer.offer_id) {
            slot = &g_browser_offers[i];
            break;
          }
        }
        if (slot) {
          uint32_t id = 0;
          for (int attempt = 0; attempt < 16 && !id; attempt++) {
            id = browser_random_id();
            if (browser_find_offer(id))
              id = 0;
          }
          if (id) {
            slot->offer = proposed;
            slot->offer.offer_id = id;
            slot->touched_at = time(NULL);
            LOG_INFO("Browser offer %u registered for %s", id,
                     proposed.url);
          } else {
            slot = NULL;
          }
        }
      }
      if (slot)
        response = slot->offer;
    }
    ipc_write_exact(client_fd, &response, sizeof(response));
    break;
  }
  case MSG_BROWSER_GET_OFFER: {
    uint32_t id = 0;
    IpcBrowserOffer response = {0};
    if (ipc_read_exact(client_fd, &id, sizeof(id)) != 0)
      return;
    BrowserOfferSlot *slot = browser_find_offer(id);
    if (slot) {
      slot->touched_at = time(NULL);
      response = slot->offer;
    }
    ipc_write_exact(client_fd, &response, sizeof(response));
    break;
  }
  case MSG_BROWSER_CONFIRM: {
    char payload[sizeof(uint32_t) * 2 + IPC_MAX_PATH_LEN];
    uint32_t download_id = 0;
    if (ipc_read_exact(client_fd, payload, hdr->length) != 0)
      return;
    uint32_t offer_id = 0, path_len = 0;
    memcpy(&offer_id, payload, sizeof(offer_id));
    memcpy(&path_len, payload + sizeof(offer_id), sizeof(path_len));
    if (path_len > 0 && path_len < IPC_MAX_PATH_LEN &&
        hdr->length == sizeof(uint32_t) * 2 + path_len &&
        !memchr(payload + sizeof(uint32_t) * 2, '\0', path_len)) {
      char dest[IPC_MAX_PATH_LEN];
      memcpy(dest, payload + sizeof(uint32_t) * 2, path_len);
      dest[path_len] = '\0';
      BrowserOfferSlot *slot = browser_find_offer(offer_id);
      if (slot && slot->offer.state == IPC_BROWSER_CONFIRMED)
        download_id = slot->offer.download_id;
      else if (slot && slot->offer.state == IPC_BROWSER_WAITING) {
        RequestOptions opts = {0};
        snprintf(opts.referrer, sizeof(opts.referrer), "%s",
                 slot->offer.referrer);
        download_id = reserve_download(slot->offer.url, dest, &opts, false);
        if (download_id) {
          slot->offer.download_id = download_id;
          slot->offer.state = IPC_BROWSER_CONFIRMED;
          slot->touched_at = time(NULL);
          LOG_INFO("Browser offer %u confirmed as download %u", offer_id,
                   download_id);
        }
      }
    }
    ipc_write_exact(client_fd, &download_id, sizeof(download_id));
    break;
  }
  case MSG_BROWSER_DISMISS: {
    uint32_t id = 0;
    if (ipc_read_exact(client_fd, &id, sizeof(id)) != 0)
      return;
    BrowserOfferSlot *slot = browser_find_offer(id);
    IpcResult result = IPC_RESULT_NOT_FOUND;
    if (slot && slot->offer.state == IPC_BROWSER_WAITING) {
      slot->offer.state = IPC_BROWSER_DISMISSED;
      slot->touched_at = time(NULL);
      result = IPC_RESULT_OK;
      LOG_INFO("Browser offer %u dismissed", id);
    } else if (slot && slot->offer.state == IPC_BROWSER_DISMISSED) {
      result = IPC_RESULT_OK;
    } else if (slot) {
      result = IPC_RESULT_REJECTED;
    }
    send_command_result(client_fd, result);
    break;
  }
  case MSG_BROWSER_SUBSCRIBE_PROGRESS: {
    uint32_t id = 0;
    if (ipc_read_exact(client_fd, &id, sizeof(id)) != 0)
      return;
    IpcBrowserProgress event = browser_progress_snapshot(id, NULL);
    dm_mutex_lock(&g_client_mutex);
    if (strcmp(event.status, "NOT_FOUND") != 0) {
      for (int i = 0; i < g_client_count; i++) {
        if (g_client_fds[i] == client_fd) {
          g_client_browser_download[i] = id;
          break;
        }
      }
    }
    ipc_write_exact(client_fd, &event, sizeof(event));
    dm_mutex_unlock(&g_client_mutex);
    break;
  }
  case MSG_RELOAD_CONFIG:
    config_init(NULL);
    send_command_result(client_fd, IPC_RESULT_OK);
    break;
  case MSG_PAUSE: {
    uint32_t id;
    if (ipc_read_exact(client_fd, &id, sizeof(id)) != 0)
      return;
    DownloadStatus before;
    if (!queue_manager_get_status(id, &before)) {
      send_command_result(client_fd, IPC_RESULT_NOT_FOUND);
      break;
    }
    if (before != DOWNLOAD_ACTIVE && before != DOWNLOAD_QUEUED) {
      send_command_result(client_fd, IPC_RESULT_REJECTED);
      break;
    }
    bool was_active = queue_manager_pause(id);
    if (!was_active)
      db_update_status(id, "PAUSED");
    send_command_result(client_fd, IPC_RESULT_OK);
    break;
  }
  case MSG_RESUME: {
    uint32_t id;
    if (ipc_read_exact(client_fd, &id, sizeof(id)) != 0)
      return;
    DownloadStatus before;
    if (!queue_manager_get_status(id, &before)) {
      send_command_result(client_fd, IPC_RESULT_NOT_FOUND);
      break;
    }
    if (queue_manager_resume(id)) {
      db_update_status(id, "QUEUED");
      send_command_result(client_fd, IPC_RESULT_OK);
    } else {
      send_command_result(client_fd, IPC_RESULT_REJECTED);
    }
    break;
  }
  case MSG_CANCEL: {
    uint32_t id;
    if (ipc_read_exact(client_fd, &id, sizeof(id)) != 0)
      return;
    DownloadStatus before;
    if (!queue_manager_get_status(id, &before)) {
      send_command_result(client_fd, IPC_RESULT_NOT_FOUND);
      break;
    }
    if (before == DOWNLOAD_DONE) {
      send_command_result(client_fd, IPC_RESULT_REJECTED);
      break;
    }
    char canceled_path[IPC_MAX_PATH_LEN] = {0};
    Download *download = queue_manager_find_by_id(id);
    if (download)
      strncpy(canceled_path, download->dest_path, sizeof(canceled_path) - 1);
    bool was_active = queue_manager_cancel(id);
    if (!was_active) {
      queue_manager_clear_resume_state(id);
      db_delete_chunks(id);
      db_update_total_size(id, 0);
      if (canceled_path[0] != '\0')
        unlink(canceled_path);
      db_update_status(id, "CANCELED");
    }
    send_command_result(client_fd, IPC_RESULT_OK);
    break;
  }
  case MSG_LIST: {
    int count = queue_manager_count_by_status(DOWNLOAD_ACTIVE) +
                queue_manager_count_by_status(DOWNLOAD_QUEUED);
    uint32_t ucount = (uint32_t)count;
    ipc_write_exact(client_fd, &ucount, sizeof(ucount));
    break;
  }
  case MSG_LIST_ALL: {
    int n = db_count_downloads(IPC_LIST_ALL_MAX);
    if (n < 0) {
      uint32_t count = 0;
      ipc_write_exact(client_fd, &count, sizeof(count));
      break;
    }
    uint32_t count = (uint32_t)n;
    ipc_write_exact(client_fd, &count, sizeof(count));
    ListResponseContext response = {.client_fd = client_fd};
    db_visit_downloads(send_download_row, &response, IPC_LIST_ALL_MAX);
    break;
  }
  case MSG_LIST_PAGE: {
    uint32_t request[2];
    if (ipc_read_exact(client_fd, request, sizeof(request)) != 0)
      return;
    uint32_t limit = request[1] > IPC_LIST_PAGE_MAX ? IPC_LIST_PAGE_MAX
                                                     : request[1];
    int64_t db_total = db_count_downloads_total();
    uint32_t total = db_total < 0 ? 0
                     : db_total > UINT32_MAX ? UINT32_MAX
                                              : (uint32_t)db_total;
    PageCollectContext page = {0};
    if (db_total >= 0 && limit > 0 && request[0] < total) {
      page.rows = calloc(limit, sizeof(*page.rows));
      if (page.rows) {
        int count = db_visit_downloads_page(collect_download_row, &page,
                                            request[0], limit);
        if (count < 0)
          page.count = 0;
      }
    }
    uint32_t returned = (uint32_t)page.count;
    ipc_write_exact(client_fd, &total, sizeof(total));
    ipc_write_exact(client_fd, &returned, sizeof(returned));
    ListResponseContext response = {.client_fd = client_fd};
    for (int i = 0; i < page.count; i++) {
      if (send_download_row(&page.rows[i], &response) != 0)
        break;
    }
    free(page.rows);
    break;
  }
  case MSG_GET_DETAILS: {
    uint32_t id;
    ipc_read_exact(client_fd, &id, sizeof(id));

    IpcDownloadDetails details = {0};
    uint8_t found = (db_get_download_details(id, &details) == 0) ? 1 : 0;

    ipc_write_exact(client_fd, &found, sizeof(found));
    if (found) {
      write_string(client_fd, details.cookie);
      write_string(client_fd, details.referrer);
      write_string(client_fd, details.extra_headers);
      write_string(client_fd, details.expected_sha256);
      ipc_write_exact(client_fd, &details.speed_limit_bps,
                      sizeof(details.speed_limit_bps));
    }
    break;
  }
  case MSG_SUBSCRIBE:
  case MSG_SUBSCRIBE_V2: {
    dm_mutex_lock(&g_client_mutex);
    for (int i = 0; i < g_client_count; i++) {
      if (g_client_fds[i] == client_fd) {
        if (hdr->type == MSG_SUBSCRIBE ||
            g_client_browser_download[i] == 0)
          g_client_subscribed[i] = true;
        g_client_v2_subscribed[i] = hdr->type == MSG_SUBSCRIBE_V2;
        break;
      }
    }
    dm_mutex_unlock(&g_client_mutex);
    break;
  }
  default:
    LOG_WARN("Unknown message type: %d\n", hdr->type);
    break;
  }
}

/* Server lifecycle */

static int connect_daemon_socket(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  char socket_path[1024];
  get_socket_path(socket_path, sizeof(socket_path));
  size_t path_len = strlen(socket_path);
  if (path_len == 0 || path_len >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
    close(fd);
    errno = ENAMETOOLONG;
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, socket_path, path_len + 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  return fd;
}

bool ipc_server_is_running(void) {
  int fd = connect_daemon_socket();
  if (fd < 0)
    return false;
  close(fd);
  return true;
}

int ipc_server_get_pid(pid_t *pid_out) {
  if (!pid_out) {
    errno = EINVAL;
    return -1;
  }
  *pid_out = 0;
  int fd = connect_daemon_socket();
  if (fd < 0)
    return errno == ENOENT || errno == ECONNREFUSED ? 0 : -1;
#ifdef SO_PEERCRED
  struct {
    pid_t pid;
    uid_t uid;
    gid_t gid;
  } credentials;
  socklen_t length = sizeof(credentials);
  int result = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length);
  int saved_errno = errno;
  close(fd);
  if (result != 0) {
    errno = saved_errno;
    return -1;
  }
  if (length != sizeof(credentials) || credentials.pid <= 0 ||
      credentials.uid != getuid()) {
    errno = EPROTO;
    return -1;
  }
  if (kill(credentials.pid, 0) != 0 && errno == ESRCH)
    return 0;
  *pid_out = credentials.pid;
  return 1;
#else
  close(fd);
  errno = ENOTSUP;
  return -1;
#endif
}

int ipc_server_start(void) {
  if (ipc_server_is_running()) {
    LOG_ERROR("IPC server: Another daemon is already running.");
    return -1;
  }

  char socket_path[1024];
  get_socket_path(socket_path, sizeof(socket_path));

  unlink(socket_path);

  g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_listen_fd < 0) {
    LOG_ERROR("IPC server: socket() failed: %s", strerror(errno));
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("IPC server: bind() failed");
    close(g_listen_fd);
    g_listen_fd = -1;
    return -1;
  }

  if (fchmod(g_listen_fd, S_IRUSR | S_IWUSR) != 0) {
    LOG_ERROR("IPC server: could not restrict socket permissions: %s",
              strerror(errno));
    close(g_listen_fd);
    unlink(socket_path);
    g_listen_fd = -1;
    return -1;
  }

  if (listen(g_listen_fd, 5) < 0) {
    LOG_ERROR("IPC server: listen() failed: %s", strerror(errno));
    close(g_listen_fd);
    unlink(socket_path);
    g_listen_fd = -1;
    return -1;
  }

  for (int i = 0; i < MAX_CLIENTS; i++) {
    g_client_fds[i] = -1;
    g_client_subscribed[i] = false;
    g_client_browser_download[i] = 0;
    g_client_header_bytes[i] = 0;
  }
  memset(g_browser_offers, 0, sizeof(g_browser_offers));
  g_client_count = 0;

  call_once(&g_client_mutex_once, initialize_client_mutex);

  LOG_DEBUG("IPC server: Listening on %s\n", socket_path);
  return 0;
}

void ipc_server_poll(void) {
  browser_expire_offers();
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(g_listen_fd, &rfds);
  int max_fd = g_listen_fd;

  for (int i = 0; i < g_client_count; i++) {
    FD_SET(g_client_fds[i], &rfds);
    if (g_client_fds[i] > max_fd)
      max_fd = g_client_fds[i];
  }

  struct timeval tv = {0, IPC_POLL_TIMEOUT_US};
  int ready = select(max_fd + 1, &rfds, NULL, NULL, &tv);

  if (ready < 0) {
    if (errno == EINTR)
      return;
    LOG_ERROR("select() error: %s", strerror(errno));
    return;
  }
  if (ready == 0)
    return;

  if (FD_ISSET(g_listen_fd, &rfds)) {
    int new_fd = accept(g_listen_fd, NULL, NULL);
    if (new_fd >= 0) {
      dm_mutex_lock(&g_client_mutex);
      if (g_client_count < MAX_CLIENTS) {
        g_client_subscribed[g_client_count] = false;
        g_client_v2_subscribed[g_client_count] = false;
        g_client_browser_download[g_client_count] = 0;
        g_client_header_bytes[g_client_count] = 0;
        g_client_fds[g_client_count++] = new_fd;
        LOG_DEBUG("IPC: Client connected (fd=%d, total=%d)", new_fd,
                  g_client_count);
      } else {
        close(new_fd);
        LOG_DEBUG("IPC: Too many clients, rejected fd=%d", new_fd);
      }
      dm_mutex_unlock(&g_client_mutex);
    }
  }

  for (int i = 0; i < g_client_count; i++) {
    if (!FD_ISSET(g_client_fds[i], &rfds))
      continue;

    size_t remaining = sizeof(MsgHeader) - g_client_header_bytes[i];
    ssize_t n =
        recv(g_client_fds[i], g_client_header[i] + g_client_header_bytes[i],
             remaining, MSG_DONTWAIT);

    if (n == 0) {
      LOG_DEBUG("IPC: Client disconnected (fd=%d)", g_client_fds[i]);
      dm_mutex_lock(&g_client_mutex);
      remove_client(i);
      dm_mutex_unlock(&g_client_mutex);
      i--;
    } else if (n > 0) {
      g_client_header_bytes[i] += (size_t)n;
      if (g_client_header_bytes[i] != sizeof(MsgHeader))
        continue;

      MsgHeader hdr;
      memcpy(&hdr, g_client_header[i], sizeof(hdr));
      g_client_header_bytes[i] = 0;
      if (!valid_message_header(&hdr)) {
        LOG_WARN("IPC: Invalid frame type=%d len=%u from fd=%d", hdr.type,
                 hdr.length, g_client_fds[i]);
        dm_mutex_lock(&g_client_mutex);
        remove_client(i);
        dm_mutex_unlock(&g_client_mutex);
        i--;
        continue;
      }
      LOG_DEBUG("IPC: Received type=%d len=%u from fd=%d", hdr.type, hdr.length,
                g_client_fds[i]);
      handle_message(g_client_fds[i], &hdr);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      dm_mutex_lock(&g_client_mutex);
      remove_client(i);
      dm_mutex_unlock(&g_client_mutex);
      i--;
    }
  }
}

void ipc_server_stop(void) {
  for (int i = 0; i < g_client_count; i++)
    close(g_client_fds[i]);
  if (g_listen_fd >= 0)
    close(g_listen_fd);

  char socket_path[1024];
  get_socket_path(socket_path, sizeof(socket_path));
  unlink(socket_path);
  memset(g_browser_offers, 0, sizeof(g_browser_offers));
}

/* Client lifecycle */

int ipc_client_connect(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  char socket_path[1024];
  get_socket_path(socket_path, sizeof(socket_path));

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  LOG_DEBUG("IPC client connected, fd=%d\n", fd);
  return fd;
}

void ipc_client_disconnect(int fd) {
  if (fd >= 0)
    close(fd);
}

/* High‑level request / response */

static uint32_t send_add_download(int sock, MsgType type, const char *url,
                                  const char *dest_path,
                                  const IpcDownloadOptions *options) {
  if (sock < 0 || !url || !dest_path)
    return 0;

  char *options_json = NULL;
  if (options) {
    cJSON *root = cJSON_CreateObject();
    if (options->cookie && options->cookie[0])
      cJSON_AddStringToObject(root, "cookie", options->cookie);
    if (options->referrer && options->referrer[0])
      cJSON_AddStringToObject(root, "referrer", options->referrer);
    if (options->extra_headers && options->extra_headers[0])
      cJSON_AddStringToObject(root, "extra_headers", options->extra_headers);
    if (options->expected_sha256 && options->expected_sha256[0])
      cJSON_AddStringToObject(root, "expected_sha256",
                              options->expected_sha256);
    if (options->speed_limit_bps > 0)
      cJSON_AddNumberToObject(root, "speed_limit_bps",
                              (double)options->speed_limit_bps);
    options_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
  }

  const char *json_str = options_json ? options_json : "";
  uint32_t payload = 4 + (uint32_t)strlen(url) + 4 +
                     (uint32_t)strlen(dest_path) + 4 +
                     (uint32_t)strlen(json_str);

  MsgHeader hdr = {.length = payload, .type = type};
  bool sent = ipc_write_exact(sock, &hdr, sizeof(hdr)) == 0;
  if (sent)
    sent = ipc_write_exact(sock, &(uint32_t){(uint32_t)strlen(url)},
                           sizeof(uint32_t)) == 0;
  if (sent && url[0] != '\0')
    sent = ipc_write_exact(sock, url, strlen(url)) == 0;
  if (sent)
    sent = ipc_write_exact(sock, &(uint32_t){(uint32_t)strlen(dest_path)},
                           sizeof(uint32_t)) == 0;
  if (sent && dest_path[0] != '\0')
    sent = ipc_write_exact(sock, dest_path, strlen(dest_path)) == 0;
  if (sent)
    sent = ipc_write_exact(sock, &(uint32_t){(uint32_t)strlen(json_str)},
                           sizeof(uint32_t)) == 0;
  if (sent && json_str[0] != '\0')
    sent = ipc_write_exact(sock, json_str, strlen(json_str)) == 0;

  uint32_t id = 0;
  if (sent)
    sent = ipc_read_exact(sock, &id, sizeof(id)) == 0;

  if (options_json)
    cJSON_free(options_json);
  return sent ? id : 0;
}

uint32_t ipc_send_add_download(int sock, const char *url, const char *dest_path,
                               const IpcDownloadOptions *options) {
  return send_add_download(sock, MSG_ADD_DOWNLOAD, url, dest_path, options);
}

uint32_t ipc_send_add_download_auto(int sock, const char *url,
                                    const char *dest_path,
                                    const IpcDownloadOptions *options) {
  return send_add_download(sock, MSG_ADD_DOWNLOAD_AUTO, url, dest_path,
                           options);
}

int ipc_client_connect_timeout(int timeout_ms) {
  int fd = ipc_client_connect();
  if (fd < 0)
    return -1;

  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

int ipc_client_hello(int sock, uint16_t *daemon_version) {
  if (!daemon_version)
    return -1;
  MsgHeader hello = {.length = 0, .type = MSG_HELLO};
  if (ipc_write_exact(sock, &hello, sizeof(hello)) != 0 ||
      ipc_read_exact(sock, daemon_version, sizeof(*daemon_version)) != 0)
    return -1;
  return 0;
}

int ipc_client_connect_compatible(int timeout_ms, uint16_t *daemon_version) {
  int hello_timeout_ms = timeout_ms < 0 ? 1500 : timeout_ms;
  int fd = ipc_client_connect_timeout(hello_timeout_ms);
  if (fd < 0)
    return -1;
  uint16_t version = 1;
  if (ipc_client_hello(fd, &version) != 0) {
    ipc_client_disconnect(fd);
    fd = ipc_client_connect_timeout(hello_timeout_ms);
    if (fd < 0)
      return -1;
    version = 1;
  }
  if (version != IPC_PROTOCOL_VERSION)
    LOG_WARN("IPC daemon version %u differs from client version %u; using v1 messages",
             (unsigned)version, (unsigned)IPC_PROTOCOL_VERSION);
  if (daemon_version)
    *daemon_version = version;
  if (timeout_ms < 0) {
    struct timeval blocking = {0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &blocking,
                   sizeof(blocking)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &blocking,
                   sizeof(blocking)) != 0) {
      ipc_client_disconnect(fd);
      return -1;
    }
  }
  return fd;
}

int ipc_send_pause(int sock, uint32_t id) {
  MsgHeader hdr = {.length = sizeof(uint32_t), .type = MSG_PAUSE};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;
  if (ipc_write_exact(sock, &id, sizeof(id)) != 0)
    return -1;
  uint8_t result = IPC_RESULT_ERROR;
  if (ipc_read_exact(sock, &result, sizeof(result)) != 0)
    return -1;
  return result == IPC_RESULT_OK ? 0 : -1;
}

int ipc_send_resume(int sock, uint32_t id) {
  MsgHeader hdr = {.length = sizeof(uint32_t), .type = MSG_RESUME};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;
  if (ipc_write_exact(sock, &id, sizeof(id)) != 0)
    return -1;
  uint8_t result = IPC_RESULT_ERROR;
  if (ipc_read_exact(sock, &result, sizeof(result)) != 0)
    return -1;
  return result == IPC_RESULT_OK ? 0 : -1;
}

int ipc_send_cancel(int sock, uint32_t id) {
  MsgHeader hdr = {.length = sizeof(uint32_t), .type = MSG_CANCEL};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;
  if (ipc_write_exact(sock, &id, sizeof(id)) != 0)
    return -1;
  uint8_t result = IPC_RESULT_ERROR;
  if (ipc_read_exact(sock, &result, sizeof(result)) != 0)
    return -1;
  return result == IPC_RESULT_OK ? 0 : -1;
}

static int read_download_rows(int sock, uint32_t count,
                              IpcDownloadRecord *out, int max) {
  int n = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t id;
    char url[IPC_MAX_URL_LEN];
    char dest_path[IPC_MAX_PATH_LEN];
    char status[16];
    float progress;

    if (ipc_read_exact(sock, &id, sizeof(id)) != 0 ||
        read_string(sock, url, sizeof(url)) != 0 ||
        read_string(sock, dest_path, sizeof(dest_path)) != 0 ||
        read_string(sock, status, sizeof(status)) != 0 ||
        ipc_read_exact(sock, &progress, sizeof(progress)) != 0)
      return -1;

    if (n < max) {
      out[n].id = id;
      strncpy(out[n].url, url, sizeof(out[n].url) - 1);
      out[n].url[sizeof(out[n].url) - 1] = '\0';
      strncpy(out[n].dest_path, dest_path, sizeof(out[n].dest_path) - 1);
      out[n].dest_path[sizeof(out[n].dest_path) - 1] = '\0';
      strncpy(out[n].status, status, sizeof(out[n].status) - 1);
      out[n].status[sizeof(out[n].status) - 1] = '\0';
      out[n].progress = progress;
      n++;
    }
  }
  return n;
}

int ipc_send_list_all(int sock, IpcDownloadRecord *out, int max) {
  if (!out || max <= 0)
    return -1;
  MsgHeader hdr = {.length = 0, .type = MSG_LIST_ALL};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;
  uint32_t count = 0;
  if (ipc_read_exact(sock, &count, sizeof(count)) != 0)
    return -1;
  return read_download_rows(sock, count, out, max);
}

int ipc_send_list_page(int sock, uint32_t offset, uint32_t limit,
                       IpcDownloadRecord *out, int max, uint32_t *total_out) {
  if (!out || max <= 0 || !total_out)
    return -1;
  MsgHeader hdr = {.length = sizeof(uint32_t) * 2, .type = MSG_LIST_PAGE};
  uint32_t request[2] = {offset, limit};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0 ||
      ipc_write_exact(sock, request, sizeof(request)) != 0)
    return -1;
  uint32_t total = 0, returned = 0;
  if (ipc_read_exact(sock, &total, sizeof(total)) != 0 ||
      ipc_read_exact(sock, &returned, sizeof(returned)) != 0 ||
      returned > IPC_LIST_PAGE_MAX)
    return -1;
  int count = read_download_rows(sock, returned, out, max);
  if (count >= 0)
    *total_out = total;
  return count;
}

int ipc_send_get_details(int sock, uint32_t id, IpcDownloadDetails *out) {
  MsgHeader hdr = {.length = sizeof(id), .type = MSG_GET_DETAILS};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;
  if (ipc_write_exact(sock, &id, sizeof(id)) != 0)
    return -1;

  uint8_t found = 0;
  if (ipc_read_exact(sock, &found, sizeof(found)) != 0)
    return -1;
  if (!found)
    return -1;

  if (read_string(sock, out->cookie, sizeof(out->cookie)) != 0 ||
      read_string(sock, out->referrer, sizeof(out->referrer)) != 0 ||
      read_string(sock, out->extra_headers, sizeof(out->extra_headers)) != 0 ||
      read_string(sock, out->expected_sha256, sizeof(out->expected_sha256)) !=
          0)
    return -1;
  return ipc_read_exact(sock, &out->speed_limit_bps,
                        sizeof(out->speed_limit_bps));
}

void ipc_send_subscribe(int sock) {
  MsgHeader hdr = {.length = 0, .type = MSG_SUBSCRIBE};
  ipc_write_exact(sock, &hdr, sizeof(hdr));
}

int ipc_send_subscribe_v2(int sock) {
  MsgHeader hdr = {.length = 0, .type = MSG_SUBSCRIBE_V2};
  return ipc_write_exact(sock, &hdr, sizeof(hdr));
}

int ipc_send_reload_config(int sock) {
  MsgHeader hdr = {.length = 0, .type = MSG_RELOAD_CONFIG};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;
  uint8_t result = IPC_RESULT_ERROR;
  if (ipc_read_exact(sock, &result, sizeof(result)) != 0)
    return -1;
  return result == IPC_RESULT_OK ? 0 : -1;
}

static int browser_write_request(int sock, MsgType type, const void *payload,
                                 uint32_t size) {
  if (sock < 0 || size > IPC_MAX_FRAME_SIZE)
    return -1;
  MsgHeader header = {.length = size, .type = type};
  if (ipc_write_exact(sock, &header, sizeof(header)) != 0)
    return -1;
  return size ? ipc_write_exact(sock, payload, size) : 0;
}

int ipc_browser_offer(int sock, const IpcBrowserOffer *offer,
                      IpcBrowserOffer *out) {
  if (!offer || !out || !offer->request_id[0] || !offer->url[0])
    return -1;
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return -1;
  cJSON_AddStringToObject(root, "request_id", offer->request_id);
  cJSON_AddStringToObject(root, "url", offer->url);
  cJSON_AddStringToObject(root, "filename", offer->filename);
  cJSON_AddStringToObject(root, "mime", offer->mime);
  cJSON_AddStringToObject(root, "referrer", offer->referrer);
  cJSON_AddNumberToObject(root, "total_bytes", (double)offer->total_bytes);
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json)
    return -1;
  size_t length = strlen(json);
  int result = -1;
  if (length <= IPC_MAX_FRAME_SIZE &&
      browser_write_request(sock, MSG_BROWSER_OFFER, json,
                            (uint32_t)length) == 0 &&
      ipc_read_exact(sock, out, sizeof(*out)) == 0 && out->offer_id)
    result = 0;
  cJSON_free(json);
  return result;
}

int ipc_browser_get_offer(int sock, uint32_t offer_id, IpcBrowserOffer *out) {
  if (!out || !offer_id ||
      browser_write_request(sock, MSG_BROWSER_GET_OFFER, &offer_id,
                            sizeof(offer_id)) != 0 ||
      ipc_read_exact(sock, out, sizeof(*out)) != 0)
    return -1;
  return out->offer_id ? 0 : -1;
}

int ipc_browser_confirm(int sock, uint32_t offer_id, const char *dest_path,
                        uint32_t *download_id) {
  if (!offer_id || !dest_path || !download_id)
    return -1;
  size_t len = strlen(dest_path);
  if (!len || len >= IPC_MAX_PATH_LEN)
    return -1;
  char payload[sizeof(uint32_t) * 2 + IPC_MAX_PATH_LEN];
  uint32_t wire_len = (uint32_t)len;
  memcpy(payload, &offer_id, sizeof(offer_id));
  memcpy(payload + sizeof(offer_id), &wire_len, sizeof(wire_len));
  memcpy(payload + sizeof(uint32_t) * 2, dest_path, len);
  if (browser_write_request(sock, MSG_BROWSER_CONFIRM, payload,
                            (uint32_t)(sizeof(uint32_t) * 2 + len)) != 0 ||
      ipc_read_exact(sock, download_id, sizeof(*download_id)) != 0)
    return -1;
  return *download_id ? 0 : -1;
}

int ipc_browser_dismiss(int sock, uint32_t offer_id) {
  if (!offer_id || browser_write_request(sock, MSG_BROWSER_DISMISS, &offer_id,
                                         sizeof(offer_id)) != 0)
    return -1;
  uint8_t result = IPC_RESULT_ERROR;
  if (ipc_read_exact(sock, &result, sizeof(result)) != 0)
    return -1;
  return result == IPC_RESULT_OK ? 0 : -1;
}

int ipc_browser_subscribe_progress(int sock, uint32_t download_id,
                                   IpcBrowserProgress *initial) {
  if (!download_id || !initial ||
      browser_write_request(sock, MSG_BROWSER_SUBSCRIBE_PROGRESS, &download_id,
                            sizeof(download_id)) != 0 ||
      ipc_read_exact(sock, initial, sizeof(*initial)) != 0)
    return -1;
  return strcmp(initial->status, "NOT_FOUND") == 0 ? -1 : 0;
}

/* Low‑level I/O */

int ipc_read_exact(int fd, void *buf, size_t len) {
  size_t remaining = len;
  char *ptr = (char *)buf;
  while (remaining > 0) {
    ssize_t n = read(fd, ptr, remaining);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1; /* connection closed */
    remaining -= (size_t)n;
    ptr += n;
  }
  return 0;
}

int ipc_write_exact(int fd, const void *buf, size_t len) {
  size_t remaining = len;
  const char *ptr = (const char *)buf;
  while (remaining > 0) {
#if defined(MSG_NOSIGNAL)
    ssize_t n = send(fd, ptr, remaining, MSG_NOSIGNAL);
#else
    ssize_t n = write(fd, ptr, remaining);
#endif
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    remaining -= (size_t)n;
    ptr += n;
  }
  return 0;
}

/* Broadcast */

void ipc_broadcast_status(uint32_t download_id, const char *status,
                          float progress) {
  const char *text = status ? status : "";
  size_t status_len = strlen(text);
  size_t payload_len =
      sizeof(download_id) + sizeof(progress) + sizeof(uint32_t) + status_len;
  if (payload_len > IPC_MAX_FRAME_SIZE)
    return;

  unsigned char frame[sizeof(MsgHeader) + IPC_MAX_FRAME_SIZE];
  MsgHeader hdr = {.length = (uint32_t)payload_len, .type = MSG_STATUS_EVENT};
  size_t offset = 0;
  memcpy(frame + offset, &hdr, sizeof(hdr));
  offset += sizeof(hdr);
  memcpy(frame + offset, &download_id, sizeof(download_id));
  offset += sizeof(download_id);
  memcpy(frame + offset, &progress, sizeof(progress));
  offset += sizeof(progress);
  uint32_t wire_status_len = (uint32_t)status_len;
  memcpy(frame + offset, &wire_status_len, sizeof(wire_status_len));
  offset += sizeof(wire_status_len);
  memcpy(frame + offset, text, status_len);
  offset += status_len;

  IpcBrowserProgress browser_event = browser_progress_snapshot(
      download_id, strcmp(text, "Error") == 0 ||
                           strcmp(text, "Verification failed") == 0
                       ? text
                       : NULL);
  unsigned char browser_frame[sizeof(MsgHeader) + sizeof(browser_event)];
  MsgHeader browser_hdr = {.length = sizeof(browser_event),
                           .type = MSG_BROWSER_PROGRESS_EVENT};
  memcpy(browser_frame, &browser_hdr, sizeof(browser_hdr));
  memcpy(browser_frame + sizeof(browser_hdr), &browser_event,
         sizeof(browser_event));

  IpcProgressV2 rich_event;
  memset(&rich_event, 0, sizeof(rich_event));
  DownloadRuntimeSnapshot transfer_snapshot = {0};
  if (queue_manager_get_runtime_snapshot(download_id, &transfer_snapshot)) {
    rich_event.speed_bps = transfer_snapshot.transfer_metrics.speed_bps;
    rich_event.eta_seconds = transfer_snapshot.transfer_metrics.eta_seconds;
  } else {
    rich_event.eta_seconds = UINT64_MAX;
  }
  rich_event.download_id = download_id;
  rich_event.bytes_received = browser_event.bytes_received;
  rich_event.total_bytes = browser_event.total_bytes;
  rich_event.progress = browser_event.total_bytes ? progress : -1.0f;
  snprintf(rich_event.status, sizeof(rich_event.status), "%s", text);
  snprintf(rich_event.error, sizeof(rich_event.error), "%s",
           browser_event.error);
  unsigned char rich_frame[sizeof(MsgHeader) + sizeof(rich_event)];
  MsgHeader rich_hdr = {.length = sizeof(rich_event),
                        .type = MSG_STATUS_EVENT_V2};
  memcpy(rich_frame, &rich_hdr, sizeof(rich_hdr));
  memcpy(rich_frame + sizeof(rich_hdr), &rich_event, sizeof(rich_event));

  dm_mutex_lock(&g_client_mutex);
  for (int i = 0; i < g_client_count; i++) {
    if (!g_client_subscribed[i] &&
        g_client_browser_download[i] != download_id)
      continue;

    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif
#if defined(MSG_DONTWAIT)
    flags |= MSG_DONTWAIT;
#endif
    const void *bytes = g_client_browser_download[i] == download_id
                            ? (const void *)browser_frame
                            : (const void *)frame;
    size_t bytes_len = g_client_browser_download[i] == download_id
                           ? sizeof(browser_frame)
                           : offset;
    if (g_client_v2_subscribed[i]) {
      ssize_t rich_sent = send(g_client_fds[i], rich_frame,
                               sizeof(rich_frame), flags);
      if (rich_sent != (ssize_t)sizeof(rich_frame)) {
        LOG_DEBUG("IPC: removing slow or disconnected v2 subscriber (fd=%d)",
                  g_client_fds[i]);
        remove_client(i);
        i--;
        continue;
      }
    }
    ssize_t sent = send(g_client_fds[i], bytes, bytes_len, flags);
    if (sent != (ssize_t)bytes_len) {
      LOG_DEBUG("IPC: removing slow or disconnected subscriber (fd=%d)",
                g_client_fds[i]);
      remove_client(i);
      i--;
    }
  }
  dm_mutex_unlock(&g_client_mutex);
}
