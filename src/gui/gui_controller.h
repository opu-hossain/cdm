#ifndef GUI_GUI_CONTROLLER_H
#define GUI_GUI_CONTROLLER_H

#include "gui_client.h"
#include <stdbool.h>
#include <stdint.h>

#define GUI_CONTROLLER_MAX_ROWS 256

typedef enum {
  GUI_CONTROLLER_EVENT_OPERATION,
  GUI_CONTROLLER_EVENT_STATUS,
  GUI_CONTROLLER_EVENT_SNAPSHOT,
  GUI_CONTROLLER_EVENT_DETAILS,
  GUI_CONTROLLER_EVENT_CONNECTION,
  GUI_CONTROLLER_EVENT_ERROR,
} GuiControllerEventType;

typedef enum {
  GUI_CONTROLLER_OPERATION_ADD,
  GUI_CONTROLLER_OPERATION_PAUSE,
  GUI_CONTROLLER_OPERATION_RESUME,
  GUI_CONTROLLER_OPERATION_CANCEL,
} GuiControllerOperation;

typedef struct {
  GuiControllerOperation operation;
  uint32_t download_id;
  char url[IPC_MAX_URL_LEN];
  char dest_path[IPC_MAX_PATH_LEN];
  bool succeeded;
} GuiControllerOperationEvent;

typedef struct {
  uint32_t download_id;
  char status[16];
  float progress;
} GuiControllerStatusEvent;

typedef struct {
  GuiDownloadRecord records[GUI_CONTROLLER_MAX_ROWS];
  int count;
} GuiControllerSnapshotEvent;

typedef struct {
  uint32_t download_id;
  bool found;
  GuiDownloadDetails details;
} GuiControllerDetailsEvent;

typedef struct {
  bool connected;
} GuiControllerConnectionEvent;

typedef struct {
  char message[256];
} GuiControllerErrorEvent;

typedef struct {
  GuiControllerEventType type;
  union {
    GuiControllerOperationEvent operation;
    GuiControllerStatusEvent status;
    GuiControllerSnapshotEvent snapshot;
    GuiControllerDetailsEvent details;
    GuiControllerConnectionEvent connection;
    GuiControllerErrorEvent error;
  } data;
} GuiControllerEvent;

void gui_controller_init(void);
bool gui_controller_start(void);
void gui_controller_stop(void);
void gui_controller_shutdown(void);
bool gui_controller_publish(const GuiControllerEvent *event);
bool gui_controller_poll(GuiControllerEvent *event);

bool gui_controller_enqueue_add(const char *url, const char *dest_path,
                                const IpcDownloadOptions *options);
bool gui_controller_enqueue_pause(uint32_t id);
bool gui_controller_enqueue_resume(uint32_t id);
bool gui_controller_enqueue_cancel(uint32_t id);
bool gui_controller_enqueue_details(uint32_t id);

#endif