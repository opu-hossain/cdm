// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "ipc_socket.h"

#include "../core/queue_manager.h"
#include "../persistence/db.h"
#include "../utils/log.h"
#include "../vendor/cJSON.h"
#include "thread.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */
#define SOCKET_PATH "/tmp/downloadmgr.sock"
#define MAX_CLIENTS 16
#define IPC_POLL_TIMEOUT_US 20000 /* 20 ms */

/* ------------------------------------------------------------------ */
/*  Server state                                                      */
/* ------------------------------------------------------------------ */
static int g_listen_fd = -1;
static int g_client_fds[MAX_CLIENTS];
static int g_client_count = 0;
static bool g_client_subscribed[MAX_CLIENTS];
static dm_mutex_t g_client_mutex;
static bool g_client_mutex_ready = false;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

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

/** Receive a length‑prefixed string (max `max_len` bytes including NUL). */
static void read_string(int fd, char *out, size_t max_len) {
  uint32_t len = 0;
  ipc_read_exact(fd, &len, sizeof(len));
  if (len >= max_len)
    len = (uint32_t)(max_len - 1);
  if (len > 0)
    ipc_read_exact(fd, out, len);
  out[len] = '\0';
}

/** Remove a client from the array and compact. */
static void remove_client(int index) {
  close(g_client_fds[index]);
  for (int i = index; i < g_client_count - 1; i++) {
    g_client_fds[i] = g_client_fds[i + 1];
    g_client_subscribed[i] = g_client_subscribed[i + 1];
  }
  g_client_fds[g_client_count - 1] = -1;
  g_client_subscribed[g_client_count - 1] = false;
  g_client_count--;
}

