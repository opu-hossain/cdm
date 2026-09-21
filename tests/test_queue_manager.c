#include "../src/core/queue_manager.h"
#include <criterion/criterion.h>
#include <stdlib.h>
#include <unistd.h>

static void setup(void) {
  // Allow tests to write inside /tmp
  setenv("DOWNLOADMGR_ROOT", "/tmp", 1);
}

static void teardown(void) {
  // No global cleanup needed; each test removes its own downloads.
}

TestSuite(queue_manager, .init = setup, .fini = teardown);

Test(queue_manager, add_and_find) {
  uint32_t id = queue_manager_add("http://example.com/file", "/tmp/file", NULL);
  cr_assert_neq(id, 0);
  Download *d = queue_manager_find_by_id(id);
  cr_assert_not_null(d);
  cr_assert_str_eq(d->url, "http://example.com/file");
  cr_assert_str_eq(d->dest_path, "/tmp/file");
  cr_assert_eq(d->status, DOWNLOAD_QUEUED);
  queue_manager_remove(id);
}

Test(queue_manager, update_status) {
  uint32_t id = queue_manager_add("http://example.com", "/tmp/file", NULL);
  cr_assert_neq(id, 0); // ensure add succeeded
  queue_manager_update_status(id, DOWNLOAD_ACTIVE);
  Download *d = queue_manager_find_by_id(id);
  cr_assert_not_null(d);
  cr_assert_eq(d->status, DOWNLOAD_ACTIVE);
  queue_manager_remove(id);
}

Test(queue_manager, count_by_status) {
  uint32_t id1 = queue_manager_add("http://a", "/tmp/a", NULL);
  uint32_t id2 = queue_manager_add("http://b", "/tmp/b", NULL);
  cr_assert_neq(id1, 0);
  cr_assert_neq(id2, 0);
  queue_manager_update_status(id1, DOWNLOAD_ACTIVE);
  int count = queue_manager_count_by_status(DOWNLOAD_ACTIVE);
  cr_assert_eq(count, 1);
  count = queue_manager_count_by_status(DOWNLOAD_QUEUED);
  cr_assert_eq(count, 1); // id2 is QUEUED
  queue_manager_remove(id1);
  queue_manager_remove(id2);
}

Test(queue_manager, remove) {
  uint32_t id = queue_manager_add("http://example.com", "/tmp/file", NULL);
  cr_assert_neq(id, 0);
  queue_manager_remove(id);
  Download *d = queue_manager_find_by_id(id);
  cr_assert_null(d);
}

Test(queue_manager, remove_active_requests_cancel_without_freeing) {
  uint32_t id = queue_manager_add("http://example.com", "/tmp/file", NULL);
  cr_assert_neq(id, 0);
  queue_manager_update_status(id, DOWNLOAD_ACTIVE);

  queue_manager_remove(id);

  Download *d = queue_manager_find_by_id(id);
  cr_assert_not_null(d);
  cr_assert(atomic_load(&d->cancel_requested));

  queue_manager_update_status(id, DOWNLOAD_ERROR);
  queue_manager_remove(id);
  cr_assert_null(queue_manager_find_by_id(id));
}

Test(queue_manager, status_query_does_not_expose_queue_memory) {
  uint32_t id = queue_manager_add("http://example.com/status", "/tmp/status",
                                  NULL);
  cr_assert_neq(id, 0);

  DownloadStatus status = DOWNLOAD_ERROR;
  cr_assert(queue_manager_get_status(id, &status));
  cr_assert_eq(status, DOWNLOAD_QUEUED);

  queue_manager_remove(id);
  cr_assert_not(queue_manager_get_status(id, &status));
}

