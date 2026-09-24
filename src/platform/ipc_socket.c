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
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

/* Constants */
#define MAX_CLIENTS 16
#define IPC_POLL_TIMEOUT_US 20000 /* 20 ms */

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

/** Get user-isolated IPC socket path (XDG_RUNTIME_DIR,
 * ~/.local/share/cdm, or /tmp/cdm_UID.sock). */
static void get_socket_path(char *out, size_t out_size) {
  static char cached_path[1024] = {0};
  if (cached_path[0] != '\0') {
    strncpy(out, cached_path, out_size - 1);
    out[out_size - 1] = '\0';
    return;
  }

  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (runtime_dir && runtime_dir[0] != '\0') {
    if (!join_path(cached_path, sizeof(cached_path), runtime_dir,
                   "/cdm.sock"))
      cached_path[0] = '\0';
  } else {
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
      char dir[1024];
      if (join_path(dir, sizeof(dir), home, "/.local/share/cdm") &&
          join_path(cached_path, sizeof(cached_path), dir, "/ipc.sock")) {
        file_ensure_directory(dir);
      } else {
        cached_path[0] = '\0';
      }
    } else {
      snprintf(cached_path, sizeof(cached_path), "/tmp/cdm_%u.sock",
               (unsigned int)getuid());
    }
  }

  strncpy(out, cached_path, out_size - 1);
  out[out_size - 1] = '\0';
}

/* Server state */
static int g_listen_fd = -1;
static int g_client_fds[MAX_CLIENTS];
static int g_client_count = 0;
static bool g_client_subscribed[MAX_CLIENTS];
static unsigned char g_client_header[MAX_CLIENTS][sizeof(MsgHeader)];
static size_t g_client_header_bytes[MAX_CLIENTS];
static dm_mutex_t g_client_mutex;
static once_flag g_client_mutex_once = ONCE_FLAG_INIT;

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
    g_client_header_bytes[i] = g_client_header_bytes[i + 1];
    memcpy(g_client_header[i], g_client_header[i + 1], sizeof(MsgHeader));
  }
  g_client_fds[g_client_count - 1] = -1;
  g_client_subscribed[g_client_count - 1] = false;
  g_client_header_bytes[g_client_count - 1] = 0;
  g_client_count--;
}

static bool valid_message_header(const MsgHeader *header) {
  if (header->length > IPC_MAX_FRAME_SIZE)
    return false;

  switch (header->type) {
  case MSG_ADD_DOWNLOAD:
    return header->length <= IPC_MAX_FRAME_SIZE;
  case MSG_PAUSE:
  case MSG_RESUME:
  case MSG_CANCEL:
  case MSG_GET_DETAILS:
    return header->length == sizeof(uint32_t);
  case MSG_LIST:
  case MSG_LIST_ALL:
  case MSG_SUBSCRIBE:
  case MSG_RELOAD_CONFIG:
    return header->length == 0;
  default:
    return false;
  }
}

typedef struct {
  int client_fd;
} ListResponseContext;

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

/** Dispatch an incoming message. */
static void handle_message(int client_fd, MsgHeader *hdr) {
  switch (hdr->type) {
  case MSG_ADD_DOWNLOAD: {
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

    char unique_dest[IPC_MAX_PATH_LEN];
    uint32_t id = 0;
    for (unsigned int attempt = 0; attempt < 1000000; attempt++) {
      if (!path_make_unique(dest, unique_dest, sizeof(unique_dest)))
        break;
      id = queue_manager_add(url, unique_dest, &opts);
      if (id == 0)
        break;
      /* Claim the actual destination before replying to the client. */
      int claim = file_preallocate(unique_dest, 0);
      if (claim == 0) {
        Download *download = queue_manager_find_by_id(id);
        if (download)
          download->reserved_file = true;
        if (db_insert_reserved_download(id, url, unique_dest, &opts) == 0)
          break;
        unlink(unique_dest);
      }
      queue_manager_remove(id);
      id = 0;
      if (claim != -2)
        break;
    }
    if (id != 0 && strcmp(unique_dest, dest) != 0)
      LOG_INFO("MSG_ADD_DOWNLOAD: selected unique destination '%s'",
               unique_dest);
    LOG_INFO("MSG_ADD_DOWNLOAD: queue_manager_add returned id=%u", id);

    ipc_write_exact(client_fd, &id, sizeof(id));
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
  case MSG_SUBSCRIBE: {
    dm_mutex_lock(&g_client_mutex);
    for (int i = 0; i < g_client_count; i++) {
      if (g_client_fds[i] == client_fd) {
        g_client_subscribed[i] = true;
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

bool ipc_server_is_running(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return false;

  char socket_path[1024];
  get_socket_path(socket_path, sizeof(socket_path));

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  bool running = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  close(fd);
  return running;
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
    g_client_header_bytes[i] = 0;
  }
  g_client_count = 0;

  call_once(&g_client_mutex_once, initialize_client_mutex);

  LOG_DEBUG("IPC server: Listening on %s\n", socket_path);
  return 0;
}

void ipc_server_poll(void) {
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

uint32_t ipc_send_add_download(int sock, const char *url, const char *dest_path,
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

  MsgHeader hdr = {.length = payload, .type = MSG_ADD_DOWNLOAD};
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

int ipc_send_list_all(int sock, IpcDownloadRecord *out, int max) {
  if (!out || max <= 0)
    return -1;
  MsgHeader hdr = {.length = 0, .type = MSG_LIST_ALL};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;

  uint32_t count = 0;
  if (ipc_read_exact(sock, &count, sizeof(count)) != 0)
    return -1;

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

int ipc_send_reload_config(int sock) {
  MsgHeader hdr = {.length = 0, .type = MSG_RELOAD_CONFIG};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;
  uint8_t result = IPC_RESULT_ERROR;
  if (ipc_read_exact(sock, &result, sizeof(result)) != 0)
    return -1;
  return result == IPC_RESULT_OK ? 0 : -1;
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

  dm_mutex_lock(&g_client_mutex);
  for (int i = 0; i < g_client_count; i++) {
    if (!g_client_subscribed[i])
      continue;

    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif
#if defined(MSG_DONTWAIT)
    flags |= MSG_DONTWAIT;
#endif
    ssize_t sent = send(g_client_fds[i], frame, offset, flags);
    if (sent != (ssize_t)offset) {
      LOG_DEBUG("IPC: removing slow or disconnected subscriber (fd=%d)",
                g_client_fds[i]);
      remove_client(i);
      i--;
    }
  }
  dm_mutex_unlock(&g_client_mutex);
}
