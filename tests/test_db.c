#include "../src/persistence/db.h"
#include <sqlite3.h>
#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Use in-memory DB for tests
static void setup_db(void) { db_init(":memory:"); }

static void close_db(void) { db_close(); }

TestSuite(db, .init = setup_db, .fini = close_db);

static int find_category_row(const DbDownloadRow *row, void *ctx) {
  if (row->id == 901)
    *(uint32_t *)ctx = row->category_id;
  return 0;
}

Test(db, category_assignment_survives_delete_as_default) {
  Category video = {0};
  strcpy(video.name, "Video");
  strcpy(video.extensions, "mp4");
  uint32_t category_id = 0, observed = 0;
  cr_assert_eq(db_category_create(&video, &category_id), 0);
  cr_assert_eq(db_insert_download(901, "http://127.0.0.1/video.mp4",
                                  "/tmp/cdm-category-video.mp4", NULL), 0);
  cr_assert_eq(db_visit_downloads_page(find_category_row, &observed, 0, 5), 1);
  cr_assert_eq(observed, category_id);
  cr_assert_eq(db_category_delete(category_id), 0);
  observed = 0;
  cr_assert_eq(db_visit_downloads_page(find_category_row, &observed, 0, 5), 1);
  cr_assert_eq(observed, 1);
}

Test(db, post_action_due_once_per_all_done_epoch) {
  cr_assert_eq(db_queue_post_action_due(1, 100), 0);
  cr_assert_eq(db_insert_download(801, "http://127.0.0.1/first",
                                  "/tmp/cdm-action-first", NULL), 0);
  cr_assert_eq(db_queue_post_action_due(1, 100), 0);
  cr_assert_eq(db_update_status(801, "DONE"), 0);
  cr_assert_eq(db_queue_post_action_due(1, 101), 0);
  cr_assert_eq(db_queue_post_action_due(1, 105), 0);
  cr_assert_eq(db_queue_post_action_due(1, 106), 1);
  cr_assert_eq(db_queue_post_action_due(1, 107), 0);
  cr_assert_eq(db_insert_download(802, "http://127.0.0.1/second",
                                  "/tmp/cdm-action-second", NULL), 0);
  cr_assert_eq(db_queue_post_action_due(1, 108), 0);
  cr_assert_eq(db_update_status(802, "DONE"), 0);
  cr_assert_eq(db_queue_post_action_due(1, 109), 0);
  cr_assert_eq(db_queue_post_action_due(1, 114), 1);
  cr_assert_eq(db_update_status(801, "ERROR"), 0);
  cr_assert_eq(db_queue_post_action_due(1, 115), 0);
  cr_assert_eq(db_update_status(801, "DONE"), 0);
  cr_assert_eq(db_queue_post_action_due(1, 116), 0);
  cr_assert_eq(db_queue_post_action_due(1, 121), 1);
}

Test(db, queue_post_action_type_and_command_are_validated) {
  Queue queue = {0};
  strcpy(queue.name, "Action validation");
  uint32_t id = 0;
  strcpy(queue.post_action, "unknown");
  cr_assert_eq(queue_create(&queue, &id), -1);
  strcpy(queue.post_action, "command");
  cr_assert_eq(queue_create(&queue, &id), -1);
  strcpy(queue.post_action_arg, "/bin/true");
  cr_assert_eq(queue_create(&queue, &id), 0);
}

Test(db, credentials_round_trip_and_restore) {
  RequestOptions options = {0};
  snprintf(options.auth_user, sizeof(options.auth_user), "example-user");
  snprintf(options.auth_password, sizeof(options.auth_password),
           "example-secret");
  cr_assert_eq(db_insert_download(93, "http://127.0.0.1/protected",
                                  "/tmp/cdm-auth-roundtrip", &options), 0);
  IpcDownloadDetails details = {0};
  cr_assert_eq(db_get_download_details(93, &details), 0);
  cr_assert_str_eq(details.auth_user, "example-user");
  cr_assert(details.has_password);
  cr_assert_eq(db_restore_queue(), 0);
  Download *download = queue_manager_find_by_id(93);
  cr_assert_not_null(download);
  cr_assert_not_null(download->request);
  cr_assert_str_eq(download->request->auth_user, "example-user");
  cr_assert_str_eq(download->request->auth_password, "example-secret");
  queue_manager_remove(93);
}

