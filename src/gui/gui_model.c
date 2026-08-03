#include "gui_model.h"
#include "../platform/thread.h"
#include "../utils/log.h"
#include <string.h>

static GuiRow g_rows[GUI_MODEL_MAX_ROWS];
static int g_row_count = 0;
static dm_mutex_t g_mutex;
static bool g_mutex_ready = false;

static void ensure_mutex(void) {
  if (!g_mutex_ready) {
    dm_mutex_init(&g_mutex);
    g_mutex_ready = true;
  }
}

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
  g_row_count = 0;
  for (int i = 0; i < count && g_row_count < GUI_MODEL_MAX_ROWS; i++) {
    GuiRow *row = &g_rows[g_row_count];
    row->id = records[i].id;
    strncpy(row->url, records[i].url, sizeof(row->url) - 1);
    row->url[sizeof(row->url) - 1] = '\0';
    strncpy(row->dest_path, records[i].dest_path, sizeof(row->dest_path) - 1);
    row->dest_path[sizeof(row->dest_path) - 1] = '\0';
    strncpy(row->status, records[i].status, sizeof(row->status) - 1);
    row->status[sizeof(row->status) - 1] = '\0';
    row->progress = records[i].progress;
    g_row_count++;
  }
  dm_mutex_unlock(&g_mutex);
  LOG_DEBUG("loaded %d row(s)", g_row_count);
}

void gui_model_apply_status_update(uint32_t id, const char *status,
                                   float progress) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (int i = 0; i < g_row_count; i++) {
    if (g_rows[i].id == id) {
      g_rows[i].progress = progress;
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
void gui_model_add_local_row(uint32_t id, const char *url) {
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
    row->id = id;
    strncpy(row->url, url, sizeof(row->url) - 1);
    row->url[sizeof(row->url) - 1] = '\0';
    row->dest_path[0] = '\0';
    strncpy(row->status, "QUEUED", sizeof(row->status) - 1);
    row->status[sizeof(row->status) - 1] = '\0';
    row->progress = 0.0f;
    g_row_count++;
  }
  dm_mutex_unlock(&g_mutex);
  LOG_INFO("id=%u url='%s'", id, url ? url : "");
}
