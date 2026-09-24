#include "gui_controller.h"

#include "../platform/thread.h"
#include "../utils/log.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define GUI_CONTROLLER_EVENT_CAP 256
#define GUI_CONTROLLER_COMMAND_CAP 64
#define GUI_CONTROLLER_REFRESH_MS 2000

typedef enum {
  GUI_CONTROLLER_COMMAND_ADD,
  GUI_CONTROLLER_COMMAND_PAUSE,
  GUI_CONTROLLER_COMMAND_RESUME,
  GUI_CONTROLLER_COMMAND_CANCEL,
  GUI_CONTROLLER_COMMAND_DETAILS,
} GuiControllerCommandType;

typedef struct {
  GuiControllerCommandType type;
  uint32_t id;
  bool auto_filename;
  char url[IPC_MAX_URL_LEN];
  char dest_path[IPC_MAX_PATH_LEN];
  IpcDownloadOptions options;
  char cookie[1024];
  char referrer[2048];
  char extra_headers[4096];
  char expected_sha256[65];
} GuiControllerCommand;

static GuiControllerEvent g_events[GUI_CONTROLLER_EVENT_CAP];
static int g_head;
static int g_tail;
static int g_count;
static dm_mutex_t g_mutex;
static once_flag g_mutex_once = ONCE_FLAG_INIT;
static GuiControllerCommand g_commands[GUI_CONTROLLER_COMMAND_CAP];
static int g_command_head;
static int g_command_tail;
static int g_command_count;
static dm_mutex_t g_command_mutex;
static once_flag g_command_mutex_once = ONCE_FLAG_INIT;
static dm_thread_t g_thread;
static bool g_thread_started;
static _Atomic bool g_running;

static void initialize_mutex(void) { dm_mutex_init(&g_mutex); }

static void ensure_mutex(void) { call_once(&g_mutex_once, initialize_mutex); }

static void initialize_command_mutex(void) { dm_mutex_init(&g_command_mutex); }

static void ensure_command_mutex(void) {
  call_once(&g_command_mutex_once, initialize_command_mutex);
}

void gui_controller_init(void) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  g_head = 0;
  g_tail = 0;
  g_count = 0;
  dm_mutex_unlock(&g_mutex);

  ensure_command_mutex();
  dm_mutex_lock(&g_command_mutex);
  g_command_head = 0;
  g_command_tail = 0;
  g_command_count = 0;
  dm_mutex_unlock(&g_command_mutex);
}

void gui_controller_shutdown(void) { gui_controller_stop(); }

bool gui_controller_publish(const GuiControllerEvent *event) {
  if (!event)
    return false;

  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  if (g_count == GUI_CONTROLLER_EVENT_CAP) {
    dm_mutex_unlock(&g_mutex);
    LOG_WARN("gui_controller: event queue full");
    return false;
  }

  g_events[g_tail] = *event;
  g_tail = (g_tail + 1) % GUI_CONTROLLER_EVENT_CAP;
  g_count++;
  dm_mutex_unlock(&g_mutex);
  return true;
}

bool gui_controller_poll(GuiControllerEvent *event) {
  if (!event)
    return false;

  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  if (g_count == 0) {
    dm_mutex_unlock(&g_mutex);
    return false;
  }

  *event = g_events[g_head];
  g_head = (g_head + 1) % GUI_CONTROLLER_EVENT_CAP;
  g_count--;
  dm_mutex_unlock(&g_mutex);
  return true;
}

static bool enqueue_command(const GuiControllerCommand *command) {
  ensure_command_mutex();
  dm_mutex_lock(&g_command_mutex);
  if (g_command_count == GUI_CONTROLLER_COMMAND_CAP) {
    dm_mutex_unlock(&g_command_mutex);
    LOG_WARN("gui_controller: command queue full");
    return false;
  }
  GuiControllerCommand *queued = &g_commands[g_command_tail];
  *queued = *command;
  if (queued->options.cookie)
    queued->options.cookie = queued->cookie;
  if (queued->options.referrer)
    queued->options.referrer = queued->referrer;
  if (queued->options.extra_headers)
    queued->options.extra_headers = queued->extra_headers;
  if (queued->options.expected_sha256)
    queued->options.expected_sha256 = queued->expected_sha256;
  g_command_tail = (g_command_tail + 1) % GUI_CONTROLLER_COMMAND_CAP;
  g_command_count++;
  dm_mutex_unlock(&g_command_mutex);
  return true;
}