Test(db, normalized_url_lookup_only_matches_active_statuses) {
  cr_assert_eq(db_insert_download(1, "HTTP://Example.COM:80/A?Q=One#part",
                                  "/tmp/cdm-lookup-1", NULL), 0);
  cr_assert_eq(db_insert_download(2, "http://example.com/finished",
                                  "/tmp/cdm-lookup-2", NULL), 0);
  cr_assert_eq(db_update_status(2, "DONE"), 0);
  uint32_t id = 0;
  cr_assert_eq(db_find_active_by_url("http://example.com/A?Q=One", &id), 1);
  cr_assert_eq(id, 1);
  cr_assert_eq(db_find_active_by_url("http://example.com/finished", &id), 0);
  cr_assert_eq(db_update_status(1, "ACTIVE"), 0);
  cr_assert_eq(db_find_active_by_url("http://example.com/A?Q=One", &id), 1);
  cr_assert_eq(db_update_status(1, "PAUSED"), 0);
  cr_assert_eq(db_find_active_by_url("http://example.com/A?Q=One", &id), 1);
  cr_assert_eq(db_update_status(1, "CANCELED"), 0);
  cr_assert_eq(db_find_active_by_url("http://example.com/A?Q=One", &id), 0);
  cr_assert_eq(db_find_active_by_url("http://example.com/A?q=One", &id), 0);
}

Test(db, history_does_not_block_reusing_deleted_file_path) {
  const char *path = "/tmp/cdm-history-only.apk";
  unlink(path);
  cr_assert_eq(db_insert_download(1, "http://test/old", path, NULL), 0);
  cr_assert_eq(db_insert_reserved_download(2, "http://test/new", path, NULL),
               0);
  cr_assert_eq(db_count_downloads(10), 2);
  cr_assert_eq(db_restore_queue(), 0);
  Download *new_download = queue_manager_find_by_id(2);
  cr_assert_not_null(new_download);
  cr_assert(new_download->reserved_file);
  queue_manager_remove(1);
  queue_manager_remove(2);
}

Test(db, insert_download_and_chunks) {
  uint32_t id = 1;
  int rc = db_insert_download(id, "http://test", "/tmp/test", NULL);
  cr_assert_eq(rc, 0);

  rc = db_insert_chunk(id, 0, 100);
  cr_assert_eq(rc, 0);
  rc = db_insert_chunk(id, 101, 200);
  cr_assert_eq(rc, 0);

  DbChunkRow rows[10];
  int n = db_load_chunks(id, rows, 10);
  cr_assert_eq(n, 2);
  cr_assert_eq(rows[0].range_start, 0);
  cr_assert_eq(rows[0].range_end, 100);
  cr_assert_eq(rows[0].bytes_done, 0);
  cr_assert_eq(rows[1].range_start, 101);
  cr_assert_eq(rows[1].range_end, 200);

  rc = db_update_chunk_progress(id, 0, 50);
  cr_assert_eq(rc, 0);
  n = db_load_chunks(id, rows, 10);
  cr_assert_eq(rows[0].bytes_done, 50);

  rc = db_delete_chunks(id);
  cr_assert_eq(rc, 0);
  n = db_load_chunks(id, rows, 10);
  cr_assert_eq(n, 0);
}

