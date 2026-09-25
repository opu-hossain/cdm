#include "gui_client.h"
#include "../platform/ipc_socket.h"
#include "../platform/thread.h"
#include "../utils//log.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>

#define GUI_CMD_TIMEOUT_MS 5000
#define GUI_EVENT_QUEUE_CAP 512

static int g_cmd_fd = -1;
static bool g_cmd_page_supported = false;
static uint16_t g_cmd_version = 1;
static int g_listener_fd = -1;
static dm_thread_t g_listener_thread;
static bool g_listener_started = false;
static _Atomic bool g_running = false;
static _Atomic bool g_was_connected = true;

static GuiClientEvent g_queue[GUI_EVENT_QUEUE_CAP];
static int g_queue_head = 0, g_queue_tail = 0, g_queue_count = 0;
static dm_mutex_t g_queue_mutex;
static once_flag g_queue_mutex_once = ONCE_FLAG_INIT;

static void initialize_queue_mutex(void) { dm_mutex_init(&g_queue_mutex); }

static void ensure_queue_mutex(void) {
  call_once(&g_queue_mutex_once, initialize_queue_mutex);
}

static void push_event(GuiClientEvent evt) {
  ensure_queue_mutex();
  dm_mutex_lock(&g_queue_mutex);
  if (g_queue_count == GUI_EVENT_QUEUE_CAP) {
    // Drop oldest — a 512-deep queue at a ~5/sec broadcast rate, drained
    // every frame at 60fps, should never realistically fill. If this ever
    // fires it means the GUI thread stalled badly; losing a status update
    // is far better than blocking the listener thread on a full queue.
    g_queue_head = (g_queue_head + 1) % GUI_EVENT_QUEUE_CAP;
    g_queue_count--;
    LOG_WARN("gui_client: event queue full, dropping oldest event");
  }
  g_queue[g_queue_tail] = evt;
  g_queue_tail = (g_queue_tail + 1) % GUI_EVENT_QUEUE_CAP;
  g_queue_count++;
  dm_mutex_unlock(&g_queue_mutex);
}

bool gui_client_poll_event(GuiClientEvent *out) {
  ensure_queue_mutex();
  dm_mutex_lock(&g_queue_mutex);
  bool has_event = g_queue_count > 0;
  if (has_event) {
    *out = g_queue[g_queue_head];
    g_queue_head = (g_queue_head + 1) % GUI_EVENT_QUEUE_CAP;
    g_queue_count--;
  }
  dm_mutex_unlock(&g_queue_mutex);
  return has_event;
}

// Maps whatever text scheduler.c's ipc_broadcast_status() sends (mixed
// case — "Paused", "Downloading", "Retrying", "Canceled", "Error",
// "Verification failed", "Done") onto the canonical uppercase set that
// MSG_LIST_ALL / the DB already use, so nothing downstream ever needs to
// do case-insensitive comparisons again.
static void normalize_status(const char *raw, char *out, size_t out_size) {
  struct {
    const char *raw;
    const char *canonical;
  } map[] = {
      {"Downloading", "ACTIVE"}, {"Retrying", "QUEUED"},
      {"Paused", "PAUSED"},      {"Canceled", "CANCELED"},
      {"Error", "ERROR"},        {"Verification failed", "ERROR"},
      {"Done", "DONE"},
  };
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    if (strcmp(raw, map[i].raw) == 0) {
      strncpy(out, map[i].canonical, out_size - 1);
      out[out_size - 1] = '\0';
      return;
    }
  }
  // Unrecognized text (e.g. it already arrived uppercase) — just uppercase
  // it as a safety net rather than dropping the update silently.
  size_t i;
  for (i = 0; raw[i] && i < out_size - 1; i++) {
    char c = raw[i];
    out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
  }
  out[i] = '\0';
}

static int read_string_local(int fd, char *out, size_t max_len) {
  uint32_t len = 0;
  if (ipc_read_exact(fd, &len, sizeof(len)) != 0)
    return -1;
  if (len >= max_len)
    len = (uint32_t)(max_len - 1);
  if (len > 0 && ipc_read_exact(fd, out, len) != 0)
    return -1;
  out[len] = '\0';
  return 0;
}