static bool dequeue_command(GuiControllerCommand *command) {
  ensure_command_mutex();
  dm_mutex_lock(&g_command_mutex);
  if (g_command_count == 0) {
    dm_mutex_unlock(&g_command_mutex);
    return false;
  }
  *command = g_commands[g_command_head];
  g_command_head = (g_command_head + 1) % GUI_CONTROLLER_COMMAND_CAP;
  g_command_count--;
  dm_mutex_unlock(&g_command_mutex);
  return true;
}

static bool enqueue_add(const char *url, const char *dest_path,
                        const IpcDownloadOptions *options,
                        bool auto_filename) {
  if (!url || !dest_path)
    return false;
  GuiControllerCommand command = {.type = GUI_CONTROLLER_COMMAND_ADD};
  command.auto_filename = auto_filename;
  strncpy(command.url, url, sizeof(command.url) - 1);
  strncpy(command.dest_path, dest_path, sizeof(command.dest_path) - 1);
  if (options) {
    command.options = *options;
    if (options->cookie) {
      strncpy(command.cookie, options->cookie, sizeof(command.cookie) - 1);
      command.options.cookie = command.cookie;
    }
    if (options->referrer) {
      strncpy(command.referrer, options->referrer,
              sizeof(command.referrer) - 1);
      command.options.referrer = command.referrer;
    }
    if (options->extra_headers) {
      strncpy(command.extra_headers, options->extra_headers,
              sizeof(command.extra_headers) - 1);
      command.options.extra_headers = command.extra_headers;
    }
    if (options->expected_sha256) {
      strncpy(command.expected_sha256, options->expected_sha256,
              sizeof(command.expected_sha256) - 1);
      command.options.expected_sha256 = command.expected_sha256;
    }
  }
  return enqueue_command(&command);
}

bool gui_controller_enqueue_add(const char *url, const char *dest_path,
                                const IpcDownloadOptions *options) {
  return enqueue_add(url, dest_path, options, false);
}

bool gui_controller_enqueue_add_auto(const char *url, const char *dest_path,
                                     const IpcDownloadOptions *options) {
  return enqueue_add(url, dest_path, options, true);
}

static bool enqueue_id_command(GuiControllerCommandType type, uint32_t id) {
  GuiControllerCommand command = {.type = type, .id = id};
  return enqueue_command(&command);
}

bool gui_controller_enqueue_pause(uint32_t id) {
  return enqueue_id_command(GUI_CONTROLLER_COMMAND_PAUSE, id);
}

bool gui_controller_enqueue_resume(uint32_t id) {
  return enqueue_id_command(GUI_CONTROLLER_COMMAND_RESUME, id);
}

bool gui_controller_enqueue_cancel(uint32_t id) {
  return enqueue_id_command(GUI_CONTROLLER_COMMAND_CANCEL, id);
}

bool gui_controller_enqueue_details(uint32_t id) {
  return enqueue_id_command(GUI_CONTROLLER_COMMAND_DETAILS, id);
}

static void publish_operation(GuiControllerOperation operation, uint32_t id,
                              const char *url, const char *dest_path,
                              bool succeeded) {
  GuiControllerEvent event = {.type = GUI_CONTROLLER_EVENT_OPERATION};
  event.data.operation.operation = operation;
  event.data.operation.download_id = id;
  if (url)
    snprintf(event.data.operation.url, sizeof(event.data.operation.url), "%s",
             url);
  if (dest_path)
    snprintf(event.data.operation.dest_path,
             sizeof(event.data.operation.dest_path), "%s", dest_path);
  event.data.operation.succeeded = succeeded;
  gui_controller_publish(&event);
}

static void publish_snapshot(void) {
  GuiDownloadRecord *records = NULL;
  int count = 0;
  if (!gui_client_list_all(&records, &count))
    return;

  GuiControllerEvent event = {.type = GUI_CONTROLLER_EVENT_SNAPSHOT};
  event.data.snapshot.count =
      count > GUI_CONTROLLER_MAX_ROWS ? GUI_CONTROLLER_MAX_ROWS : count;
  if (event.data.snapshot.count > 0)
    memcpy(event.data.snapshot.records, records,
           (size_t)event.data.snapshot.count * sizeof(records[0]));
  gui_controller_publish(&event);
  free(records);
}