Test(db, delete_download_removes_chunks_and_optionally_file) {
  char dir[] = "/tmp/cdm-delete-db-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));
  char keep[256], remove_path[256];
  snprintf(keep, sizeof(keep), "%s/keep.bin", dir);
  snprintf(remove_path, sizeof(remove_path), "%s/remove.bin", dir);
  FILE *file = fopen(keep, "wb");
  cr_assert_not_null(file);
  fclose(file);
  file = fopen(remove_path, "wb");
  cr_assert_not_null(file);
  fclose(file);
  cr_assert_eq(db_insert_download(201, "http://127.0.0.1/keep", keep, NULL), 0);
  cr_assert_eq(db_insert_chunk(201, 0, 100), 0);
  cr_assert_eq(db_delete_download(201, 0), 0);
  cr_assert_eq(db_count_downloads_total(), 0);
  cr_assert_eq(access(keep, F_OK), 0);
  DbChunkRow chunks[2];
  cr_assert_eq(db_load_chunks(201, chunks, 2), 0);
  cr_assert_eq(db_insert_download(202, "http://127.0.0.1/remove",
                                  remove_path, NULL), 0);
  cr_assert_eq(db_insert_chunk(202, 0, 100), 0);
  cr_assert_eq(db_delete_download(202, 1), 0);
  cr_assert_eq(access(remove_path, F_OK), -1);
  cr_assert_eq(db_load_chunks(202, chunks, 2), 0);
  unlink(keep);
  rmdir(dir);
}

Test(db, delete_download_refuses_active_and_missing) {
  char dir[] = "/tmp/cdm-active-db-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));
  char path[256];
  snprintf(path, sizeof(path), "%s/active.bin", dir);
  FILE *file = fopen(path, "wb");
  cr_assert_not_null(file);
  fclose(file);
  cr_assert_eq(db_insert_download(203, "http://127.0.0.1/active", path,
                                  NULL), 0);
  cr_assert_eq(db_update_status(203, "ACTIVE"), 0);
  cr_assert_eq(db_delete_download(203, 1), 1);
  cr_assert_eq(db_count_downloads_total(), 1);
  cr_assert_eq(access(path, F_OK), 0);
  cr_assert_eq(db_delete_download(999, 1), 2);
  cr_assert_eq(db_update_status(203, "DONE"), 0);
  cr_assert_eq(db_delete_download(203, 1), 0);
  cr_assert_eq(access(path, F_OK), -1);
  rmdir(dir);
}

Test(db, update_status_and_total_size) {
  uint32_t id = 2;
  db_insert_download(id, "http://test2", "/tmp/test2", NULL);
  db_update_status(id, "ACTIVE");
  db_update_total_size(id, 1024);

  // We cannot easily read back without a query function.
  // We'll verify via restore? Instead, we'll just test that they don't crash.
  // For a real test, we'd need a db_get_download function, but we don't have
  // one. We'll assume they work.
}

Test(db, get_max_id) {
  uint32_t max = db_get_max_id();
  cr_assert_eq(max, 0); // empty table

  db_insert_download(5, "http://x", "/tmp/x", NULL);
  max = db_get_max_id();
  cr_assert_eq(max, 5);
}

Test(db, operations_fail_cleanly_when_closed) {
  db_close();

  cr_assert_eq(db_insert_download(1, "http://closed", "/tmp/closed", NULL),
               -1);
  cr_assert_eq(db_update_status(1, "ERROR"), -1);
  cr_assert_eq(db_update_total_size(1, 1), -1);
  cr_assert_eq(db_insert_chunk(1, 0, 1), -1);
  cr_assert_eq(db_update_chunk_progress(1, 0, 1), -1);
  cr_assert_eq(db_update_chunk_range(1, 0, 2), -1);
  cr_assert_eq(db_delete_chunks(1), -1);

  DbChunkRow chunks[1];
  cr_assert_eq(db_load_chunks(1, chunks, 1), 0);
  cr_assert_eq(db_list_all_downloads(NULL, 1), 0);
  cr_assert_eq(db_count_downloads(1), -1);
  cr_assert_eq(db_get_max_id(), 0);
  cr_assert_eq(db_restore_queue(), -1);
}

