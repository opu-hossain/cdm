#include "../src/platform/ipc_socket.h"
#include <criterion/criterion.h>

Test(ipc, connect, .disabled = true) { // mark as disabled
  int fd = ipc_client_connect();
  cr_assert_gt(fd, 0);
  ipc_client_disconnect(fd);
}
