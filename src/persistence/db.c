// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "db.h"

#include "../core/queue_manager.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Global state                                                      */
/* ------------------------------------------------------------------ */

static sqlite3 *g_db = NULL;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

int db_init(const char *db_path) {
  int rc = sqlite3_open(db_path, &g_db);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Cannot open database %s: %s\n", db_path,
            sqlite3_errmsg(g_db));
    return -1;
  }

  /* Enable WAL for better concurrent read performance. */
  sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

  const char *schema =
      "CREATE TABLE IF NOT EXISTS downloads ("
      "  id              INTEGER PRIMARY KEY,"
      "  url             TEXT NOT NULL,"
      "  dest_path       TEXT NOT NULL,"
      "  total_size      INTEGER DEFAULT 0,"
      "  status          TEXT DEFAULT 'QUEUED',"
      "  priority        INTEGER DEFAULT 0,"
      "  created_at      INTEGER,"
      "  cookie          TEXT DEFAULT '',"
      "  referrer        TEXT DEFAULT '',"
      "  extra_headers   TEXT DEFAULT '',"
      "  expected_sha256 TEXT DEFAULT '',"
      "  speed_limit_bps INTEGER DEFAULT 0"
      ");"
      ""
      "CREATE TABLE IF NOT EXISTS chunks ("
      "  download_id INTEGER,"
      "  range_start INTEGER,"
      "  range_end   INTEGER,"
      "  bytes_done  INTEGER DEFAULT 0,"
      "  FOREIGN KEY(download_id) REFERENCES downloads(id) ON DELETE CASCADE"
      ");";

  char *err_msg = NULL;
  rc = sqlite3_exec(g_db, schema, NULL, NULL, &err_msg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Schema creation failed: %s\n", err_msg);
    sqlite3_free(err_msg);
    return -1;
  }

  /* Migrate older databases that lack request‑option columns.
     Silently ignore errors — the column already exists if the error
     is "duplicate column". */
  static const char *new_columns[] = {
      "cookie TEXT DEFAULT ''", "referrer TEXT DEFAULT ''",
      "extra_headers TEXT DEFAULT ''", "expected_sha256 TEXT DEFAULT ''",
      "speed_limit_bps INTEGER DEFAULT 0"};
  for (size_t i = 0; i < sizeof(new_columns) / sizeof(new_columns[0]); i++) {
    char sql[256];
    snprintf(sql, sizeof(sql), "ALTER TABLE downloads ADD COLUMN %s",
             new_columns[i]);
    sqlite3_exec(g_db, sql, NULL, NULL, NULL);
  }

  return 0;
}

void db_close(void) {
  if (g_db) {
    sqlite3_close(g_db);
    g_db = NULL;
  }
}

/* ------------------------------------------------------------------ */
/*  Download persistence                                              */
/* ------------------------------------------------------------------ */

int db_insert_download(uint32_t id, const char *url, const char *dest_path,
                       const RequestOptions *opts) {
  const char *sql =
      "INSERT OR REPLACE INTO downloads "
      "(id, url, dest_path, status, created_at, cookie, referrer, "
      "extra_headers, expected_sha256, speed_limit_bps) "
      "VALUES (?, ?, ?, 'QUEUED', ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_insert_download: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int(stmt, 1, (int)id);
  sqlite3_bind_text(stmt, 2, url, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, dest_path, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
  sqlite3_bind_text(stmt, 5, opts ? opts->cookie : "", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 6, opts ? opts->referrer : "", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 7, opts ? opts->extra_headers : "", -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 8, opts ? opts->expected_sha256 : "", -1,
                    SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 9,
                     (sqlite3_int64)(opts ? opts->speed_limit_bps : 0));

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_status(uint32_t id, const char *status) {
  const char *sql = "UPDATE downloads SET status = ? WHERE id = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_update_status: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, (int)id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_total_size(uint32_t id, uint64_t total_size) {
  const char *sql = "UPDATE downloads SET total_size = ? WHERE id = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_update_total_size: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)total_size);
  sqlite3_bind_int(stmt, 2, (int)id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Chunk persistence                                                 */
/* ------------------------------------------------------------------ */

int db_insert_chunk(uint32_t download_id, uint64_t range_start,
                    uint64_t range_end) {
  const char *sql =
      "INSERT INTO chunks (download_id, range_start, range_end, bytes_done) "
      "VALUES (?, ?, ?, 0)";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_insert_chunk: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int(stmt, 1, (int)download_id);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)range_start);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)range_end);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_chunk_progress(uint32_t download_id, uint64_t range_start,
                             uint64_t bytes_done) {
  const char *sql = "UPDATE chunks SET bytes_done = ? "
                    "WHERE download_id = ? AND range_start = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_update_chunk_progress: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)bytes_done);
  sqlite3_bind_int(stmt, 2, (int)download_id);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)range_start);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_chunk_range(uint32_t download_id, uint64_t range_start,
                          uint64_t new_range_end) {
  const char *sql = "UPDATE chunks SET range_end = ? "
                    "WHERE download_id = ? AND range_start = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_update_chunk_range: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)new_range_end);
  sqlite3_bind_int(stmt, 2, (int)download_id);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)range_start);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_delete_chunks(uint32_t download_id) {
  const char *sql = "DELETE FROM chunks WHERE download_id = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_delete_chunks: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int(stmt, 1, (int)download_id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Bulk queries / restore                                            */
/* ------------------------------------------------------------------ */

int db_load_chunks(uint32_t download_id, DbChunkRow *out, int max) {
  const char *sql = "SELECT range_start, range_end, bytes_done FROM chunks "
                    "WHERE download_id = ? ORDER BY range_start";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_int(stmt, 1, (int)download_id);

  int n = 0;
  while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
    out[n].range_start = (uint64_t)sqlite3_column_int64(stmt, 0);
    out[n].range_end = (uint64_t)sqlite3_column_int64(stmt, 1);
    out[n].bytes_done = (uint64_t)sqlite3_column_int64(stmt, 2);
    n++;
  }
  sqlite3_finalize(stmt);
  return n;
}

