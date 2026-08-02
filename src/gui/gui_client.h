#ifndef GUI_GUI_CLIENT_H
#define GUI_GUI_CLIENT_H

#include "../platform/ipc_socket.h"
#include <stdbool.h>
#include <stdint.h>

/* Lightweight — one row in the list view. Mirrors IpcDownloadRecord. */
typedef struct {
  uint32_t id;
  char url[IPC_MAX_URL_LEN];
  char dest_path[IPC_MAX_PATH_LEN];
  char status[16]; // always canonical uppercase — gui_client normalizes
                   // every string before anything downstream sees it
  float progress;
} GuiDownloadRecord;

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
} GuiClientEvent;

bool gui_client_connect(void);
void gui_client_disconnect(void);

bool gui_client_pause(uint32_t id);
bool gui_client_resume(uint32_t id);
bool gui_client_cancel(uint32_t id);

bool gui_client_add_download(const char *url, const char *dest,
                             const IpcDownloadOptions *opts, uint32_t *out_id);

// Full snapshot fetch. On success, caller owns *out_records (free() it).
bool gui_client_list_all(GuiDownloadRecord **out_records, int *out_count);

// On-demand single-download details fetch — only call when the user
// actually opens a row's details panel, never in a bulk/periodic loop.
bool gui_client_get_details(uint32_t id, GuiDownloadDetails *out);

bool gui_client_poll_event(GuiClientEvent *out);

#endif