static bool drain_payload(int fd, uint32_t length) {
  char discard[256];
  while (length > 0) {
    size_t chunk = length < sizeof(discard) ? length : sizeof(discard);
    if (ipc_read_exact(fd, discard, chunk) != 0)
      return false;
    length -= (uint32_t)chunk;
  }
  return true;
}

static void subscribe_listener(int fd, uint16_t version) {
  if (version >= 2)
    ipc_send_subscribe_v2(fd);
  else
    ipc_send_subscribe(fd);
}

static int listener_thread_fn(void *arg) {
  (void)arg;
  bool skip_v1_fallback = false;
  while (atomic_load(&g_running)) {
    MsgHeader hdr;
    if (ipc_read_exact(g_listener_fd, &hdr, sizeof(hdr)) != 0) {
      if (atomic_exchange(&g_was_connected, false)) {
        push_event((GuiClientEvent){.type = GUI_EVT_CONNECTION_LOST});
      }
      if (!atomic_load(&g_running))
        break;
      dm_thread_sleep_ms(1000);
      if (g_listener_fd >= 0)
        ipc_client_disconnect(g_listener_fd);
      uint16_t version = 1;
      g_listener_fd = ipc_client_connect_compatible(-1, &version);
      if (g_listener_fd >= 0) {
        subscribe_listener(g_listener_fd, version);
        skip_v1_fallback = false;
        if (!atomic_exchange(&g_was_connected, true)) {
          push_event((GuiClientEvent){.type = GUI_EVT_CONNECTION_RESTORED});
        }
      }
      continue;
    }

    if (hdr.length > IPC_MAX_FRAME_SIZE) {
      ipc_client_disconnect(g_listener_fd);
      g_listener_fd = -1;
      continue;
    }
    if (hdr.type == MSG_STATUS_EVENT_V2 &&
        hdr.length == sizeof(IpcProgressV2)) {
      IpcProgressV2 rich;
      if (ipc_read_exact(g_listener_fd, &rich, sizeof(rich)) != 0) {
        ipc_client_disconnect(g_listener_fd);
        g_listener_fd = -1;
        continue;
      }
      rich.status[sizeof(rich.status) - 1] = '\0';
      rich.error[sizeof(rich.error) - 1] = '\0';
      GuiClientEvent evt = {.type = GUI_EVT_STATUS_UPDATE,
                            .download_id = rich.download_id,
                            .progress = rich.progress,
                            .has_v2 = true,
                            .v2 = rich};
      normalize_status(rich.status, evt.status, sizeof(evt.status));
      push_event(evt);
      skip_v1_fallback = true;
      continue;
    }
    if (hdr.type != MSG_STATUS_EVENT || skip_v1_fallback) {
      if (!drain_payload(g_listener_fd, hdr.length)) {
        ipc_client_disconnect(g_listener_fd);
        g_listener_fd = -1;
      }
      skip_v1_fallback = false;
      continue;
    }

    uint32_t id;
    float progress;
    char raw_status[32] = {0};

    ipc_read_exact(g_listener_fd, &id, sizeof(id));
    ipc_read_exact(g_listener_fd, &progress, sizeof(progress));

    uint32_t slen = 0;
    ipc_read_exact(g_listener_fd, &slen, sizeof(slen));
    if (slen > 0) {
      if (slen < sizeof(raw_status)) {
        ipc_read_exact(g_listener_fd, raw_status, slen);
        raw_status[slen] = '\0';
      } else {
        char discard[256];
        uint32_t remaining = slen;
        while (remaining > 0) {
          uint32_t chunk =
              remaining < sizeof(discard) ? remaining : sizeof(discard);
          if (ipc_read_exact(g_listener_fd, discard, chunk) != 0)
            break;
          remaining -= chunk;
        }
      }
    }

    GuiClientEvent evt = {
        .type = GUI_EVT_STATUS_UPDATE, .download_id = id, .progress = progress};
    if (raw_status[0]) {
      normalize_status(raw_status, evt.status, sizeof(evt.status));
    } else {
      evt.status[0] = '\0';
    }
    push_event(evt);
  }
  return 0;
}