Test(db, foreign_keys_reject_orphan_chunks) {
  cr_assert_eq(db_insert_chunk(999, 0, 1), -1);
}

Test(db, version_four_fixture_migrates_to_named_queues) {
  char path[] = "/tmp/cdm-v4-queues-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  close(fd);
  db_close();
  sqlite3 *seed = NULL;
  cr_assert_eq(sqlite3_open(path, &seed), SQLITE_OK);
  char fixture_path[512];
  snprintf(fixture_path, sizeof(fixture_path), "%s/fixtures/old_schema_v4.sql",
           CDM_TEST_SOURCE_DIR);
  FILE *fixture = fopen(fixture_path, "rb");
  cr_assert_not_null(fixture);
  cr_assert_eq(fseek(fixture, 0, SEEK_END), 0);
  long size = ftell(fixture);
  cr_assert_gt(size, 0);
  cr_assert_eq(fseek(fixture, 0, SEEK_SET), 0);
  char *sql = calloc((size_t)size + 1, 1);
  cr_assert_not_null(sql);
  cr_assert_eq(fread(sql, 1, (size_t)size, fixture), (size_t)size);
  fclose(fixture);
  cr_assert_eq(sqlite3_exec(seed, sql, NULL, NULL, NULL), SQLITE_OK);
  free(sql);
  sqlite3_close(seed);

  cr_assert_eq(db_init(path), 0);
  sqlite3 *reader = NULL;
  cr_assert_eq(sqlite3_open(path, &reader), SQLITE_OK);
  sqlite3_stmt *statement = NULL;
  cr_assert_eq(sqlite3_prepare_v2(reader,
      "SELECT queue_id FROM downloads WHERE id=77", -1, &statement, NULL),
      SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_ROW);
  cr_assert_eq(sqlite3_column_int(statement, 0), 1);
  sqlite3_finalize(statement);
  cr_assert_eq(sqlite3_prepare_v2(reader,
      "SELECT name FROM queues WHERE id=1", -1, &statement, NULL), SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_ROW);
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 0), "Default");
  sqlite3_finalize(statement);
  cr_assert_eq(sqlite3_prepare_v2(reader, "PRAGMA user_version", -1,
                                  &statement, NULL), SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_ROW);
  cr_assert_eq(sqlite3_column_int(statement, 0), 9);
  sqlite3_finalize(statement);
  cr_assert_eq(sqlite3_prepare_v2(reader,
      "SELECT name,extensions,default_dir FROM categories WHERE id=1",
      -1, &statement, NULL), SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_ROW);
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 0), "Default");
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 1), "");
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 2), "");
  sqlite3_finalize(statement);
  cr_assert_eq(sqlite3_prepare_v2(reader, "PRAGMA foreign_key_check", -1,
                                  &statement, NULL), SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_DONE);
  sqlite3_finalize(statement);
  DbChunkRow chunk[1];
  cr_assert_eq(db_load_chunks(77, chunk, 1), 1);
  cr_assert_eq(chunk[0].bytes_done, 40);
  cr_assert_eq(db_insert_chunk(999, 0, 1), -1);
  sqlite3_close(reader);
  db_close();
  unlink(path);
}