Test(queue_manager, runtime_snapshot_is_a_value_copy) {
  uint32_t id = queue_manager_add("http://example.com/snapshot",
                                  "/tmp/snapshot", NULL);
  cr_assert_neq(id, 0);
  Download *download = queue_manager_find_by_id(id);
  cr_assert_not_null(download);
  download->total_size = 100;
  download->chunk_count = 1;
  download->chunks[0] = (DownloadChunk){0, 99, 25};

  DownloadRuntimeSnapshot snapshot = {0};
  cr_assert(queue_manager_get_runtime_snapshot(id, &snapshot));
  cr_assert_eq(snapshot.status, DOWNLOAD_QUEUED);
  cr_assert_eq(snapshot.total_size, 100);
  cr_assert_eq(snapshot.chunk_count, 1);
  cr_assert_eq(snapshot.chunks[0].bytes_done, 25);

  queue_manager_remove(id);
  cr_assert_eq(snapshot.total_size, 100);
  cr_assert_eq(snapshot.chunks[0].bytes_done, 25);
}

Test(queue_manager, request_options_are_optional_and_owned) {
  RequestOptions empty = {0};
  uint32_t empty_id =
      queue_manager_add("http://example.com/empty", "/tmp/empty", &empty);
  cr_assert_neq(empty_id, 0);
  cr_assert_null(queue_manager_find_by_id(empty_id)->request);

  RequestOptions options = {0};
  strcpy(options.cookie, "session=abc");
  uint32_t id =
      queue_manager_add("http://example.com/options", "/tmp/options", &options);
  cr_assert_neq(id, 0);
  options.cookie[0] = '\0';

  Download *download = queue_manager_find_by_id(id);
  cr_assert_not_null(download->request);
  cr_assert_str_eq(download->request->cookie, "session=abc");

  queue_manager_remove(empty_id);
  queue_manager_remove(id);
}

Test(queue_manager, allowed_root_tracks_environment_changes) {
  setenv("DOWNLOADMGR_ROOT", "/var", 1);
  cr_assert_eq(queue_manager_add("http://example.com/rejected", "/tmp/rejected",
                                 NULL),
               0);

  setenv("DOWNLOADMGR_ROOT", "/tmp", 1);
  uint32_t id =
      queue_manager_add("http://example.com/accepted", "/tmp/accepted", NULL);
  cr_assert_neq(id, 0);
  queue_manager_remove(id);
}

Test(queue_manager, duplicate_destinations_are_rejected) {
  const char *dest = "/tmp/duplicate-destination.bin";
  unlink(dest);

  uint32_t id1 = queue_manager_add("http://example.com/first", dest, NULL);
  cr_assert_neq(id1, 0);

  uint32_t id2 = queue_manager_add("http://example.com/second", dest, NULL);
  cr_assert_eq(id2, 0);

  queue_manager_remove(id1);
  unlink(dest);
}

Test(queue_manager, snapshot_active_progress) {
  uint32_t id1 = queue_manager_add("http://a", "/tmp/a", NULL);
  uint32_t id2 = queue_manager_add("http://b", "/tmp/b", NULL);
  cr_assert_neq(id1, 0);
  cr_assert_neq(id2, 0);
  queue_manager_update_status(id1, DOWNLOAD_ACTIVE);
  queue_manager_update_status(id2, DOWNLOAD_ACTIVE);
  atomic_store(&queue_manager_find_by_id(id1)->bytes_downloaded, 100);
  atomic_store(&queue_manager_find_by_id(id2)->bytes_downloaded, 200);

  DownloadProgressSnapshot snaps[8];
  int n = queue_manager_snapshot_active_progress(snaps, 8);
  cr_assert_eq(n, 2);
  int found1 = 0, found2 = 0;
  for (int i = 0; i < n; i++) {
    if (snaps[i].id == id1) {
      cr_assert_eq(snaps[i].bytes_downloaded, 100);
      found1 = 1;
    }
    if (snaps[i].id == id2) {
      cr_assert_eq(snaps[i].bytes_downloaded, 200);
      found2 = 1;
    }
  }
  cr_assert(found1 && found2);
  queue_manager_remove(id1);
  queue_manager_remove(id2);
}