bool gui_client_connect(void) {
  uint16_t command_version = 1;
  g_cmd_fd = ipc_client_connect_compatible(GUI_CMD_TIMEOUT_MS,
                                             &command_version);
  if (g_cmd_fd < 0)
    return false;
  g_cmd_version = command_version;
  g_cmd_page_supported = command_version >= 2;

  uint16_t version = 1;
  g_listener_fd = ipc_client_connect_compatible(-1, &version);
  if (g_listener_fd < 0) {
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
    return false;
  }
  subscribe_listener(g_listener_fd, version);

  atomic_store(&g_running, true);
  atomic_store(&g_was_connected, true);
  if (dm_thread_create(&g_listener_thread, listener_thread_fn, NULL) != 0) {
    ipc_client_disconnect(g_listener_fd);
    ipc_client_disconnect(g_cmd_fd);
    g_listener_fd = -1;
    g_cmd_fd = -1;
    atomic_store(&g_running, false);
    return false;
  }
  g_listener_started = true;
  return true;
}

void gui_client_disconnect(void) {
  atomic_store(&g_running, false);
  if (g_listener_fd >= 0)
    shutdown(g_listener_fd, SHUT_RDWR);
  if (g_listener_started) {
    dm_thread_join(&g_listener_thread, NULL);
    g_listener_started = false;
  }
  if (g_cmd_fd >= 0) {
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
  }
  if (g_listener_fd >= 0) {
    ipc_client_disconnect(g_listener_fd);
    g_listener_fd = -1;
  }
}

static void note_lost(void) {
  if (atomic_exchange(&g_was_connected, false)) {
    push_event((GuiClientEvent){.type = GUI_EVT_CONNECTION_LOST});
  }
}
static void note_restored(void) {
  if (!atomic_exchange(&g_was_connected, true)) {
    push_event((GuiClientEvent){.type = GUI_EVT_CONNECTION_RESTORED});
  }
}

typedef int (*FireAndForgetFn)(int sock, uint32_t id);

static bool send_with_retry(FireAndForgetFn fn, uint32_t id) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (g_cmd_fd < 0) {
      g_cmd_fd = ipc_client_connect_compatible(GUI_CMD_TIMEOUT_MS,
                                               &g_cmd_version);
      if (g_cmd_fd < 0)
        continue;
      note_restored();
    }
    if (fn(g_cmd_fd, id) == 0)
      return true;

    // Write failed — the socket is dead or desynced. Drop it; the next
    // loop iteration (or the next call entirely) reconnects fresh.
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
  }
  note_lost();
  return false;
}

bool gui_client_pause(uint32_t id) {
  return send_with_retry(ipc_send_pause, id);
}
bool gui_client_resume(uint32_t id) {
  return send_with_retry(ipc_send_resume, id);
}
bool gui_client_cancel(uint32_t id) {
  return send_with_retry(ipc_send_cancel, id);
}

bool gui_client_remove_download(uint32_t id, bool delete_file) {
  if (g_cmd_fd < 0) {
    g_cmd_fd = ipc_client_connect_compatible(GUI_CMD_TIMEOUT_MS,
                                              &g_cmd_version);
    if (g_cmd_fd < 0) {
      note_lost();
      return false;
    }
    note_restored();
  }
  if (g_cmd_version < 4)
    return false;
  IpcResult result = IPC_RESULT_ERROR;
  if (ipc_send_remove_download(g_cmd_fd, id, delete_file, &result) != 0) {
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
    note_lost();
    return false;
  }
  return result == IPC_RESULT_OK;
}

static int send_reload_config(int sock, uint32_t id) {
  (void)id;
  return ipc_send_reload_config(sock);
}

bool gui_client_reload_config(void) {
  return send_with_retry(send_reload_config, 0);
}

static bool queue_connection(void) {
  if (g_cmd_fd < 0) {
    g_cmd_fd = ipc_client_connect_compatible(GUI_CMD_TIMEOUT_MS,
                                              &g_cmd_version);
    if (g_cmd_fd < 0) {
      note_lost();
      return false;
    }
    note_restored();
  }
  return g_cmd_version >= 5;
}

int gui_client_queue_list(Queue *out, int max) {
  if (!queue_connection())
    return -1;
  int count = ipc_send_queue_list(g_cmd_fd, out, max);
  if (count < 0) {
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
    note_lost();
  }
  return count;
}

bool gui_client_queue_create(const Queue *queue) {
  if (!queue_connection())
    return false;
  uint32_t id = 0;
  return ipc_send_queue_create(g_cmd_fd, queue, &id) == 0;
}

