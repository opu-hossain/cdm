#include "../src/platform/ipc_socket.h"
#include "../src/core/queue_manager.h"
#include "../src/persistence/db.h"
#include "../src/platform/thread.h"
#include "../src/utils/log.h"
#include <criterion/criterion.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>

static char ipc_runtime_dir[64];

static void setup_ipc(void) {
  snprintf(ipc_runtime_dir, sizeof(ipc_runtime_dir),
           "/tmp/cdm-ipc-test-XXXXXX");
  cr_assert_not_null(mkdtemp(ipc_runtime_dir));
  cr_assert_eq(setenv("XDG_RUNTIME_DIR", ipc_runtime_dir, 1), 0);
  if (ipc_server_is_running()) {
    ipc_server_stop();
  }
  log_init(NULL, LOG_ERROR); // suppress logs
}

static void teardown_ipc(void) {
  ipc_server_stop();
  log_close();
  unsetenv("XDG_RUNTIME_DIR");
  rmdir(ipc_runtime_dir);
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

Test(ipc, list_page_covers_history_beyond_legacy_limit) {
  char dir[] = "/tmp/cdm-page-ipc-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));
  setenv("DOWNLOADMGR_ROOT", dir, 1);
  cr_assert_eq(db_init(":memory:"), 0);
  for (uint32_t id = 1; id <= 600; id++) {
    char path[128];
    snprintf(path, sizeof(path), "%s/item-%u", dir, id);
    cr_assert_eq(db_insert_download(id, "http://127.0.0.1/item", path, NULL),
                 0);
  }
  cr_assert_eq(ipc_server_start(), 0);
  atomic_store(&browser_server_running, true);
  thrd_t server;
  cr_assert_eq(thrd_create(&server, browser_server_thread, NULL), thrd_success);
  int client = ipc_client_connect_compatible(-1, NULL);
  cr_assert_geq(client, 0);

  IpcDownloadRecord *rows = calloc(500, sizeof(*rows));
  cr_assert_not_null(rows);
  uint32_t total = 0;
  int returned = ipc_send_list_page(client, 0, 500, rows, 500, &total);
  cr_assert_eq(total, 600);
  cr_assert_eq(returned, 500);
  for (int i = 0; i < returned; i++)
    cr_assert_eq(rows[i].id, (uint32_t)(600 - i));

  returned = ipc_send_list_page(client, 500, 500, rows, 500, &total);
  cr_assert_eq(total, 600);
  cr_assert_eq(returned, 100);
  for (int i = 0; i < returned; i++)
    cr_assert_eq(rows[i].id, (uint32_t)(100 - i));

  returned = ipc_send_list_page(client, 600, 500, rows, 500, &total);
  cr_assert_eq(total, 600);
  cr_assert_eq(returned, 0);
  returned = ipc_send_list_page(client, 0, 1000, rows, 500, &total);
  cr_assert_eq(total, 600);
  cr_assert_eq(returned, 500);
  cr_assert_eq(ipc_send_list_all(client, rows, 500), 200);

  free(rows);
  ipc_client_disconnect(client);
  atomic_store(&browser_server_running, false);
  thrd_join(server, NULL);
  ipc_server_stop();
  db_close();
  rmdir(dir);
  unsetenv("DOWNLOADMGR_ROOT");
}

Test(ipc, add_credentials_details_never_return_password) {
  char dir[] = "/tmp/cdm-auth-ipc-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));
  setenv("DOWNLOADMGR_ROOT", dir, 1);
  cr_assert_eq(db_init(":memory:"), 0);
  cr_assert_eq(ipc_server_start(), 0);
  atomic_store(&browser_server_running, true);
  thrd_t server;
  cr_assert_eq(thrd_create(&server, browser_server_thread, NULL), thrd_success);
  int client = ipc_client_connect_compatible(-1, NULL);
  cr_assert_geq(client, 0);
  char dest[128];
  snprintf(dest, sizeof(dest), "%s/protected.bin", dir);
  IpcDownloadOptions options = {.auth_user = "example-user",
                                .auth_password = "example-secret"};
  uint32_t id = ipc_send_add_download(client, "http://127.0.0.1/protected",
                                      dest, &options);
  cr_assert_neq(id, 0);
  IpcDownloadDetails details = {0};
  cr_assert_eq(ipc_send_get_details_v2(client, id, &details), 0);
  cr_assert_str_eq(details.auth_user, "example-user");
  cr_assert(details.has_password);
  cr_assert_eq(ipc_send_get_details(client, id, &details), 0);
  cr_assert_str_empty(details.auth_user);
  cr_assert_not(details.has_password);
  cr_assert_null(memmem(&details, sizeof(details), "example-secret",
                       strlen("example-secret")));
  RequestOptions stored = {0};
  Download *download = queue_manager_find_by_id(id);
  cr_assert_not_null(download);
  cr_assert_not_null(download->request);
  stored = *download->request;
  cr_assert_str_eq(stored.auth_password, "example-secret");
  ipc_client_disconnect(client);
  atomic_store(&browser_server_running, false);
  thrd_join(server, NULL);
  ipc_server_stop();
  queue_manager_remove(id);
  db_close();
  unlink(dest);
  rmdir(dir);
  unsetenv("DOWNLOADMGR_ROOT");
}

Test(ipc, duplicate_v2_add_returns_existing_id_without_second_row) {
  char dir[] = "/tmp/cdm-duplicate-ipc-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));
  setenv("DOWNLOADMGR_ROOT", dir, 1);
  cr_assert_eq(db_init(":memory:"), 0);
  cr_assert_eq(ipc_server_start(), 0);
  atomic_store(&browser_server_running, true);
  thrd_t server;
  cr_assert_eq(thrd_create(&server, browser_server_thread, NULL), thrd_success);
  int client = ipc_client_connect_compatible(-1, NULL);
  cr_assert_geq(client, 0);
  char first[128], second[128];
  snprintf(first, sizeof(first), "%s/first.bin", dir);
  snprintf(second, sizeof(second), "%s/second.bin", dir);
  IpcAddResponse result = {0};
  cr_assert_eq(ipc_send_add_download_v2(client,
      "HTTP://127.0.0.1:80/File?Q=One#part", first, NULL, true, &result), 0);
  cr_assert_eq(result.result, IPC_RESULT_OK);
  cr_assert_neq(result.id, 0);
  uint32_t existing = result.id;
  cr_assert_eq(ipc_send_add_download_v2(client,
      "http://127.0.0.1/File?Q=One", second, NULL, true, &result), 0);
  cr_assert_eq(result.result, IPC_RESULT_REJECTED);
  cr_assert_eq(result.id, existing);
  cr_assert_eq(db_count_downloads_total(), 1);
  cr_assert_eq(access(second, F_OK), -1);
  ipc_client_disconnect(client);
  atomic_store(&browser_server_running, false);
  thrd_join(server, NULL);
  ipc_server_stop();
  queue_manager_remove(existing);
  db_close();
  unlink(first);
  rmdir(dir);
  unsetenv("DOWNLOADMGR_ROOT");
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
  IpcBrowserOffer duplicate_request = {0}, duplicate_offer = {0};
  strcpy(duplicate_request.request_id, "ipc-test-offer-duplicate");
  strcpy(duplicate_request.url, "https://EXAMPLE.org:443/archive.tar.zst#x");
  strcpy(duplicate_request.filename, "duplicate.tar.zst");
  cr_assert_eq(ipc_browser_offer(client, &duplicate_request,
                                 &duplicate_offer), 0);
  char duplicate_dest[1024];
  snprintf(duplicate_dest, sizeof(duplicate_dest), "%s/duplicate.tar.zst",
           dir);
  IpcAddResponse duplicate_result = {0};
  cr_assert_eq(ipc_browser_confirm_v2(client, duplicate_offer.offer_id,
                                      duplicate_dest, &duplicate_result), 0);
  cr_assert_eq(duplicate_result.result, IPC_RESULT_REJECTED);
  cr_assert_eq(duplicate_result.id, download_id);
  cr_assert_eq(ipc_browser_confirm_v2(client, duplicate_offer.offer_id,
                                      duplicate_dest, &duplicate_result), 0);
  cr_assert_eq(duplicate_result.result, IPC_RESULT_REJECTED);
  cr_assert_eq(db_count_downloads_total(), 1);
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
  queue_manager_update_status(download_id, DOWNLOAD_ACTIVE);
  Download *active = queue_manager_find_by_id(download_id);
  cr_assert_not_null(active);
  dm_mutex_t *queue_mutex = (dm_mutex_t *)queue_manager_get_mutex();
  dm_mutex_lock(queue_mutex);
  active->total_size = 10000;
  atomic_store(&active->bytes_downloaded, 3000);
  dm_mutex_unlock(queue_mutex);
  DownloadTransferMetrics sample = {.speed_bps = 1300, .eta_seconds = 6};
  queue_manager_set_transfer_metrics(download_id, sample);
  ipc_broadcast_status(download_id, "Downloading", 0.3f);
  MsgHeader v2_header = {0}, fallback_header = {0};
  IpcProgressV2 rich = {0};
  cr_assert_eq(ipc_read_exact(v2_client, &v2_header, sizeof(v2_header)), 0);
  cr_assert_eq(v2_header.type, MSG_STATUS_EVENT_V2);
  cr_assert_eq(v2_header.length, sizeof(rich));
  cr_assert_eq(ipc_read_exact(v2_client, &rich, sizeof(rich)), 0);
  cr_assert_eq(rich.download_id, download_id);
  cr_assert_eq(rich.bytes_received, 3000);
  cr_assert_eq(rich.total_bytes, 10000);
  cr_assert_eq(rich.speed_bps, 1300);
  cr_assert_eq(rich.eta_seconds, 6);
  cr_assert_float_eq(rich.progress, 0.3f, 0.001f);
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
  queue_manager_update_status(download_id, DOWNLOAD_QUEUED);

  int popup_client = ipc_client_connect_compatible(1500, NULL);
  cr_assert_geq(popup_client, 0);
  IpcBrowserProgress popup_initial = {0};
  cr_assert_eq(ipc_browser_subscribe_progress(popup_client, download_id,
                                              &popup_initial), 0);
  cr_assert_eq(ipc_send_subscribe_v2(popup_client), 0);
  cr_assert_eq(ipc_write_exact(popup_client, &barrier, sizeof(barrier)), 0);
  cr_assert_eq(ipc_read_exact(popup_client, &ignored_count,
                              sizeof(ignored_count)), 0);
  ipc_broadcast_status(download_id, "Paused", 0.3f);
  MsgHeader popup_v2_header = {0};
  cr_assert_eq(ipc_read_exact(popup_client, &popup_v2_header,
                              sizeof(popup_v2_header)), 0);
  cr_assert_eq(popup_v2_header.type, MSG_STATUS_EVENT_V2);
  cr_assert_eq(popup_v2_header.length, sizeof(IpcProgressV2));
  cr_assert_eq(ipc_read_exact(popup_client, &rich, sizeof(rich)), 0);
  cr_assert_eq(rich.speed_bps, 1300);
  MsgHeader popup_browser_header = {0};
  cr_assert_eq(ipc_read_exact(popup_client, &popup_browser_header,
                              sizeof(popup_browser_header)), 0);
  cr_assert_eq(popup_browser_header.type, MSG_BROWSER_PROGRESS_EVENT);
  cr_assert_eq(ipc_read_exact(popup_client, &event, sizeof(event)), 0);
  ipc_client_disconnect(popup_client);
  for (int pending = 0; pending < 2; pending++) {
    MsgHeader pending_header = {0};
    cr_assert_eq(ipc_read_exact(client, &pending_header,
                                sizeof(pending_header)), 0);
    cr_assert_eq(pending_header.type, MSG_BROWSER_PROGRESS_EVENT);
    cr_assert_eq(ipc_read_exact(client, &event, sizeof(event)), 0);
  }

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
