#ifndef GUI_GUI_MODEL_H
#define GUI_GUI_MODEL_H

#include "gui_client.h"
#include <stdbool.h>
#include <stddef.h>

#define GUI_MODEL_MAX_ROWS 256
#define GUI_MODEL_MAX_QUEUES 64

/* Lightweight — matches GuiDownloadRecord, no details fields. */
typedef struct {
  uint32_t id;
  char url[IPC_MAX_URL_LEN];
  char dest_path[IPC_MAX_PATH_LEN];
  char status[16];
  float progress;
  bool has_v2;
  uint64_t bytes_received;
  uint64_t total_bytes;
  uint64_t speed_bps;
  uint64_t eta_seconds;
  char error[256];
} GuiRow;

void gui_model_init(void);
void gui_model_apply_snapshot(const GuiDownloadRecord *records, int count);
void gui_model_apply_status_update(uint32_t id, const char *status,
                                   float progress);
void gui_model_apply_status_update_v2(uint32_t id, const char *status,
                                      const IpcProgressV2 *progress);
void gui_format_bytes(uint64_t bytes, char *out, size_t capacity);
void gui_format_eta(uint64_t seconds, char *out, size_t capacity);
void gui_model_apply_optimistic(uint32_t id, const char *new_status);
void gui_model_add_local_row(uint32_t id, const char *url,
                             const char *dest_path);
void gui_model_remove_local_row(uint32_t id);
void gui_model_for_each_row(void (*fn)(const GuiRow *row, void *ctx),
                            void *ctx);

int gui_model_snapshot_rows(GuiRow *out, int max);
void gui_model_apply_queues(const Queue *queues, int count);
int gui_model_snapshot_queues(Queue *out, int max);
bool gui_schedule_valid(const char *start, const char *stop);

#endif