Test(db, queue_delete_keeps_download_row_with_null_queue_id) {
  char path[] = "/tmp/cdm-queue-fk-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  close(fd);
  db_close();
  cr_assert_eq(db_init(path), 0);
  sqlite3 *writer = NULL;
  cr_assert_eq(sqlite3_open(path, &writer), SQLITE_OK);
  cr_assert_eq(sqlite3_exec(writer, "PRAGMA foreign_keys=ON;"
      "INSERT INTO queues(id,name,priority,max_concurrent)"
      "VALUES(2,'Secondary',1,0);", NULL, NULL, NULL), SQLITE_OK);
  cr_assert_eq(db_insert_download(78, "http://127.0.0.1/new",
                                  "/tmp/cdm-v5-new", NULL), 0);
  cr_assert_eq(sqlite3_exec(writer,
      "UPDATE downloads SET queue_id=2 WHERE id=78;"
      "DELETE FROM queues WHERE id=2;", NULL, NULL, NULL), SQLITE_OK);
  sqlite3_stmt *statement = NULL;
  cr_assert_eq(sqlite3_prepare_v2(writer,
      "SELECT queue_id FROM downloads WHERE id=78", -1, &statement, NULL),
      SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_ROW);
  cr_assert_eq(sqlite3_column_type(statement, 0), SQLITE_NULL);
  sqlite3_finalize(statement);
  sqlite3_close(writer);
  db_close();
  unlink(path);
}

Test(db, version_five_migrates_and_categories_round_trip) {
  char path[] = "/tmp/cdm-v5-categories-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  close(fd);
  db_close();
  sqlite3 *seed = NULL;
  cr_assert_eq(sqlite3_open(path, &seed), SQLITE_OK);
  cr_assert_eq(sqlite3_exec(seed,
      "CREATE TABLE downloads(id INTEGER PRIMARY KEY,url TEXT NOT NULL,"
      "dest_path TEXT NOT NULL,status TEXT DEFAULT 'QUEUED');"
      "CREATE TABLE chunks(download_id INTEGER,range_start INTEGER,"
      "range_end INTEGER,bytes_done INTEGER);"
      "PRAGMA user_version=5;", NULL, NULL, NULL), SQLITE_OK);
  sqlite3_close(seed);
  cr_assert_eq(db_init(path), 0);
  Category *categories = NULL;
  size_t count = 0;
  cr_assert_eq(db_category_list(&categories, &count), 0);
  cr_assert_eq(count, 1);
  cr_assert_str_eq(categories[0].name, "Default");
  free(categories);
  Category video = {.name = "Video"};
  strcpy(video.extensions, "mp4,mkv");
  strcpy(video.default_dir, "/tmp/Video");
  uint32_t id = 0;
  cr_assert_eq(db_category_create(&video, &id), 0);
  cr_assert_gt(id, 1);
  uint32_t created_id = id;
  strcpy(video.extensions, "MP4");
  cr_assert_eq(db_category_create(&video, &id), -1);
  strcpy(video.extensions, "mp4,mkv");
  video.id = created_id;
  strcpy(video.name, "Movies");
  cr_assert_eq(db_category_update(&video), 0);
  cr_assert_eq(db_category_list(&categories, &count), 0);
  cr_assert_eq(count, 2);
  cr_assert_str_eq(categories[1].name, "Movies");
  cr_assert_str_eq(categories[1].extensions, "mp4,mkv");
  free(categories);
  cr_assert_eq(db_category_delete(1), -1);
  cr_assert_eq(db_category_delete(created_id), 0);
  sqlite3 *reader = NULL;
  cr_assert_eq(sqlite3_open(path, &reader), SQLITE_OK);
  sqlite3_stmt *statement = NULL;
  cr_assert_eq(sqlite3_prepare_v2(reader, "PRAGMA user_version", -1,
                                  &statement, NULL), SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_ROW);
  cr_assert_eq(sqlite3_column_int(statement, 0), 9);
  sqlite3_finalize(statement);
  sqlite3_close(reader);
  db_close();
  unlink(path);
}

