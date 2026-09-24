#include "gui_model.h"
#include "../platform/thread.h"
#include "../utils/log.h"
#include <stdio.h>
#include <string.h>
#include <threads.h>

static GuiRow g_rows[GUI_MODEL_MAX_ROWS];
static int g_row_count = 0;
static dm_mutex_t g_mutex;
static once_flag g_mutex_once = ONCE_FLAG_INIT;

static void initialize_mutex(void) { dm_mutex_init(&g_mutex); }

static void ensure_mutex(void) { call_once(&g_mutex_once, initialize_mutex); }

void gui_model_init(void) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  g_row_count = 0;
  dm_mutex_unlock(&g_mutex);
  LOG_DEBUG("cleared snapshot state");
}

void gui_model_apply_snapshot(const GuiDownloadRecord *records, int count) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  struct {
    uint32_t id;
    float progress;
    bool has_v2;
    uint64_t bytes_received, total_bytes, speed_bps, eta_seconds;
    char status[16], error[256];
  } saved[GUI_MODEL_MAX_ROWS];
  int old_count = g_row_count;
  for (int i = 0; i < old_count; i++) {
    saved[i].id = g_rows[i].id;
    saved[i].progress = g_rows[i].progress;
    saved[i].has_v2 = g_rows[i].has_v2;
    saved[i].bytes_received = g_rows[i].bytes_received;
    saved[i].total_bytes = g_rows[i].total_bytes;
    saved[i].speed_bps = g_rows[i].speed_bps;
    saved[i].eta_seconds = g_rows[i].eta_seconds;
    memcpy(saved[i].status, g_rows[i].status, sizeof(saved[i].status));
    memcpy(saved[i].error, g_rows[i].error, sizeof(saved[i].error));
  }
  g_row_count = 0;
  for (int i = 0; i < count && g_row_count < GUI_MODEL_MAX_ROWS; i++) {
    GuiRow *row = &g_rows[g_row_count];
    memset(row, 0, sizeof(*row));
    row->id = records[i].id;
    strncpy(row->url, records[i].url, sizeof(row->url) - 1);
    row->url[sizeof(row->url) - 1] = '\0';
    strncpy(row->dest_path, records[i].dest_path, sizeof(row->dest_path) - 1);
    row->dest_path[sizeof(row->dest_path) - 1] = '\0';
    strncpy(row->status, records[i].status, sizeof(row->status) - 1);
    row->status[sizeof(row->status) - 1] = '\0';
    row->progress = records[i].progress;
    for (int j = 0; j < old_count; j++) {
      if (saved[j].id != row->id || !saved[j].has_v2)
        continue;
      row->has_v2 = true;
      row->bytes_received = saved[j].bytes_received;
      row->total_bytes = saved[j].total_bytes;
      row->speed_bps = saved[j].speed_bps;
      row->eta_seconds = saved[j].eta_seconds;
      memcpy(row->error, saved[j].error, sizeof(row->error));
      if (strcmp(row->status, saved[j].status) == 0)
        row->progress = saved[j].progress;
      break;
    }
    g_row_count++;
  }
  dm_mutex_unlock(&g_mutex);
  LOG_DEBUG("loaded %d row(s)", g_row_count);
}

void gui_model_apply_status_update_v2(uint32_t id, const char *status,
                                      const IpcProgressV2 *progress) {
  if (!progress)
    return;
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (int i = 0; i < g_row_count; i++) {
    GuiRow *row = &g_rows[i];
    if (row->id != id)
      continue;
    row->progress = progress->progress;
    if (status && status[0]) {
      strncpy(row->status, status, sizeof(row->status) - 1);
      row->status[sizeof(row->status) - 1] = '\0';
    }
    row->has_v2 = true;
    row->bytes_received = progress->bytes_received;
    row->total_bytes = progress->total_bytes;
    row->speed_bps = progress->speed_bps;
    row->eta_seconds = progress->eta_seconds;
    memcpy(row->error, progress->error, sizeof(row->error));
    row->error[sizeof(row->error) - 1] = '\0';
    break;
  }
  dm_mutex_unlock(&g_mutex);
}

void gui_format_bytes(uint64_t bytes, char *out, size_t capacity) {
  if (!out || capacity == 0)
    return;
  static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
  double value = (double)bytes;
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
    value /= 1024.0;
    unit++;
  }
  if (unit == 0)
    snprintf(out, capacity, "%llu B", (unsigned long long)bytes);
  else
    snprintf(out, capacity, "%.1f %s", value, units[unit]);
}

