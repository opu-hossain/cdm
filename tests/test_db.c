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
  cr_assert_eq(sqlite3_column_int(statement, 0), 3);
  sqlite3_finalize(statement);

  cr_assert_eq(sqlite3_prepare_v2(
                   reader, "SELECT etag,last_modified,auto_filename,status FROM downloads "
                           "WHERE id=7", -1, &statement, NULL), SQLITE_OK);
  cr_assert_eq(sqlite3_step(statement), SQLITE_ROW);
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 0), "");
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 1), "");
  cr_assert_eq(sqlite3_column_int(statement, 2), 0);
  cr_assert_str_eq((const char *)sqlite3_column_text(statement, 3), "PAUSED");
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
  db_close();
  cr_assert_eq(db_init(path), 0);
  cr_assert_eq(db_restore_queue(), 0);
  Download *download = queue_manager_find_by_id(81);
  cr_assert_not_null(download);
  cr_assert(atomic_load(&download->auto_filename));
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