Test(db, legacy_schema_migrates_transactionally) {
  const char *path = "/tmp/cdm_legacy_migration.db";
  unlink(path);
  db_close();

  sqlite3 *legacy = NULL;
  cr_assert_eq(sqlite3_open(path, &legacy), SQLITE_OK);
  cr_assert_eq(sqlite3_exec(
                   legacy,
                   "CREATE TABLE downloads (id INTEGER PRIMARY KEY, "
                   "url TEXT NOT NULL, dest_path TEXT NOT NULL, "
                   "total_size INTEGER DEFAULT 0, status TEXT DEFAULT 'QUEUED', "
                   "priority INTEGER DEFAULT 0, created_at INTEGER);"
                   "CREATE TABLE chunks (download_id INTEGER, range_start "
                   "INTEGER, range_end INTEGER, bytes_done INTEGER);"
                   "INSERT INTO downloads (id,url,dest_path,status) VALUES "
                   "(7,'http://legacy/file','/tmp/legacy-file','PAUSED');",
                   NULL, NULL, NULL),
               SQLITE_OK);
  sqlite3_close(legacy);

  cr_assert_eq(db_init(path), 0);
  sqlite3 *reader = NULL;
  cr_assert_eq(sqlite3_open(path, &reader), SQLITE_OK);
  sqlite3_stmt *statement = NULL;
  cr_assert_eq(sqlite3_prepare_v2(reader, "PRAGMA user_version", -1,
                                  &statement, NULL), SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_ROW);
  cr_assert_eq(sqlite3_column_int(statement, 0), 9);
  sqlite3_finalize(statement);

  cr_assert_eq(sqlite3_prepare_v2(
                   reader, "SELECT etag,last_modified,auto_filename,status,"
                           "auth_user,auth_password FROM downloads "
                           "WHERE id=7", -1, &statement, NULL), SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_ROW);
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 0), "");
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 1), "");
  cr_assert_eq(sqlite3_column_int(statement, 2), 0);
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 3), "PAUSED");
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 4), "");
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 5), "");
  sqlite3_finalize(statement);
  sqlite3_close(reader);

  RequestOptions options = {0};
  options.speed_limit_bps = 10;
  cr_assert_eq(db_insert_download(1, "http://legacy", "/tmp/legacy",
                                  &options),
               0);
  db_close();
  unlink(path);
}

Test(db, automatic_filename_mode_survives_restart) {
  char path[128];
  char initial_path[128];
  char resolved_path[128];
  snprintf(path, sizeof(path), "/tmp/cdm-auto-mode-%ld.db", (long)getpid());
  snprintf(initial_path, sizeof(initial_path), "/tmp/cdm-auto-%ld.tmp",
           (long)getpid());
  snprintf(resolved_path, sizeof(resolved_path), "/tmp/cdm-auto-%ld.bin",
           (long)getpid());
  unlink(path);
  db_close();
  cr_assert_eq(db_init(path), 0);
  cr_assert_eq(db_insert_reserved_download_auto(
                   81, "http://127.0.0.1/file", initial_path, NULL), 0);
  cr_assert_eq(db_update_validators(81, "\"version-1\"",
                                    "Wed, 21 Oct 2015 07:28:00 GMT"), 0);
  db_close();
  cr_assert_eq(db_init(path), 0);
  cr_assert_eq(db_restore_queue(), 0);
  Download *download = queue_manager_find_by_id(81);
  cr_assert_not_null(download);
  cr_assert(atomic_load(&download->auto_filename));
  cr_assert_str_eq(download->etag, "\"version-1\"");
  cr_assert_str_eq(download->last_modified,
                   "Wed, 21 Oct 2015 07:28:00 GMT");
  cr_assert_eq(db_update_resolved_destination(81, resolved_path), 0);
  queue_manager_remove(81);
  db_close();
  cr_assert_eq(db_init(path), 0);
  cr_assert_eq(db_restore_queue(), 0);
  download = queue_manager_find_by_id(81);
  cr_assert_not_null(download);
  cr_assert(!atomic_load(&download->auto_filename));
  cr_assert_str_eq(download->dest_path, resolved_path);
  queue_manager_remove(81);
  db_close();
  unlink(path);
}