int db_list_all_downloads(DbDownloadRow *out, int max) {
  const char *sql =
      "SELECT id, url, dest_path, status, total_size FROM downloads "
      "ORDER BY id DESC LIMIT ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_list_all_downloads: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return 0;
  }
  sqlite3_bind_int(stmt, 1, max);

  int n = 0;
  while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
    out[n].id = (uint32_t)sqlite3_column_int(stmt, 0);
    const char *url = (const char *)sqlite3_column_text(stmt, 1);
    const char *path = (const char *)sqlite3_column_text(stmt, 2);
    const char *stat = (const char *)sqlite3_column_text(stmt, 3);
    out[n].total_size = (uint64_t)sqlite3_column_int64(stmt, 4);

    strncpy(out[n].url, url ? url : "", sizeof(out[n].url) - 1);
    out[n].url[sizeof(out[n].url) - 1] = '\0';
    strncpy(out[n].dest_path, path ? path : "", sizeof(out[n].dest_path) - 1);
    out[n].dest_path[sizeof(out[n].dest_path) - 1] = '\0';
    strncpy(out[n].status, stat ? stat : "", sizeof(out[n].status) - 1);
    out[n].status[sizeof(out[n].status) - 1] = '\0';
    n++;
  }
  sqlite3_finalize(stmt);
  return n;
}

uint32_t db_get_max_id(void) {
  const char *sql = "SELECT MAX(id) FROM downloads";
  sqlite3_stmt *stmt = NULL;
  uint32_t max_id = 0;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_get_max_id: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return 0;
  }
  if (sqlite3_step(stmt) == SQLITE_ROW)
    max_id = (uint32_t)sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return max_id;
}

int db_restore_queue(void) {
  const char *sql = "SELECT id, url, dest_path, total_size, status, priority, "
                    "cookie, referrer, extra_headers, expected_sha256, "
                    "speed_limit_bps "
                    "FROM downloads WHERE status != 'DONE'";

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "db_restore_queue: prepare failed: %s\n",
            sqlite3_errmsg(g_db));
    return -1;
  }

  int restored = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    Download *d = calloc(1, sizeof(Download));
    if (!d)
      continue;

    d->id = (uint32_t)sqlite3_column_int(stmt, 0);
    d->total_size = (uint64_t)sqlite3_column_int64(stmt, 3);
    d->priority = sqlite3_column_int(stmt, 5);

    const char *url = (const char *)sqlite3_column_text(stmt, 1);
    const char *path = (const char *)sqlite3_column_text(stmt, 2);
    const char *stat = (const char *)sqlite3_column_text(stmt, 4);
    const char *cookie = (const char *)sqlite3_column_text(stmt, 6);
    const char *referrer = (const char *)sqlite3_column_text(stmt, 7);
    const char *headers = (const char *)sqlite3_column_text(stmt, 8);
    const char *sha256 = (const char *)sqlite3_column_text(stmt, 9);
    uint64_t speed_limit = (uint64_t)sqlite3_column_int64(stmt, 10);

    strncpy(d->url, url ? url : "", sizeof(d->url) - 1);
    strncpy(d->dest_path, path ? path : "", sizeof(d->dest_path) - 1);
    strncpy(d->request.cookie, cookie ? cookie : "",
            sizeof(d->request.cookie) - 1);
    strncpy(d->request.referrer, referrer ? referrer : "",
            sizeof(d->request.referrer) - 1);
    strncpy(d->request.extra_headers, headers ? headers : "",
            sizeof(d->request.extra_headers) - 1);
    strncpy(d->request.expected_sha256, sha256 ? sha256 : "",
            sizeof(d->request.expected_sha256) - 1);
    d->request.speed_limit_bps = speed_limit;

    /* Map status string to enum. */
    if (strcmp(stat, "QUEUED") == 0)
      d->status = DOWNLOAD_QUEUED;
    else if (strcmp(stat, "ACTIVE") == 0)
      d->status = DOWNLOAD_QUEUED; // re‑queue with resume data
    else if (strcmp(stat, "PAUSED") == 0)
      d->status = DOWNLOAD_PAUSED;
    else if (strcmp(stat, "ERROR") == 0)
      d->status = DOWNLOAD_ERROR;
    else
      d->status = DOWNLOAD_QUEUED;

    /* Restore chunk information so resume can continue. */
    DbChunkRow rows[QM_MAX_CHUNKS];
    int n_chunks = db_load_chunks(d->id, rows, QM_MAX_CHUNKS);
    for (int i = 0; i < n_chunks; i++) {
      d->chunks[i].range_start = rows[i].range_start;
      d->chunks[i].range_end = rows[i].range_end;
      d->chunks[i].bytes_done = rows[i].bytes_done;
    }
    d->chunk_count = n_chunks;

    queue_manager_add_existing(d);
    restored++;
  }

  sqlite3_finalize(stmt);
  printf("Restored %d download(s) from database\n", restored);
  return 0;
}
