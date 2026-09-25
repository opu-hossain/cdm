#include "../src/core/queue_manager.h"
#include "../src/persistence/db.h"
#include "../src/platform/thread.h"
#include <sqlite3.h>
#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>
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

Test(queue_manager, named_queue_crud_and_priority_selection) {
  cr_assert_eq(db_init(":memory:"), 0);
  Queue fast = {.priority = 8, .max_concurrent = 1};
  strcpy(fast.name, "Fast");
  uint32_t fast_id = 0;
  cr_assert_eq(queue_create(&fast, &fast_id), 0);
  cr_assert_neq(fast_id, 0);
  Queue fetched = {0};
  cr_assert_eq(queue_get(fast_id, &fetched), 0);
  cr_assert_str_eq(fetched.name, "Fast");
  cr_assert_eq(fetched.priority, 8);
  cr_assert_eq(fetched.max_concurrent, 1);
  uint32_t duplicate_id = 0;
  cr_assert_eq(queue_create(&fast, &duplicate_id), -1);
  Queue invalid_schedule = {.priority = 1};
  strcpy(invalid_schedule.name, "Invalid schedule");
  strcpy(invalid_schedule.schedule_start, "09:00");
  strcpy(invalid_schedule.schedule_stop, "09:00");
  cr_assert_eq(queue_create(&invalid_schedule, &duplicate_id), -1);
  strcpy(invalid_schedule.schedule_stop, "25:00");
  cr_assert_eq(queue_create(&invalid_schedule, &duplicate_id), -1);
  strcpy(invalid_schedule.schedule_stop, "17:00");
  cr_assert_eq(queue_create(&invalid_schedule, &duplicate_id), 0);
  cr_assert_eq(queue_delete(duplicate_id), 0);
  cr_assert_eq(queue_get(UINT32_MAX, &fetched), -1);
  Queue *all = NULL;
  size_t count = 0;
  cr_assert_eq(queue_list(&all, &count), 0);
  cr_assert_eq(count, 2);
  cr_assert_eq(all[0].id, fast_id);
  cr_assert_eq(all[1].id, 1);
  free(all);
  cr_assert_eq(queue_reorder(fast_id, 9), 0);
  cr_assert_eq(queue_get(fast_id, &fetched), 0);
  cr_assert_eq(fetched.priority, 9);
  fetched.max_concurrent = 2;
  cr_assert_eq(queue_update(&fetched), 0);
  cr_assert_eq(queue_get(fast_id, &fetched), 0);
  cr_assert_eq(fetched.max_concurrent, 2);

  uint32_t old = queue_manager_add("http://127.0.0.1/old", "/tmp/cdm-q-old", NULL);
  uint32_t newer = queue_manager_add("http://127.0.0.1/new", "/tmp/cdm-q-new", NULL);
  uint32_t normal = queue_manager_add("http://127.0.0.1/normal", "/tmp/cdm-q-normal", NULL);
  cr_assert_neq(old, 0);
  cr_assert_neq(newer, 0);
  cr_assert_neq(normal, 0);
  queue_manager_find_by_id(old)->queue_id = fast_id;
  queue_manager_find_by_id(old)->created_at = 10;
  queue_manager_find_by_id(newer)->queue_id = fast_id;
  queue_manager_find_by_id(newer)->created_at = 20;
  queue_manager_find_by_id(normal)->created_at = 5;
  dm_mutex_t *mutex = (dm_mutex_t *)queue_manager_get_mutex();
  dm_mutex_lock(mutex);
  cr_assert_eq(queue_manager_find_next_queued()->id, old);
  dm_mutex_unlock(mutex);
  queue_manager_update_status(old, DOWNLOAD_ACTIVE);
  dm_mutex_lock(mutex);
  cr_assert_eq(queue_manager_find_next_queued()->id, newer);
  dm_mutex_unlock(mutex);
  fetched.max_concurrent = 1;
  cr_assert_eq(queue_update(&fetched), 0);
  dm_mutex_lock(mutex);
  cr_assert_eq(queue_manager_find_next_queued()->id, normal);
  dm_mutex_unlock(mutex);
  fetched.max_concurrent = 2;
  cr_assert_eq(queue_update(&fetched), 0);
  queue_manager_update_status(newer, DOWNLOAD_ACTIVE);
  dm_mutex_lock(mutex);
  cr_assert_eq(queue_manager_find_next_queued()->id, normal);
  dm_mutex_unlock(mutex);
  queue_manager_update_status(old, DOWNLOAD_DONE);
  queue_manager_update_status(newer, DOWNLOAD_DONE);
  queue_manager_remove(old);
  queue_manager_remove(newer);
  queue_manager_remove(normal);
  cr_assert_eq(queue_delete(1), -1);
  cr_assert_eq(queue_delete(fast_id), 0);
  cr_assert_eq(queue_get(fast_id, &fetched), -1);
  db_close();
}

Test(queue_manager, restore_keeps_queue_assignment_and_created_at) {
  char path[] = "/tmp/cdm-queue-restore-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  close(fd);
  cr_assert_eq(db_init(path), 0);
  Queue extra = {.priority = 4};
  strcpy(extra.name, "Later");
  uint32_t queue_id = 0;
  cr_assert_eq(queue_create(&extra, &queue_id), 0);
  cr_assert_eq(db_insert_download(910, "http://127.0.0.1/restore",
                                  "/tmp/cdm-queue-restore-file", NULL), 0);
  sqlite3 *writer = NULL;
  cr_assert_eq(sqlite3_open(path, &writer), SQLITE_OK);
  sqlite3_stmt *statement = NULL;
  cr_assert_eq(sqlite3_prepare_v2(writer,
      "UPDATE downloads SET queue_id=?, created_at=42 WHERE id=910", -1,
      &statement, NULL), SQLITE_OK);
  sqlite3_bind_int64(statement, 1, (sqlite3_int64)queue_id);
  cr_assert_eq(sqlite3_step(statement), SQLITE_DONE);
  sqlite3_finalize(statement);
  sqlite3_close(writer);
  cr_assert_eq(db_restore_queue(), 0);
  Download *restored = queue_manager_find_by_id(910);
  cr_assert_not_null(restored);
  cr_assert_eq(restored->queue_id, queue_id);
  cr_assert_eq(restored->created_at, 42);
  queue_manager_remove(910);
  db_close();
  unlink(path);
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

Test(queue_manager, destination_history_does_not_claim_filesystem_path) {
  const char *dest = "/tmp/duplicate-destination.bin";
  unlink(dest);

  uint32_t id1 = queue_manager_add("http://example.com/first", dest, NULL);
  cr_assert_neq(id1, 0);

  uint32_t id2 = queue_manager_add("http://example.com/second", dest, NULL);
  cr_assert_neq(id2, 0);

  queue_manager_remove(id1);
  queue_manager_remove(id2);
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
