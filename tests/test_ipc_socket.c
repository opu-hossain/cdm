#include "../src/platform/ipc_socket.h"
#include "../src/core/queue_manager.h"
#include "../src/persistence/db.h"
#include "../src/utils/log.h"
#include <criterion/criterion.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
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

Test(ipc, read_exact_handles_fragmented_input) {
  int sockets[2];
  cr_assert_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

  const char payload[] = "fragmented IPC payload";
  for (size_t i = 0; i < sizeof(payload); i++)
    cr_assert_eq(write(sockets[0], &payload[i], 1), 1);

  char received[sizeof(payload)] = {0};
  cr_assert_eq(ipc_read_exact(sockets[1], received, sizeof(received)), 0);
  cr_assert_str_eq(received, payload);

  close(sockets[0]);
  close(sockets[1]);
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

static atomic_bool browser_server_running;

static int browser_server_thread(void *unused) {
  (void)unused;
  while (atomic_load(&browser_server_running))
    ipc_server_poll();
  return 0;
}

Test(ipc, browser_offer_confirm_is_idempotent_and_dismiss_blocks_queueing) {
  char dir[] = "/tmp/cdm-browser-ipc-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));
  setenv("DOWNLOADMGR_ROOT", dir, 1);
  cr_assert_eq(db_init(":memory:"), 0);
  cr_assert_eq(ipc_server_start(), 0);

  atomic_store(&browser_server_running, true);
  thrd_t server;
  cr_assert_eq(thrd_create(&server, browser_server_thread, NULL), thrd_success);
  int client = ipc_client_connect_timeout(1500);
  cr_assert_geq(client, 0);

  uint16_t version = 0;
  cr_assert_eq(ipc_client_hello(client, &version), 0);
  cr_assert_eq(version, IPC_PROTOCOL_VERSION);
  MsgHeader legacy = {.length = 0, .type = MSG_LIST};
  cr_assert_eq(sizeof(legacy), 8);
  cr_assert_eq(ipc_write_exact(client, &legacy, sizeof(legacy)), 0);
  uint32_t count = UINT32_MAX;
  cr_assert_eq(ipc_read_exact(client, &count, sizeof(count)), 0);
  cr_assert_eq(count, 0);
  uint16_t negotiated = 0;
  int compatible = ipc_client_connect_compatible(-1, &negotiated);
  cr_assert_geq(compatible, 0);
  cr_assert_eq(negotiated, IPC_PROTOCOL_VERSION);
  struct timeval receive_timeout;
  socklen_t timeout_size = sizeof(receive_timeout);
  cr_assert_eq(getsockopt(compatible, SOL_SOCKET, SO_RCVTIMEO,
                          &receive_timeout, &timeout_size), 0);
  cr_assert_eq(receive_timeout.tv_sec, 0);
  cr_assert_eq(receive_timeout.tv_usec, 0);
  ipc_client_disconnect(compatible);

  IpcBrowserOffer request = {0}, first = {0}, repeated = {0};
  strcpy(request.request_id, "ipc-test-offer-1");
  strcpy(request.url, "https://example.org/archive.tar.zst");
  strcpy(request.filename, "archive.tar.zst");
  request.total_bytes = 1234;
  cr_assert_eq(ipc_browser_offer(client, &request, &first), 0);
  cr_assert_neq(first.offer_id, 0);
  cr_assert_eq(first.state, IPC_BROWSER_WAITING);
  cr_assert_eq(queue_manager_count_by_status(DOWNLOAD_QUEUED), 0);
  cr_assert_eq(ipc_browser_offer(client, &request, &repeated), 0);
  cr_assert_eq(repeated.offer_id, first.offer_id);

  char dest[1024];
  snprintf(dest, sizeof(dest), "%s/archive.tar.zst", dir);
  uint32_t download_id = 0, second_id = 0;
  cr_assert_eq(ipc_browser_confirm(client, first.offer_id, dest,
                                   &download_id), 0);
  cr_assert_neq(download_id, 0);
  cr_assert_eq(ipc_browser_confirm(client, first.offer_id, dest,
                                   &second_id), 0);
  cr_assert_eq(second_id, download_id);
  cr_assert_eq(queue_manager_count_by_status(DOWNLOAD_QUEUED), 1);
  cr_assert_neq(ipc_browser_dismiss(client, first.offer_id), 0);

  IpcBrowserProgress initial = {0};
  cr_assert_eq(ipc_browser_subscribe_progress(client, download_id, &initial),
               0);
  cr_assert_eq(initial.download_id, download_id);
  cr_assert_str_eq(initial.status, "QUEUED");
  cr_assert_str_eq(initial.dest_path, dest);
  ipc_broadcast_status(download_id, "Downloading", 0.0f);
  MsgHeader event_header = {0};
  IpcBrowserProgress event = {0};
  cr_assert_eq(ipc_read_exact(client, &event_header, sizeof(event_header)), 0);
  cr_assert_eq(event_header.type, MSG_BROWSER_PROGRESS_EVENT);
  cr_assert_eq(event_header.length, sizeof(event));
  cr_assert_eq(ipc_read_exact(client, &event, sizeof(event)), 0);
  cr_assert_eq(event.download_id, download_id);

  int v2_client = ipc_client_connect_compatible(1500, NULL);
  cr_assert_geq(v2_client, 0);
  cr_assert_eq(ipc_send_subscribe_v2(v2_client), 0);
  MsgHeader barrier = {.length = 0, .type = MSG_LIST};
  uint32_t ignored_count = 0;
  cr_assert_eq(ipc_write_exact(v2_client, &barrier, sizeof(barrier)), 0);
  cr_assert_eq(ipc_read_exact(v2_client, &ignored_count,
                              sizeof(ignored_count)), 0);
  int legacy_client = ipc_client_connect_timeout(1500);
  cr_assert_geq(legacy_client, 0);
  ipc_send_subscribe(legacy_client);
  cr_assert_eq(ipc_write_exact(legacy_client, &barrier, sizeof(barrier)), 0);
  cr_assert_eq(ipc_read_exact(legacy_client, &ignored_count,
                              sizeof(ignored_count)), 0);
  ipc_broadcast_status(download_id, "Downloading", 0.25f);
  MsgHeader v2_header = {0}, fallback_header = {0};
  IpcProgressV2 rich = {0};
  cr_assert_eq(ipc_read_exact(v2_client, &v2_header, sizeof(v2_header)), 0);
  cr_assert_eq(v2_header.type, MSG_STATUS_EVENT_V2);
  cr_assert_eq(v2_header.length, sizeof(rich));
  cr_assert_eq(ipc_read_exact(v2_client, &rich, sizeof(rich)), 0);
  cr_assert_eq(rich.download_id, download_id);
  cr_assert_eq(rich.total_bytes, 0);
  cr_assert_eq(rich.speed_bps, 0);
  cr_assert_eq(rich.eta_seconds, UINT64_MAX);
  cr_assert_float_eq(rich.progress, -1.0f, 0.001f);
  cr_assert_str_eq(rich.status, "Downloading");
  cr_assert_eq(ipc_read_exact(v2_client, &fallback_header,
                              sizeof(fallback_header)), 0);
  cr_assert_eq(fallback_header.type, MSG_STATUS_EVENT);
  char fallback[64];
  cr_assert_leq(fallback_header.length, sizeof(fallback));
  cr_assert_eq(ipc_read_exact(v2_client, fallback, fallback_header.length), 0);
  ipc_client_disconnect(v2_client);
  MsgHeader legacy_event = {0};
  cr_assert_eq(ipc_read_exact(legacy_client, &legacy_event,
                              sizeof(legacy_event)), 0);
  cr_assert_eq(legacy_event.type, MSG_STATUS_EVENT);
  cr_assert_leq(legacy_event.length, sizeof(fallback));
  cr_assert_eq(ipc_read_exact(legacy_client, fallback, legacy_event.length), 0);
  ipc_client_disconnect(legacy_client);

  strcpy(request.request_id, "ipc-test-offer-2");
  IpcBrowserOffer dismissed = {0};
  cr_assert_eq(ipc_browser_offer(client, &request, &dismissed), 0);
  cr_assert_eq(ipc_browser_dismiss(client, dismissed.offer_id), 0);
  cr_assert_neq(ipc_browser_confirm(client, dismissed.offer_id, dest,
                                    &second_id), 0);
  cr_assert_eq(queue_manager_count_by_status(DOWNLOAD_QUEUED), 1);

  IpcBrowserOffer invalid = request, ignored = {0};
  strcpy(invalid.request_id, "ipc-test-invalid");
  strcpy(invalid.url, "file:///etc/passwd");
  cr_assert_neq(ipc_browser_offer(client, &invalid, &ignored), 0);
  cr_assert_eq(queue_manager_count_by_status(DOWNLOAD_QUEUED), 1);

  int oversized = ipc_client_connect_timeout(1500);
  cr_assert_geq(oversized, 0);
  MsgHeader bad_header = {.length = IPC_MAX_FRAME_SIZE + 1,
                          .type = MSG_BROWSER_OFFER};
  cr_assert_eq(ipc_write_exact(oversized, &bad_header, sizeof(bad_header)), 0);
  char rejected = 0;
  cr_assert_eq(read(oversized, &rejected, 1), 0);
  ipc_client_disconnect(oversized);

  ipc_client_disconnect(client);
  atomic_store(&browser_server_running, false);
  thrd_join(server, NULL);
  ipc_server_stop();
  queue_manager_remove(download_id);
  db_close();
  rmdir(dir);
  unsetenv("DOWNLOADMGR_ROOT");
}
