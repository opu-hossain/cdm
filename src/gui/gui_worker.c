#include "gui_worker.h"
#include "../platform/thread.h"
#include "../utils/log.h"
#include "gui_client.h"
#include "gui_model.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define GUI_WORKER_CMD_QUEUE_CAP 64
#define GUI_WORKER_SEQ_MAX 64
#define GUI_WORKER_TICK_MS 50
#define GUI_WORKER_REFRESH_TICKS 40 // 50ms * 40 = 2s

typedef enum {
  GUI_CMD_ADD_DOWNLOAD,
  GUI_CMD_PAUSE,
  GUI_CMD_RESUME,
  GUI_CMD_CANCEL,
  GUI_CMD_GET_DETAILS, // NEW
} GuiCmdType;

typedef struct {
  char url[IPC_MAX_URL_LEN];
  char dest_path[IPC_MAX_PATH_LEN];
  char cookie[1024];
  char referrer[2048];
  char extra_headers[4096];
  char expected_sha256[65];
  uint64_t speed_limit_bps;
} AddDownloadPayload;

typedef struct {
  GuiCmdType type;
  char seq[GUI_WORKER_SEQ_MAX];
  uint32_t id; // pause/resume/cancel/get_details
  void *payload;
} GuiCommand;

// Bundled onto the heap and handed to webview_dispatch — freed by the
// dispatch callback once it has run on the UI thread.
typedef struct {
  webview_t w;
  bool has_seq;
  char seq[GUI_WORKER_SEQ_MAX];
  bool ok;
  bool refresh_ui;
  // NEW — for GUI_CMD_GET_DETAILS results only. has_details_payload tells
  // dispatch_result whether to build a details JSON instead of {"ok":...}.
  bool has_details_payload;
  GuiDownloadDetails details;
} DispatchResult;

static webview_t g_webview = NULL;
static dm_thread_t g_thread;
static bool g_thread_started = false;
static _Atomic bool g_running = false;

static GuiCommand g_queue[GUI_WORKER_CMD_QUEUE_CAP];
static int g_q_head = 0, g_q_tail = 0, g_q_count = 0;
static dm_mutex_t g_q_mutex;
static once_flag g_q_mutex_once = ONCE_FLAG_INIT;

static void initialize_q_mutex(void) { dm_mutex_init(&g_q_mutex); }

static void ensure_q_mutex(void) { call_once(&g_q_mutex_once, initialize_q_mutex); }

static bool enqueue(GuiCommand cmd) {
  ensure_q_mutex();
  dm_mutex_lock(&g_q_mutex);
  bool ok = g_q_count < GUI_WORKER_CMD_QUEUE_CAP;
  if (ok) {
    g_queue[g_q_tail] = cmd;
    g_q_tail = (g_q_tail + 1) % GUI_WORKER_CMD_QUEUE_CAP;
    g_q_count++;
  } else {
    LOG_WARN("gui_worker: command queue full, dropping command");
  }
  dm_mutex_unlock(&g_q_mutex);
  return ok;
}

static bool dequeue(GuiCommand *out) {
  ensure_q_mutex();
  dm_mutex_lock(&g_q_mutex);
  bool has = g_q_count > 0;
  if (has) {
    *out = g_queue[g_q_head];
    g_q_head = (g_q_head + 1) % GUI_WORKER_CMD_QUEUE_CAP;
    g_q_count--;
  }
  dm_mutex_unlock(&g_q_mutex);
  return has;
}

static void copy_seq(char *out, const char *seq) {
  if (!seq) {
    out[0] = '\0';
    return;
  }
  strncpy(out, seq, GUI_WORKER_SEQ_MAX - 1);
  out[GUI_WORKER_SEQ_MAX - 1] = '\0';
}

static void free_command_payload(GuiCommand *cmd) {
  if (cmd && cmd->type == GUI_CMD_ADD_DOWNLOAD && cmd->payload) {
    free(cmd->payload);
    cmd->payload = NULL;
  }
}

