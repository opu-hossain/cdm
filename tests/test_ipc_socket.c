#include "../src/platform/ipc_socket.h"
#include "../src/utils/log.h"
#include <criterion/criterion.h>
#include <unistd.h>

static void setup_ipc(void) {
  if (ipc_server_is_running()) {
    ipc_server_stop();
  }
  log_init(NULL, LOG_ERROR); // suppress logs
}

static void teardown_ipc(void) {
  ipc_server_stop();
  log_close();
}

TestSuite(ipc, .init = setup_ipc, .fini = teardown_ipc);

Test(ipc, server_start_stop) {
  int rc = ipc_server_start();
  cr_assert_eq(rc, 0);
  cr_assert(ipc_server_is_running());
  ipc_server_stop();
  cr_assert(!ipc_server_is_running());
}

// This test is disabled because it can hang if the server doesn't respond.
// Enable only when running integration tests with a real daemon.
Test(ipc, client_connect_send_receive, .disabled = true) {
  int rc = ipc_server_start();
  cr_assert_eq(rc, 0);

  int client_fd = ipc_client_connect();
  cr_assert_gt(client_fd, 0);

  uint32_t id =
      ipc_send_add_download(client_fd, "http://test", "/tmp/test", NULL);
  cr_assert_neq(id, 0);

  ipc_client_disconnect(client_fd);
  ipc_server_stop();
}
