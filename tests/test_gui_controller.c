#include "../src/gui/gui_controller.h"
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