void gui_worker_enqueue_add_download(const char *seq, const char *url,
                                     const char *dest_path, const char *cookie,
                                     const char *referrer,
                                     const char *extra_headers,
                                     const char *expected_sha256,
                                     uint64_t speed_limit_bps) {
  AddDownloadPayload *payload = calloc(1, sizeof(AddDownloadPayload));
  if (!payload) {
    if (seq)
      webview_return(g_webview, seq, 0, "{\"ok\":false}");
    return;
  }

  strncpy(payload->url, url, sizeof(payload->url) - 1);
  strncpy(payload->dest_path, dest_path, sizeof(payload->dest_path) - 1);
  if (cookie)
    strncpy(payload->cookie, cookie, sizeof(payload->cookie) - 1);
  if (referrer)
    strncpy(payload->referrer, referrer, sizeof(payload->referrer) - 1);
  if (extra_headers)
    strncpy(payload->extra_headers, extra_headers,
            sizeof(payload->extra_headers) - 1);
  if (expected_sha256)
    strncpy(payload->expected_sha256, expected_sha256,
            sizeof(payload->expected_sha256) - 1);
  payload->speed_limit_bps = speed_limit_bps;

  GuiCommand cmd = {0};
  cmd.type = GUI_CMD_ADD_DOWNLOAD;
  copy_seq(cmd.seq, seq);
  cmd.payload = payload;

  if (!enqueue(cmd) && seq) {
    free_command_payload(&cmd);
    webview_return(g_webview, seq, 0, "{\"ok\":false}");
  }
}

static void enqueue_id_command(GuiCmdType type, const char *seq, uint32_t id) {
  GuiCommand cmd = {0};
  cmd.type = type;
  copy_seq(cmd.seq, seq);
  cmd.id = id;
  if (!enqueue(cmd) && seq) {
    webview_return(g_webview, seq, 0, "{\"ok\":false}");
  }
}

void gui_worker_enqueue_pause(const char *seq, uint32_t id) {
  enqueue_id_command(GUI_CMD_PAUSE, seq, id);
}
void gui_worker_enqueue_resume(const char *seq, uint32_t id) {
  enqueue_id_command(GUI_CMD_RESUME, seq, id);
}
void gui_worker_enqueue_cancel(const char *seq, uint32_t id) {
  enqueue_id_command(GUI_CMD_CANCEL, seq, id);
}
void gui_worker_enqueue_get_details(const char *seq, uint32_t id) {
  enqueue_id_command(GUI_CMD_GET_DETAILS, seq, id);
}

// Runs on the UI thread, via webview_dispatch. This is the ONLY place
// webview_return / push_state_to_ui get called from now.
static void dispatch_result(webview_t w, void *arg) {
  DispatchResult *r = (DispatchResult *)arg;

  if (r->has_seq) {
    if (r->has_details_payload) {
      // Build a small JSON object with the details fields. cJSON isn't
      // pulled in here to keep gui_worker.c decoupled from it — this is a
      // handful of fields, hand-building keeps the dependency list small.
      extern void gui_return_details_json(webview_t w, const char *seq,
                                          const GuiDownloadDetails *d);
      gui_return_details_json(w, r->seq, &r->details);
    } else {
      char result[64];
      snprintf(result, sizeof(result), "{\"ok\":%s}", r->ok ? "true" : "false");
      webview_return(w, r->seq, 0, result);
    }
  }
  if (r->refresh_ui) {
    extern void gui_push_state_to_ui(webview_t w);
    gui_push_state_to_ui(w);
  }
  free(r);
}

static void schedule_dispatch(bool has_seq, const char *seq, bool ok,
                              bool refresh_ui) {
  DispatchResult *r = calloc(1, sizeof(DispatchResult));
  if (!r)
    return;
  r->w = g_webview;
  r->has_seq = has_seq;
  if (has_seq) {
    strncpy(r->seq, seq, sizeof(r->seq) - 1);
    r->seq[sizeof(r->seq) - 1] = '\0';
  }
  r->ok = ok;
  r->refresh_ui = refresh_ui;
  webview_dispatch(g_webview, dispatch_result, r);
}

static void schedule_details_dispatch(const char *seq,
                                      const GuiDownloadDetails *details) {
  DispatchResult *r = calloc(1, sizeof(DispatchResult));
  if (!r)
    return;
  r->w = g_webview;
  r->has_seq = true;
  strncpy(r->seq, seq, sizeof(r->seq) - 1);
  r->seq[sizeof(r->seq) - 1] = '\0';
  r->has_details_payload = true;
  r->details = *details;
  r->refresh_ui = false;
  webview_dispatch(g_webview, dispatch_result, r);
}

