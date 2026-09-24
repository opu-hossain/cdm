#include "../src/gui/gui_controller.h"
#include "../src/gui/gui_model.h"
#include <criterion/criterion.h>
#include <string.h>

TestSuite(gui_controller, .init = gui_controller_init,
          .fini = gui_controller_shutdown);

Test(gui_controller, preserves_event_order) {
  GuiControllerEvent first = {.type = GUI_CONTROLLER_EVENT_OPERATION};
  first.data.operation.operation = GUI_CONTROLLER_OPERATION_PAUSE;
  first.data.operation.download_id = 7;
  first.data.operation.succeeded = true;

  GuiControllerEvent second = {.type = GUI_CONTROLLER_EVENT_CONNECTION};
  second.data.connection.connected = false;

  cr_assert(gui_controller_publish(&first));
  cr_assert(gui_controller_publish(&second));

  GuiControllerEvent received = {0};
  cr_assert(gui_controller_poll(&received));
  cr_assert_eq(received.type, GUI_CONTROLLER_EVENT_OPERATION);
  cr_assert_eq(received.data.operation.download_id, 7);
  cr_assert(gui_controller_poll(&received));
  cr_assert_eq(received.type, GUI_CONTROLLER_EVENT_CONNECTION);
  cr_assert_not(received.data.connection.connected);
  cr_assert_not(gui_controller_poll(&received));
}

Test(gui_controller, copies_payloads) {
  GuiControllerEvent event = {.type = GUI_CONTROLLER_EVENT_ERROR};
  strcpy(event.data.error.message, "connection failed");
  cr_assert(gui_controller_publish(&event));
  memset(&event, 0, sizeof(event));

  GuiControllerEvent received = {0};
  cr_assert(gui_controller_poll(&received));
  cr_assert_str_eq(received.data.error.message, "connection failed");
}

Test(gui_controller, v2_progress_survives_snapshot_and_formats_row_metadata) {
  gui_model_init();
  GuiDownloadRecord record = {.id = 17, .progress = 0.0f};
  strcpy(record.status, "ACTIVE");
  gui_model_apply_snapshot(&record, 1);

  GuiControllerEvent update = {.type = GUI_CONTROLLER_EVENT_STATUS};
  update.data.status.download_id = 17;
  strcpy(update.data.status.status, "ACTIVE");
  update.data.status.progress = 0.25f;
  update.data.status.has_v2 = true;
  update.data.status.v2.download_id = 17;
  update.data.status.v2.bytes_received = 1024;
  update.data.status.v2.total_bytes = 4096;
  update.data.status.v2.speed_bps = 512;
  update.data.status.v2.eta_seconds = 6;
  update.data.status.v2.progress = 0.25f;
  cr_assert(gui_controller_publish(&update));

  GuiControllerEvent received = {0};
  cr_assert(gui_controller_poll(&received));
  cr_assert(received.data.status.has_v2);
  gui_model_apply_status_update_v2(received.data.status.download_id,
                                    received.data.status.status,
                                    &received.data.status.v2);
  gui_model_apply_snapshot(&record, 1);
  GuiRow row = {0};
  cr_assert_eq(gui_model_snapshot_rows(&row, 1), 1);
  cr_assert_eq(row.bytes_received, 1024);
  cr_assert_eq(row.total_bytes, 4096);
  cr_assert_eq(row.speed_bps, 512);
  cr_assert_eq(row.eta_seconds, 6);
  cr_assert_float_eq(row.progress, 0.25f, 0.001f);
  cr_assert(row.has_v2);

  char formatted[32];
  gui_format_bytes(1536, formatted, sizeof(formatted));
  cr_assert_str_eq(formatted, "1.5 KB");
  gui_format_eta(58, formatted, sizeof(formatted));
  cr_assert_str_eq(formatted, "00:58");
  gui_format_eta(3661, formatted, sizeof(formatted));
  cr_assert_str_eq(formatted, "01:01:01");
  gui_format_eta(UINT64_MAX, formatted, sizeof(formatted));
  cr_assert_str_eq(formatted, "--:--");

  gui_model_apply_status_update(17, "ACTIVE", 0.5f);
  cr_assert_eq(gui_model_snapshot_rows(&row, 1), 1);
  cr_assert_not(row.has_v2); // an older daemon supplies only v1 events
}