bool gui_client_queue_update(const Queue *queue) {
  return queue_connection() &&
         ipc_send_queue_update(g_cmd_fd, queue) == IPC_RESULT_OK;
}

bool gui_client_queue_delete(uint32_t id) {
  return queue_connection() &&
         ipc_send_queue_delete(g_cmd_fd, id) == IPC_RESULT_OK;
}

bool gui_client_queue_reorder(uint32_t id, int priority) {
  return queue_connection() &&
         ipc_send_queue_reorder(g_cmd_fd, id, priority) == IPC_RESULT_OK;
}

bool gui_client_add_download_result(const char *url, const char *dest,
                                    const IpcDownloadOptions *opts,
                                    bool auto_filename, uint32_t *out_id,
                                    bool *duplicate) {
  if (!out_id || !duplicate)
    return false;
  *duplicate = false;
  if (g_cmd_fd < 0) {
    g_cmd_fd = ipc_client_connect_compatible(GUI_CMD_TIMEOUT_MS,
                                              &g_cmd_version);
    if (g_cmd_fd < 0) {
      note_lost();
      return false;
    }
    note_restored();
  }

  if (opts && opts->queue_id && g_cmd_version < 5)
    return false;

  IpcAddResponse response = {0};
  if (g_cmd_version >= 6) {
    if (ipc_send_add_download_v3(g_cmd_fd, url, dest, opts, auto_filename,
                                 &response) != 0)
      response.result = IPC_RESULT_ERROR;
  } else if (g_cmd_version >= 3) {
    if (ipc_send_add_download_v2(g_cmd_fd, url, dest, opts, auto_filename,
                                 &response) != 0)
      response.result = IPC_RESULT_ERROR;
  } else {
    response.id = auto_filename
                      ? ipc_send_add_download_auto(g_cmd_fd, url, dest, opts)
                      : ipc_send_add_download(g_cmd_fd, url, dest, opts);
  }
  // Do not retry an add after an IPC failure: the request may have reached the
  // daemon even if its response was lost, and retrying could create a
  // duplicate.
  *out_id = response.id;
  *duplicate = response.result == IPC_RESULT_REJECTED && response.id != 0;
  if (response.id == 0) {
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
    note_lost();
  }
  return response.id != 0;
}

bool gui_client_add_download(const char *url, const char *dest,
                             const IpcDownloadOptions *opts, uint32_t *out_id) {
  bool duplicate = false;
  return gui_client_add_download_result(url, dest, opts, false, out_id,
                                        &duplicate);
}

bool gui_client_add_download_auto(const char *url, const char *dest,
                                  const IpcDownloadOptions *opts,
                                  uint32_t *out_id) {
  bool duplicate = false;
  return gui_client_add_download_result(url, dest, opts, true, out_id,
                                        &duplicate);
}

bool gui_client_list_all(GuiDownloadRecord **out_records, int *out_count) {
  *out_records = NULL;
  *out_count = 0;

  if (g_cmd_fd < 0) {
    g_cmd_fd = ipc_client_connect_compatible(GUI_CMD_TIMEOUT_MS,
                                             &g_cmd_version);
    if (g_cmd_fd < 0) {
      note_lost();
      return false;
    }
    note_restored();
  }

  MsgHeader hdr = {.length = 0, .type = MSG_LIST_ALL};
  if (ipc_write_exact(g_cmd_fd, &hdr, sizeof(hdr)) != 0) {
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
    note_lost();
    return false;
  }

  uint32_t count = 0;
  if (ipc_read_exact(g_cmd_fd, &count, sizeof(count)) != 0) {
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
    note_lost();
    return false;
  }

  int n = (int)count;
  if (n == 0) {
    *out_records = NULL;
    *out_count = 0;
    return true;
  }

  GuiDownloadRecord *out = calloc((size_t)n, sizeof(GuiDownloadRecord));
  if (!out)
    return false;

  for (int i = 0; i < n; i++) {
    uint32_t id;
    char url[IPC_MAX_URL_LEN];
    char dest_path[IPC_MAX_PATH_LEN];
    char status[16];
    float progress;

    if (ipc_read_exact(g_cmd_fd, &id, sizeof(id)) != 0 ||
        read_string_local(g_cmd_fd, url, sizeof(url)) != 0 ||
        read_string_local(g_cmd_fd, dest_path, sizeof(dest_path)) != 0 ||
        read_string_local(g_cmd_fd, status, sizeof(status)) != 0 ||
        ipc_read_exact(g_cmd_fd, &progress, sizeof(progress)) != 0) {
      free(out);
      ipc_client_disconnect(g_cmd_fd);
      g_cmd_fd = -1;
      note_lost();
      return false;
    }

    out[i].id = id;
    strncpy(out[i].url, url, sizeof(out[i].url) - 1);
    out[i].url[sizeof(out[i].url) - 1] = '\0';
    strncpy(out[i].dest_path, dest_path, sizeof(out[i].dest_path) - 1);
    out[i].dest_path[sizeof(out[i].dest_path) - 1] = '\0';
    strncpy(out[i].status, status, sizeof(out[i].status) - 1);
    out[i].status[sizeof(out[i].status) - 1] = '\0';
    out[i].progress = progress;
  }

  *out_records = out;
  *out_count = n;
  return true;
}