static void publish_error(const char *message) {
  GuiControllerEvent event = {.type = GUI_CONTROLLER_EVENT_ERROR};
  snprintf(event.data.error.message, sizeof(event.data.error.message), "%s",
           message ? message : "GUI operation failed");
  gui_controller_publish(&event);
}

static void publish_client_events(void) {
  GuiClientEvent client_event;
  while (gui_client_poll_event(&client_event)) {
    if (client_event.type == GUI_EVT_STATUS_UPDATE) {
      GuiControllerEvent event = {.type = GUI_CONTROLLER_EVENT_STATUS};
      event.data.status.download_id = client_event.download_id;
      event.data.status.progress = client_event.progress;
      snprintf(event.data.status.status, sizeof(event.data.status.status), "%s",
               client_event.status);
      gui_controller_publish(&event);
    } else {
      GuiControllerEvent event = {.type = GUI_CONTROLLER_EVENT_CONNECTION};
      event.data.connection.connected =
          client_event.type == GUI_EVT_CONNECTION_RESTORED;
      gui_controller_publish(&event);
    }
  }
}

static void process_command(const GuiControllerCommand *command) {
  bool succeeded = false;
  GuiControllerOperation operation = GUI_CONTROLLER_OPERATION_ADD;
  switch (command->type) {
  case GUI_CONTROLLER_COMMAND_ADD: {
    uint32_t id = 0;
    succeeded = (command->auto_filename ? gui_client_add_download_auto
                                        : gui_client_add_download)(
        command->url, command->dest_path,
        command->options.cookie || command->options.referrer ||
                command->options.extra_headers ||
                command->options.expected_sha256 ||
                command->options.speed_limit_bps > 0
            ? &command->options
            : NULL,
        &id);
    publish_operation(GUI_CONTROLLER_OPERATION_ADD, id, command->url,
                      command->dest_path, succeeded);
    if (!succeeded)
      publish_error("Download could not be added");
    return;
  }
  case GUI_CONTROLLER_COMMAND_PAUSE:
    operation = GUI_CONTROLLER_OPERATION_PAUSE;
    succeeded = gui_client_pause(command->id);
    break;
  case GUI_CONTROLLER_COMMAND_RESUME:
    operation = GUI_CONTROLLER_OPERATION_RESUME;
    succeeded = gui_client_resume(command->id);
    break;
  case GUI_CONTROLLER_COMMAND_CANCEL:
    operation = GUI_CONTROLLER_OPERATION_CANCEL;
    succeeded = gui_client_cancel(command->id);
    break;
  case GUI_CONTROLLER_COMMAND_DETAILS: {
    GuiControllerEvent event = {.type = GUI_CONTROLLER_EVENT_DETAILS};
    event.data.details.download_id = command->id;
    event.data.details.found =
        gui_client_get_details(command->id, &event.data.details.details);
    gui_controller_publish(&event);
    if (!event.data.details.found)
      publish_error("Download details are unavailable");
    return;
  }
  }
  publish_operation(operation, command->id, NULL, NULL, succeeded);
  if (!succeeded)
    publish_error("Download operation failed");
}

static int controller_thread_fn(void *arg) {
  (void)arg;
  int refresh_elapsed = GUI_CONTROLLER_REFRESH_MS;
  while (atomic_load(&g_running)) {
    publish_client_events();
    if (refresh_elapsed >= GUI_CONTROLLER_REFRESH_MS) {
      refresh_elapsed = 0;
      publish_snapshot();
    }

    GuiControllerCommand command;
    while (dequeue_command(&command))
      process_command(&command);

    dm_thread_sleep_ms(50);
    refresh_elapsed += 50;
  }
  return 0;
}

bool gui_controller_start(void) {
  gui_controller_init();
  atomic_store(&g_running, true);
  if (dm_thread_create(&g_thread, controller_thread_fn, NULL) != 0) {
    atomic_store(&g_running, false);
    return false;
  }
  g_thread_started = true;
  return true;
}

void gui_controller_stop(void) {
  atomic_store(&g_running, false);
  if (g_thread_started) {
    dm_thread_join(&g_thread, NULL);
    g_thread_started = false;
  }
}