static void process_command(const GuiCommand *cmd) {
  bool has_seq = cmd->seq[0] != '\0';
  bool ok = false;

  switch (cmd->type) {
  case GUI_CMD_ADD_DOWNLOAD: {
    const AddDownloadPayload *payload = (const AddDownloadPayload *)cmd->payload;
    if (!payload) {
      LOG_WARN("gui_worker: add_download missing payload");
      schedule_dispatch(has_seq, cmd->seq, false, /*refresh_ui=*/false);
      return;
    }

    LOG_INFO("gui_worker: processing add_download url='%s' dest='%s'",
             payload->url, payload->dest_path);

    IpcDownloadOptions opts = {
        .cookie = payload->cookie[0] ? payload->cookie : NULL,
        .referrer = payload->referrer[0] ? payload->referrer : NULL,
        .extra_headers = payload->extra_headers[0] ? payload->extra_headers : NULL,
        .expected_sha256 =
            payload->expected_sha256[0] ? payload->expected_sha256 : NULL,
        .speed_limit_bps = payload->speed_limit_bps,
    };
    bool has_options = opts.cookie || opts.referrer || opts.extra_headers ||
                       opts.expected_sha256 || opts.speed_limit_bps > 0;

    uint32_t id = 0;
    ok = gui_client_add_download(payload->url, payload->dest_path,
                                 has_options ? &opts : NULL, &id) &&
         id > 0;
    LOG_INFO("gui_worker: add_download result ok=%d id=%u", ok, id);
    if (ok)
      gui_model_add_local_row(id, payload->url);
    schedule_dispatch(has_seq, cmd->seq, ok, /*refresh_ui=*/true);
    return;
  }
  case GUI_CMD_PAUSE:
    ok = gui_client_pause(cmd->id);
    if (ok)
      gui_model_apply_optimistic(cmd->id, "PAUSED");
    schedule_dispatch(has_seq, cmd->seq, ok, /*refresh_ui=*/true);
    return;
  case GUI_CMD_RESUME:
    ok = gui_client_resume(cmd->id);
    if (ok)
      gui_model_apply_optimistic(cmd->id, "QUEUED");
    schedule_dispatch(has_seq, cmd->seq, ok, /*refresh_ui=*/true);
    return;
  case GUI_CMD_CANCEL:
    ok = gui_client_cancel(cmd->id);
    if (ok)
      gui_model_apply_optimistic(cmd->id, "CANCELED");
    schedule_dispatch(has_seq, cmd->seq, ok, /*refresh_ui=*/true);
    return;
  case GUI_CMD_GET_DETAILS: {
    // NEW — one-off fetch, no model mutation, no forced refresh_ui: this
    // must not trigger the same "rebuild the whole list" cost as every
    // other command, since it can be fired per-row on user click.
    GuiDownloadDetails details = {0};
    bool found = gui_client_get_details(cmd->id, &details);
    if (has_seq) {
      if (found) {
        schedule_details_dispatch(cmd->seq, &details);
      } else {
        webview_return(g_webview, cmd->seq, 0, "{\"ok\":false}");
      }
    }
    return;
  }
  }
}

static int worker_thread_fn(void *arg) {
  (void)arg;
  int ticks_since_refresh = 0;

  while (atomic_load(&g_running)) {
    bool did_work = false;

    GuiClientEvent evt;
    while (gui_client_poll_event(&evt)) {
      if (evt.type == GUI_EVT_STATUS_UPDATE) {
        gui_model_apply_status_update(evt.download_id, evt.status,
                                      evt.progress);
        did_work = true;
      }
      // GUI_EVT_CONNECTION_LOST/RESTORED handled via gui_push_connection_state,
      // wired in gui.c already.
    }

    if (++ticks_since_refresh >= GUI_WORKER_REFRESH_TICKS) {
      ticks_since_refresh = 0;
      GuiDownloadRecord *records = NULL;
      int count = 0;
      if (gui_client_list_all(&records, &count)) {
        gui_model_apply_snapshot(records, count);
        free(records);
        did_work = true;
      }
    }

    if (did_work) {
      schedule_dispatch(/*has_seq=*/false, NULL, /*ok=*/true,
                        /*refresh_ui=*/true);
    }

    GuiCommand cmd;
    while (dequeue(&cmd)) {
      process_command(&cmd);
      free_command_payload(&cmd);
    }

    dm_thread_sleep_ms(GUI_WORKER_TICK_MS);
  }
  return 0;
}

void gui_worker_start(webview_t w) {
  g_webview = w;
  atomic_store(&g_running, true);
  if (dm_thread_create(&g_thread, worker_thread_fn, NULL) == 0)
    g_thread_started = true;
  else
    atomic_store(&g_running, false);
}

void gui_worker_stop(void) {
  atomic_store(&g_running, false);
  if (g_thread_started) {
    dm_thread_join(&g_thread, NULL);
    g_thread_started = false;
  }
  g_webview = NULL;
}
