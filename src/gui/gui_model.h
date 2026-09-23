#ifndef GUI_GUI_MODEL_H
#define GUI_GUI_MODEL_H

#include "gui_client.h"
#include <stdbool.h>

#define GUI_MODEL_MAX_ROWS 256

/* Lightweight — matches GuiDownloadRecord, no details fields. */
typedef struct {
  uint32_t id;
  char url[IPC_MAX_URL_LEN];
  char dest_path[IPC_MAX_PATH_LEN];
  char status[16];
  float progress;
} GuiRow;

void gui_model_init(void);
void gui_model_apply_snapshot(const GuiDownloadRecord *records, int count);
void gui_model_apply_status_update(uint32_t id, const char *status,
                                   float progress);
void gui_model_apply_optimistic(uint32_t id, const char *new_status);
void gui_model_add_local_row(uint32_t id, const char *url,
                             const char *dest_path);
void gui_model_for_each_row(void (*fn)(const GuiRow *row, void *ctx),
                            void *ctx);

int gui_model_snapshot_rows(GuiRow *out, int max);

#endif
