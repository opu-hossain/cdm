// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "db.h"

#include "../core/queue_manager.h"
#include "../utils/log.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Global state                                                      */
/* ------------------------------------------------------------------ */

static sqlite3 *g_db = NULL;

static bool db_ready(void) { return g_db != NULL; }

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

int db_init(const char *db_path) {
  if (!db_path || db_path[0] == '\0')
    return -1;

  db_close();
  sqlite3 *opened_db = NULL;
  int rc = sqlite3_open(db_path, &opened_db);
  if (rc != SQLITE_OK) {
    LOG_ERROR("cannot open database %s: %s", db_path,
              opened_db ? sqlite3_errmsg(opened_db) : "unknown error");
    if (opened_db)
      sqlite3_close(opened_db);
    return -1;
  }
  g_db = opened_db;

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
    LOG_ERROR("schema creation failed: %s", err_msg);
    sqlite3_free(err_msg);
    db_close();
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
    char *migration_error = NULL;
    rc = sqlite3_exec(g_db, sql, NULL, NULL, &migration_error);
    if (rc != SQLITE_OK &&
        (!migration_error ||
         strstr(migration_error, "duplicate column name") == NULL)) {
      LOG_ERROR("migration failed for '%s': %s", sql,
                migration_error ? migration_error : "unknown error");
      sqlite3_free(migration_error);
      db_close();
      return -1;
    }
    sqlite3_free(migration_error);
  }

  LOG_INFO("opened %s", db_path);
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
  if (!db_ready() || !url || !dest_path)
    return -1;
  const char *sql =
      "INSERT OR REPLACE INTO downloads "
      "(id, url, dest_path, status, created_at, cookie, referrer, "
      "extra_headers, expected_sha256, speed_limit_bps) "
      "VALUES (?, ?, ?, 'QUEUED', ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
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
  if (rc != SQLITE_DONE) {
    LOG_ERROR("insert failed for id=%u: %s", id, sqlite3_errmsg(g_db));
  }
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_status(uint32_t id, const char *status) {
  if (!db_ready() || !status)
    return -1;
  const char *sql = "UPDATE downloads SET status = ? WHERE id = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, (int)id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    LOG_WARN("update failed for id=%u (%s)", id, status);
  }
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_total_size(uint32_t id, uint64_t total_size) {
  if (!db_ready())
    return -1;
  const char *sql = "UPDATE downloads SET total_size = ? WHERE id = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)total_size);
  sqlite3_bind_int(stmt, 2, (int)id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    LOG_WARN("update failed for id=%u", id);
  }
  return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Chunk persistence                                                 */
/* ------------------------------------------------------------------ */

int db_insert_chunk(uint32_t download_id, uint64_t range_start,
                    uint64_t range_end) {
  if (!db_ready())
    return -1;
  const char *sql =
      "INSERT INTO chunks (download_id, range_start, range_end, bytes_done) "
      "VALUES (?, ?, ?, 0)";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int(stmt, 1, (int)download_id);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)range_start);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)range_end);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    LOG_WARN("insert failed for download_id=%u", download_id);
  }
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_chunk_progress(uint32_t download_id, uint64_t range_start,
                             uint64_t bytes_done) {
  if (!db_ready())
    return -1;
  const char *sql = "UPDATE chunks SET bytes_done = ? "
                    "WHERE download_id = ? AND range_start = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)bytes_done);
  sqlite3_bind_int(stmt, 2, (int)download_id);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)range_start);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    LOG_WARN("update failed for download_id=%u", download_id);
  }
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_chunk_range(uint32_t download_id, uint64_t range_start,
                          uint64_t new_range_end) {
  if (!db_ready())
    return -1;
  const char *sql = "UPDATE chunks SET range_end = ? "
                    "WHERE download_id = ? AND range_start = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)new_range_end);
  sqlite3_bind_int(stmt, 2, (int)download_id);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)range_start);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    LOG_WARN("update failed for download_id=%u", download_id);
  }
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_delete_chunks(uint32_t download_id) {
  if (!db_ready())
    return -1;
  const char *sql = "DELETE FROM chunks WHERE download_id = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int(stmt, 1, (int)download_id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    LOG_WARN("delete failed for download_id=%u", download_id);
  }
  return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Bulk queries / restore                                            */
/* ------------------------------------------------------------------ */

int db_load_chunks(uint32_t download_id, DbChunkRow *out, int max) {
  if (!db_ready() || !out || max <= 0)
    return 0;
  const char *sql = "SELECT range_start, range_end, bytes_done FROM chunks "
                    "WHERE download_id = ? ORDER BY range_start";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_WARN("prepare failed for download_id=%u: %s", download_id,
             sqlite3_errmsg(g_db));
    return 0;
  }
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
  if (!db_ready() || !out || max <= 0)
    return 0;
  const char *sql =
      "SELECT id, url, dest_path, status FROM downloads "
      "ORDER BY id DESC LIMIT ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return 0;
  }
  sqlite3_bind_int(stmt, 1, max);

  int n = 0;
  while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
    out[n].id = (uint32_t)sqlite3_column_int(stmt, 0);
    const char *url = (const char *)sqlite3_column_text(stmt, 1);
    const char *path = (const char *)sqlite3_column_text(stmt, 2);
    const char *stat = (const char *)sqlite3_column_text(stmt, 3);

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