void gui_format_eta(uint64_t seconds, char *out, size_t capacity) {
  if (!out || capacity == 0)
    return;
  if (seconds == UINT64_MAX)
    snprintf(out, capacity, "--:--");
  else if (seconds < 3600)
    snprintf(out, capacity, "%02llu:%02llu",
             (unsigned long long)(seconds / 60),
             (unsigned long long)(seconds % 60));
  else
    snprintf(out, capacity, "%02llu:%02llu:%02llu",
             (unsigned long long)(seconds / 3600),
             (unsigned long long)((seconds / 60) % 60),
             (unsigned long long)(seconds % 60));
}

void gui_model_apply_status_update(uint32_t id, const char *status,
                                   float progress) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (int i = 0; i < g_row_count; i++) {
    if (g_rows[i].id == id) {
      g_rows[i].progress = progress;
      g_rows[i].has_v2 = false;
      g_rows[i].bytes_received = 0;
      g_rows[i].total_bytes = 0;
      g_rows[i].speed_bps = 0;
      g_rows[i].eta_seconds = UINT64_MAX;
      g_rows[i].error[0] = '\0';
      if (status[0]) {
        strncpy(g_rows[i].status, status, sizeof(g_rows[i].status) - 1);
        g_rows[i].status[sizeof(g_rows[i].status) - 1] = '\0';
      }
      break;
    }
  }
  dm_mutex_unlock(&g_mutex);
  LOG_DEBUG("id=%u progress=%.3f status='%s'", id, progress,
            status ? status : "");
}

void gui_model_apply_optimistic(uint32_t id, const char *new_status) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (int i = 0; i < g_row_count; i++) {
    if (g_rows[i].id == id) {
      strncpy(g_rows[i].status, new_status, sizeof(g_rows[i].status) - 1);
      g_rows[i].status[sizeof(g_rows[i].status) - 1] = '\0';
      break;
    }
  }
  dm_mutex_unlock(&g_mutex);
  LOG_DEBUG("id=%u status='%s'", id, new_status ? new_status : "");
}

void gui_model_for_each_row(void (*fn)(const GuiRow *row, void *ctx),
                            void *ctx) {
  if (!fn)
    return;

  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (int i = 0; i < g_row_count; i++) {
    fn(&g_rows[i], ctx);
  }
  dm_mutex_unlock(&g_mutex);
}

int gui_model_snapshot_rows(GuiRow *out, int max) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  int n = g_row_count < max ? g_row_count : max;
  memcpy(out, g_rows, (size_t)n * sizeof(GuiRow));
  dm_mutex_unlock(&g_mutex);
  return n;
}

// Called by main_view_draw() to add a brand-new row immediately after a
// successful add-download, so it's visible before the next periodic
// refresh. Declared here rather than in the header on purpose — this is
// an implementation seam specific to the "just added" UX, not part of
// the model's general contract.
void gui_model_add_local_row(uint32_t id, const char *url,
                             const char *dest_path) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  if (g_row_count < GUI_MODEL_MAX_ROWS) {
    // Shift everything down to make room at the front — new downloads
    // should appear at the top immediately, matching the ORDER BY id DESC
    // (newest first) that the next periodic MSG_LIST_ALL refresh will
    // also produce, so there's no visible reshuffle a moment later.
    for (int i = g_row_count; i > 0; i--) {
      g_rows[i] = g_rows[i - 1];
    }
    GuiRow *row = &g_rows[0];
    memset(row, 0, sizeof(*row));
    row->id = id;
    strncpy(row->url, url, sizeof(row->url) - 1);
    row->url[sizeof(row->url) - 1] = '\0';
    strncpy(row->dest_path, dest_path ? dest_path : "",
            sizeof(row->dest_path) - 1);
    row->dest_path[sizeof(row->dest_path) - 1] = '\0';
    strncpy(row->status, "QUEUED", sizeof(row->status) - 1);
    row->status[sizeof(row->status) - 1] = '\0';
    row->progress = 0.0f;
    g_row_count++;
  }
  dm_mutex_unlock(&g_mutex);
  LOG_INFO("id=%u url='%s'", id, url ? url : "");
}
