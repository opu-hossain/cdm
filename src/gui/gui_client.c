#include "gui_client.h"
#include "../platform/ipc_socket.h"
#include "../platform/thread.h"
#include "../utils//log.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define GUI_CMD_TIMEOUT_MS 5000
#define GUI_EVENT_QUEUE_CAP 512
#define GUI_LIST_ALL_FETCH_CAP 256

static int g_cmd_fd = -1;
static int g_listener_fd = -1;
static dm_thread_t g_listener_thread;
static _Atomic bool g_running = false;
static _Atomic bool g_was_connected = true;

static GuiClientEvent g_queue[GUI_EVENT_QUEUE_CAP];
static int g_queue_head = 0, g_queue_tail = 0, g_queue_count = 0;
static dm_mutex_t g_queue_mutex;
static bool g_queue_mutex_ready = false;

static void ensure_queue_mutex(void) {
  if (!g_queue_mutex_ready) {
    dm_mutex_init(&g_queue_mutex);
    g_queue_mutex_ready = true;
  }
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

static int listener_thread_fn(void *arg) {
  (void)arg;
  while (atomic_load(&g_running)) {
    MsgHeader hdr;
    if (ipc_read_exact(g_listener_fd, &hdr, sizeof(hdr)) != 0) {
      if (atomic_exchange(&g_was_connected, false)) {
        push_event((GuiClientEvent){.type = GUI_EVT_CONNECTION_LOST});
      }
      dm_thread_sleep_ms(1000);
      if (g_listener_fd >= 0)
        ipc_client_disconnect(g_listener_fd);
      g_listener_fd = ipc_client_connect();
      if (g_listener_fd >= 0) {
        ipc_send_subscribe(g_listener_fd);
        if (!atomic_exchange(&g_was_connected, true)) {
          push_event((GuiClientEvent){.type = GUI_EVT_CONNECTION_RESTORED});
        }
      }
      continue;
    }

    if (hdr.type != MSG_STATUS_EVENT)
      continue;

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
  g_cmd_fd = ipc_client_connect_timeout(GUI_CMD_TIMEOUT_MS);
  if (g_cmd_fd < 0)
    return false;

  g_listener_fd = ipc_client_connect();
  if (g_listener_fd < 0) {
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
    return false;
  }
  ipc_send_subscribe(g_listener_fd);

  atomic_store(&g_running, true);
  atomic_store(&g_was_connected, true);
  dm_thread_create(&g_listener_thread, listener_thread_fn, NULL);
  dm_thread_detach(&g_listener_thread);
  return true;
}

void gui_client_disconnect(void) {
  atomic_store(&g_running, false);
  if (g_cmd_fd >= 0) {
    ipc_client_disconnect(g_cmd_fd);
    g_cmd_fd = -1;
  }
  if (g_listener_fd >= 0) {
    ipc_client_disconnect(g_listener_fd);
    g_listener_fd = -1;
  }
  // Listener thread is detached and will exit on process teardown — matches
  // this project's existing pattern (no cross-thread cancellation mechanism
  // exists in thread.h), same as the original gui.c's listener thread.
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
      g_cmd_fd = ipc_client_connect_timeout(GUI_CMD_TIMEOUT_MS);
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

bool gui_client_add_download(const char *url, const char *dest,
                             const IpcDownloadOptions *opts, uint32_t *out_id) {
  if (g_cmd_fd < 0) {
    g_cmd_fd = ipc_client_connect_timeout(GUI_CMD_TIMEOUT_MS);
    if (g_cmd_fd < 0) {
      note_lost();
      return false;
    }
    note_restored();
  }

  uint32_t id = ipc_send_add_download(g_cmd_fd, url, dest, opts);
  // id==0 here is ambiguous between "daemon rejected it" and "the
  // read/write itself silently failed" in the current protocol — see the
  // header comment. We deliberately do NOT reconnect-and-retry an add, to
  // avoid a double-submit if the first attempt actually landed.
  *out_id = id;
  return id != 0;
}

bool gui_client_list_all(GuiDownloadRecord **out_records, int *out_count) {
  *out_records = NULL;
  *out_count = 0;

  if (g_cmd_fd < 0) {
    g_cmd_fd = ipc_client_connect_timeout(GUI_CMD_TIMEOUT_MS);
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

bool gui_client_get_details(uint32_t id, GuiDownloadDetails *out) {
  if (g_cmd_fd < 0) {
    g_cmd_fd = ipc_client_connect_timeout(GUI_CMD_TIMEOUT_MS);
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