bool gui_client_list_page(uint32_t offset, uint32_t limit,
                          GuiDownloadRecord **out_records, int *out_count,
                          uint32_t *out_total) {
  if (!out_records || !out_count || !out_total || limit == 0)
    return false;
  *out_records = NULL;
  *out_count = 0;
  *out_total = 0;
  if (limit > 256)
    limit = 256;
  if (g_cmd_fd < 0) {
    uint16_t version = 1;
    g_cmd_fd = ipc_client_connect_compatible(GUI_CMD_TIMEOUT_MS, &version);
    if (g_cmd_fd < 0) {
      note_lost();
      return false;
    }
    g_cmd_version = version;
    g_cmd_page_supported = version >= 2;
    note_restored();
  }

  GuiDownloadRecord *rows = calloc(limit, sizeof(*rows));
  if (!rows)
    return false;
  if (g_cmd_page_supported) {
    int count = ipc_send_list_page(g_cmd_fd, offset, limit, rows, (int)limit,
                                   out_total);
    if (count >= 0) {
      *out_records = rows;
      *out_count = count;
      return true;
    }
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
    g_cmd_page_supported = false;
  }

  /* Legacy daemon, including older v2 builds without type 35. */
  free(rows);
  GuiDownloadRecord *legacy = NULL;
  int legacy_count = 0;
  if (!gui_client_list_all(&legacy, &legacy_count))
    return false;
  *out_total = (uint32_t)legacy_count;
  if (offset >= (uint32_t)legacy_count) {
    free(legacy);
    return true;
  }
  int available = legacy_count - (int)offset;
  int count = available < (int)limit ? available : (int)limit;
  memmove(legacy, legacy + offset, (size_t)count * sizeof(*legacy));
  *out_records = legacy;
  *out_count = count;
  return true;
}

bool gui_client_get_details(uint32_t id, GuiDownloadDetails *out) {
  if (g_cmd_fd < 0) {
    g_cmd_fd = ipc_client_connect_compatible(GUI_CMD_TIMEOUT_MS,
                                             &g_cmd_version);
    if (g_cmd_fd < 0) {
      note_lost();
      return false;
    }
    note_restored();
  }

  IpcDownloadDetails raw = {0};
  int rc = ipc_send_get_details(g_cmd_fd, id, &raw);
  if (rc != 0) {
    // Don't tear down g_cmd_fd here — "not found" and "socket error" both
    // return -1 from ipc_send_get_details, and a not-found isn't a
    // connection problem worth reconnecting over.
    return false;
  }

  strncpy(out->cookie, raw.cookie, sizeof(out->cookie) - 1);
  out->cookie[sizeof(out->cookie) - 1] = '\0';
  strncpy(out->referrer, raw.referrer, sizeof(out->referrer) - 1);
  out->referrer[sizeof(out->referrer) - 1] = '\0';
  strncpy(out->extra_headers, raw.extra_headers,
          sizeof(out->extra_headers) - 1);
  out->extra_headers[sizeof(out->extra_headers) - 1] = '\0';
  strncpy(out->expected_sha256, raw.expected_sha256,
          sizeof(out->expected_sha256) - 1);
  out->expected_sha256[sizeof(out->expected_sha256) - 1] = '\0';
  out->speed_limit_bps = raw.speed_limit_bps;
  return true;
}