int db_count_downloads(int max) {
  if (!db_ready() || max < 0)
    return -1;
  const char *sql = "SELECT COUNT(*) FROM downloads";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;

  int count = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    sqlite3_int64 total = sqlite3_column_int64(stmt, 0);
    count = (total > max) ? max : (int)total;
  }
  sqlite3_finalize(stmt);
  return count;
}

int db_visit_downloads(DbDownloadVisitor visitor, void *ctx, int max) {
  if (!db_ready() || !visitor || max <= 0)
    return -1;

  const char *sql =
      "SELECT id, url, dest_path, status FROM downloads "
      "ORDER BY id DESC LIMIT ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, max);

  int visited = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    DbDownloadRow row = {0};
    row.id = (uint32_t)sqlite3_column_int(stmt, 0);

    const char *url = (const char *)sqlite3_column_text(stmt, 1);
    const char *path = (const char *)sqlite3_column_text(stmt, 2);
    const char *status = (const char *)sqlite3_column_text(stmt, 3);
    strncpy(row.url, url ? url : "", sizeof(row.url) - 1);
    strncpy(row.dest_path, path ? path : "", sizeof(row.dest_path) - 1);
    strncpy(row.status, status ? status : "", sizeof(row.status) - 1);

    if (visitor(&row, ctx) != 0)
      break;
    visited++;
  }
  sqlite3_finalize(stmt);
  return visited;
}

int db_get_download_details(uint32_t id, IpcDownloadDetails *out) {
  if (!db_ready() || !out)
    return -1;
  const char *sql = "SELECT cookie, referrer, extra_headers, expected_sha256, "
                    "speed_limit_bps FROM downloads WHERE id = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int(stmt, 1, (int)id);

  int rc = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *cookie = (const char *)sqlite3_column_text(stmt, 0);
    const char *referrer = (const char *)sqlite3_column_text(stmt, 1);
    const char *headers = (const char *)sqlite3_column_text(stmt, 2);
    const char *sha256 = (const char *)sqlite3_column_text(stmt, 3);
    out->speed_limit_bps = (uint64_t)sqlite3_column_int64(stmt, 4);

    strncpy(out->cookie, cookie ? cookie : "", sizeof(out->cookie) - 1);
    out->cookie[sizeof(out->cookie) - 1] = '\0';
    strncpy(out->referrer, referrer ? referrer : "", sizeof(out->referrer) - 1);
    out->referrer[sizeof(out->referrer) - 1] = '\0';
    strncpy(out->extra_headers, headers ? headers : "",
            sizeof(out->extra_headers) - 1);
    out->extra_headers[sizeof(out->extra_headers) - 1] = '\0';
    strncpy(out->expected_sha256, sha256 ? sha256 : "",
            sizeof(out->expected_sha256) - 1);
    out->expected_sha256[sizeof(out->expected_sha256) - 1] = '\0';
    rc = 0;
  }
  sqlite3_finalize(stmt);
  return rc;
}

uint32_t db_get_max_id(void) {
  if (!db_ready())
    return 0;
  const char *sql = "SELECT MAX(id) FROM downloads";
  sqlite3_stmt *stmt = NULL;
  uint32_t max_id = 0;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return 0;
  }
  if (sqlite3_step(stmt) == SQLITE_ROW)
    max_id = (uint32_t)sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return max_id;
}

int db_restore_queue(void) {
  if (!db_ready())
    return -1;
  const char *sql = "SELECT id, url, dest_path, total_size, status, priority, "
                    "cookie, referrer, extra_headers, expected_sha256, "
                    "speed_limit_bps "
                    "FROM downloads WHERE status != 'DONE'";

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
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

    if (stat && strcmp(stat, "CANCELED") == 0) {
      free(d);
      continue;
    }

    strncpy(d->url, url ? url : "", sizeof(d->url) - 1);
    strncpy(d->dest_path, path ? path : "", sizeof(d->dest_path) - 1);
        RequestOptions options = {0};
        strncpy(options.cookie, cookie ? cookie : "", sizeof(options.cookie) - 1);
        strncpy(options.referrer, referrer ? referrer : "",
          sizeof(options.referrer) - 1);
        strncpy(options.extra_headers, headers ? headers : "",
          sizeof(options.extra_headers) - 1);
        strncpy(options.expected_sha256, sha256 ? sha256 : "",
          sizeof(options.expected_sha256) - 1);
        options.speed_limit_bps = speed_limit;

        if (options.cookie[0] != '\0' || options.referrer[0] != '\0' ||
      options.extra_headers[0] != '\0' ||
      options.expected_sha256[0] != '\0' || options.speed_limit_bps != 0) {
          d->request = malloc(sizeof(*d->request));
          if (!d->request) {
      free(d);
      continue;
          }
          *d->request = options;
        }

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
  LOG_INFO("restored %d download(s) from database", restored);
  return 0;
}