/** Dispatch an incoming message. */
static void handle_message(int client_fd, MsgHeader *hdr) {
  switch (hdr->type) {
  case MSG_ADD_DOWNLOAD: {
    char url[IPC_MAX_URL_LEN];
    char dest[IPC_MAX_PATH_LEN];
    char options_json[8192];
    read_string(client_fd, url, sizeof(url));
    read_string(client_fd, dest, sizeof(dest));
    read_string(client_fd, options_json, sizeof(options_json));
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

    uint32_t id = queue_manager_add(url, dest, &opts);
    LOG_INFO("MSG_ADD_DOWNLOAD: queue_manager_add returned id=%u", id);
    if (id != 0)
      db_insert_download(id, url, dest, &opts);

    ipc_write_exact(client_fd, &id, sizeof(id));
    break;
  }
  case MSG_PAUSE: {
    uint32_t id;
    ipc_read_exact(client_fd, &id, sizeof(id));
    bool was_active = queue_manager_pause(id);
    if (!was_active)
      db_update_status(id, "PAUSED");
    break;
  }
  case MSG_RESUME: {
    uint32_t id;
    ipc_read_exact(client_fd, &id, sizeof(id));
    if (queue_manager_resume(id))
      db_update_status(id, "QUEUED");
    break;
  }
  case MSG_CANCEL: {
    uint32_t id;
    ipc_read_exact(client_fd, &id, sizeof(id));
    bool was_active = queue_manager_cancel(id);
    if (!was_active)
      db_update_status(id, "CANCELED");
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
    DbDownloadRow rows[IPC_LIST_ALL_MAX];
    int n = db_list_all_downloads(rows, IPC_LIST_ALL_MAX);
    uint32_t count = (uint32_t)n;
    ipc_write_exact(client_fd, &count, sizeof(count));

    for (int i = 0; i < n; i++) {
      char status_buf[16];
      float progress = 0.0f;

      Download *live = queue_manager_find_by_id(rows[i].id);
      if (live) {
        strncpy(status_buf, status_to_string(live->status),
                sizeof(status_buf) - 1);
        status_buf[sizeof(status_buf) - 1] = '\0';

        uint64_t total = live->total_size;
        if (total > 0) {
          uint64_t done = 0;
          if (live->status == DOWNLOAD_ACTIVE) {
            done = atomic_load(&live->bytes_downloaded);
          } else if (live->chunk_count > 0) {
            for (int c = 0; c < live->chunk_count; c++)
              done += live->chunks[c].bytes_done;
          }
          double frac = (double)done / (double)total;
          progress = (float)(frac > 1.0 ? 1.0 : frac);
        }
      } else {
        strncpy(status_buf, rows[i].status, sizeof(status_buf) - 1);
        status_buf[sizeof(status_buf) - 1] = '\0';
      }

      if (strcmp(status_buf, "DONE") == 0)
        progress = 1.0f;

      ipc_write_exact(client_fd, &rows[i].id, sizeof(rows[i].id));
      write_string(client_fd, rows[i].url);
      write_string(client_fd, rows[i].dest_path);
      write_string(client_fd, status_buf);
      ipc_write_exact(client_fd, &progress, sizeof(progress));
    }
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

/* ------------------------------------------------------------------ */
/*  Server lifecycle                                                  */
/* ------------------------------------------------------------------ */

bool ipc_server_is_running(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return false;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

  bool running = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  close(fd);
  return running;
}

int ipc_server_start(void) {
  if (ipc_server_is_running()) {
    LOG_ERROR("IPC server: Another daemon is already running.");
    return -1;
  }

  unlink(SOCKET_PATH);

  g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_listen_fd < 0) {
    LOG_ERROR("IPC server: socket() failed: %s", strerror(errno));
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

  if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("IPC server: bind() failed");
    close(g_listen_fd);
    g_listen_fd = -1;
    return -1;
  }

  if (listen(g_listen_fd, 5) < 0) {
    LOG_ERROR("IPC server: listen() failed: %s", strerror(errno));
    close(g_listen_fd);
    unlink(SOCKET_PATH);
    g_listen_fd = -1;
    return -1;
  }

  for (int i = 0; i < MAX_CLIENTS; i++) {
    g_client_fds[i] = -1;
    g_client_subscribed[i] = false;
  }
  g_client_count = 0;

  if (!g_client_mutex_ready) {
    dm_mutex_init(&g_client_mutex);
    g_client_mutex_ready = true;
  }

  LOG_DEBUG("IPC server: Listening on %s\n", SOCKET_PATH);
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

    MsgHeader hdr;
    ssize_t n = recv(g_client_fds[i], &hdr, sizeof(hdr), MSG_DONTWAIT);

    if (n == 0) {
      LOG_DEBUG("IPC: Client disconnected (fd=%d)", g_client_fds[i]);
      dm_mutex_lock(&g_client_mutex);
      close(g_client_fds[i]);
      for (int j = i; j < g_client_count - 1; j++) {
        g_client_fds[j] = g_client_fds[j + 1];
        g_client_subscribed[j] = g_client_subscribed[j + 1];
      }
      g_client_count--;
      g_client_subscribed[g_client_count] = false;
      dm_mutex_unlock(&g_client_mutex);
      i--;
    } else if (n > 0 && (size_t)n == sizeof(hdr)) {
      LOG_DEBUG("IPC: Received type=%d len=%u from fd=%d", hdr.type, hdr.length,
                g_client_fds[i]);
      handle_message(g_client_fds[i], &hdr);
    }
  }
}

void ipc_server_stop(void) {
  for (int i = 0; i < g_client_count; i++)
    close(g_client_fds[i]);
  if (g_listen_fd >= 0)
    close(g_listen_fd);
  unlink(SOCKET_PATH);
}

/* ------------------------------------------------------------------ */
/*  Client lifecycle                                                  */
/* ------------------------------------------------------------------ */

int ipc_client_connect(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

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

/* ------------------------------------------------------------------ */
/*  High‑level request / response                                     */
/* ------------------------------------------------------------------ */

uint32_t ipc_send_add_download(int sock, const char *url, const char *dest_path,
                               const IpcDownloadOptions *options) {
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
  ipc_write_exact(sock, &hdr, sizeof(hdr));
  write_string(sock, url);
  write_string(sock, dest_path);
  write_string(sock, json_str);

  uint32_t id = 0;
  ipc_read_exact(sock, &id, sizeof(id));

  if (options_json)
    cJSON_free(options_json);
  return id;
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
  return 0;
}

int ipc_send_resume(int sock, uint32_t id) {
  MsgHeader hdr = {.length = sizeof(uint32_t), .type = MSG_RESUME};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;
  if (ipc_write_exact(sock, &id, sizeof(id)) != 0)
    return -1;
  return 0;
}

int ipc_send_cancel(int sock, uint32_t id) {
  MsgHeader hdr = {.length = sizeof(uint32_t), .type = MSG_CANCEL};
  if (ipc_write_exact(sock, &hdr, sizeof(hdr)) != 0)
    return -1;
  if (ipc_write_exact(sock, &id, sizeof(id)) != 0)
    return -1;
  return 0;
}

int ipc_send_list_all(int sock, IpcDownloadRecord *out, int max) {
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

    ipc_read_exact(sock, &id, sizeof(id));
    read_string(sock, url, sizeof(url));
    read_string(sock, dest_path, sizeof(dest_path));
    read_string(sock, status, sizeof(status));
    ipc_read_exact(sock, &progress, sizeof(progress));

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

  read_string(sock, out->cookie, sizeof(out->cookie));
  read_string(sock, out->referrer, sizeof(out->referrer));
  read_string(sock, out->extra_headers, sizeof(out->extra_headers));
  read_string(sock, out->expected_sha256, sizeof(out->expected_sha256));
  return ipc_read_exact(sock, &out->speed_limit_bps,
                        sizeof(out->speed_limit_bps));
}

void ipc_send_subscribe(int sock) {
  MsgHeader hdr = {.length = 0, .type = MSG_SUBSCRIBE};
  ipc_write_exact(sock, &hdr, sizeof(hdr));
}

/* ------------------------------------------------------------------ */
/*  Low‑level I/O                                                     */
/* ------------------------------------------------------------------ */

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
    ssize_t n = write(fd, ptr, remaining);
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

/* ------------------------------------------------------------------ */
/*  Broadcast                                                         */
/* ------------------------------------------------------------------ */

void ipc_broadcast_status(uint32_t download_id, const char *status,
                          float progress) {
  MsgHeader hdr;
  hdr.type = MSG_STATUS_EVENT;
  hdr.length = sizeof(uint32_t) + sizeof(float) +
               (uint32_t)(4 + (status ? strlen(status) : 0));

  dm_mutex_lock(&g_client_mutex);
  for (int i = 0; i < g_client_count; i++) {
    if (!g_client_subscribed[i])
      continue;
    ipc_write_exact(g_client_fds[i], &hdr, sizeof(hdr));
    ipc_write_exact(g_client_fds[i], &download_id, sizeof(download_id));
    ipc_write_exact(g_client_fds[i], &progress, sizeof(progress));
    write_string(g_client_fds[i], status);
  }
  dm_mutex_unlock(&g_client_mutex);
}
