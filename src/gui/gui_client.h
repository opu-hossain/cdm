#ifndef GUI_GUI_CLIENT_H
#define GUI_GUI_CLIENT_H

#include "../core/download_record.h"
#include "../platform/ipc_socket.h"
#include <stdbool.h>
#include <stdint.h>

/* Lightweight list row. Status is canonical uppercase. */
typedef DownloadListRecord GuiDownloadRecord;

/* Heavyweight — fetched on demand, one download at a time. Mirrors
   IpcDownloadDetails. */
typedef struct {
  char cookie[1024];
  char referrer[2048];
  char extra_headers[4096];
  char expected_sha256[65];
  uint64_t speed_limit_bps;
} GuiDownloadDetails;

typedef enum {
  GUI_EVT_STATUS_UPDATE,
  GUI_EVT_CONNECTION_LOST,
  GUI_EVT_CONNECTION_RESTORED,
} GuiClientEventType;

typedef struct {
  GuiClientEventType type;
  uint32_t download_id; // valid only for GUI_EVT_STATUS_UPDATE
  char status[16];
  float progress;
  bool has_v2;
  IpcProgressV2 v2;
} GuiClientEvent;

bool gui_client_connect(void);
void gui_client_disconnect(void);

bool gui_client_pause(uint32_t id);
bool gui_client_resume(uint32_t id);
bool gui_client_cancel(uint32_t id);

bool gui_client_add_download(const char *url, const char *dest,
                             const IpcDownloadOptions *opts, uint32_t *out_id);
bool gui_client_add_download_auto(const char *url, const char *dest,
                                  const IpcDownloadOptions *opts,
                                  uint32_t *out_id);
bool gui_client_add_download_result(const char *url, const char *dest,
                                    const IpcDownloadOptions *opts,
                                    bool auto_filename, uint32_t *out_id,
                                    bool *duplicate);
bool gui_client_reload_config(void);

/* Fetch a full snapshot. The caller owns *out_records on success. */
bool gui_client_list_all(GuiDownloadRecord **out_records, int *out_count);
/* Fetch a bounded history window on the controller thread. Caller frees rows. */
bool gui_client_list_page(uint32_t offset, uint32_t limit,
                          GuiDownloadRecord **out_records, int *out_count,
                          uint32_t *out_total);

/* Fetch details only when the user opens a row's details panel. */
bool gui_client_get_details(uint32_t id, GuiDownloadDetails *out);

bool gui_client_poll_event(GuiClientEvent *out);

#endif
