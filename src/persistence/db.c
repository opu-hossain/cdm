// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "db.h"

#include "../core/queue_manager.h"
#include "../utils/log.h"
#include "../utils/url.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

/* Global state */

static sqlite3 *g_db = NULL;

static bool db_ready(void) { return g_db != NULL; }

static bool db_column_exists(const char *table, const char *column) {
  char sql[128];
  snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return false;

  bool found = false;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    if (name && strcmp(name, column) == 0) {
      found = true;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

/* Lifecycle */

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

  char *pragma_error = NULL;
  rc = sqlite3_exec(g_db, "PRAGMA foreign_keys = ON;", NULL, NULL,
                    &pragma_error);
  if (rc != SQLITE_OK) {
    LOG_ERROR("failed to enable SQLite foreign keys: %s",
              pragma_error ? pragma_error : "unknown error");
    sqlite3_free(pragma_error);
    db_close();
    return -1;
  }
  sqlite3_free(pragma_error);

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
      "  speed_limit_bps INTEGER DEFAULT 0,"
      "  reserved_file   INTEGER DEFAULT 0,"
      "  etag            TEXT DEFAULT '',"
      "  last_modified   TEXT DEFAULT '',"
      "  auto_filename   INTEGER DEFAULT 0,"
      "  auth_user       TEXT DEFAULT '',"
      "  auth_password   TEXT DEFAULT ''"
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

  static const char *migration_names[] = {"cookie", "referrer", "extra_headers",
                                          "expected_sha256", "speed_limit_bps",
                                          "reserved_file", "etag",
                                          "last_modified", "auto_filename",
                                          "auth_user", "auth_password"};
  static const char *migration_types[] = {"TEXT DEFAULT ''", "TEXT DEFAULT ''",
                                          "TEXT DEFAULT ''", "TEXT DEFAULT ''",
                                          "INTEGER DEFAULT 0", "INTEGER DEFAULT 0",
                                          "TEXT DEFAULT ''", "TEXT DEFAULT ''",
                                          "INTEGER DEFAULT 0", "TEXT DEFAULT ''",
                                          "TEXT DEFAULT ''"};

  char *migration_error = NULL;
  rc = sqlite3_exec(g_db, "BEGIN;", NULL, NULL, &migration_error);
  if (rc != SQLITE_OK) {
    LOG_ERROR("could not begin database migration: %s",
              migration_error ? migration_error : "unknown error");
    sqlite3_free(migration_error);
    db_close();
    return -1;
  }
  sqlite3_free(migration_error);

  for (size_t i = 0; i < sizeof(migration_names) / sizeof(migration_names[0]);
       i++) {
    if (db_column_exists("downloads", migration_names[i]))
      continue;
    char sql[256];
    snprintf(sql, sizeof(sql), "ALTER TABLE downloads ADD COLUMN %s %s",
             migration_names[i], migration_types[i]);
    rc = sqlite3_exec(g_db, sql, NULL, NULL, &migration_error);
    if (rc != SQLITE_OK) {
      LOG_ERROR("migration failed for '%s': %s", sql,
                migration_error ? migration_error : "unknown error");
      sqlite3_free(migration_error);
      sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
      db_close();
      return -1;
    }
    sqlite3_free(migration_error);
  }

  rc = sqlite3_exec(g_db, "PRAGMA user_version = 4; COMMIT;", NULL, NULL,
                    &migration_error);
  if (rc != SQLITE_OK) {
    LOG_ERROR("could not commit database migration: %s",
              migration_error ? migration_error : "unknown error");
    sqlite3_free(migration_error);
    sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
    db_close();
    return -1;
  }
  sqlite3_free(migration_error);

  LOG_INFO("opened %s", db_path);
  return 0;
}

void db_close(void) {
  if (g_db) {
    sqlite3_close(g_db);
    g_db = NULL;
  }
}

static int delete_rows_for_id(const char *sql, uint32_t id) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_delete_download(uint32_t id, int delete_file) {
  if (!db_ready() || id == 0 ||
      sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
    return -1;

  int result = -1;
  char *path = NULL;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db,
                         "SELECT status, dest_path FROM downloads WHERE id = ?",
                         -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
  int step = sqlite3_step(stmt);
  if (step == SQLITE_DONE) {
    result = 2;
    goto rollback;
  }
  if (step != SQLITE_ROW)
    goto rollback;
  const char *status = (const char *)sqlite3_column_text(stmt, 0);
  const char *stored_path = (const char *)sqlite3_column_text(stmt, 1);
  if (status && strcmp(status, "ACTIVE") == 0) {
    result = 1;
    goto rollback;
  }
  if (!stored_path || !(path = strdup(stored_path)))
    goto rollback;
  sqlite3_finalize(stmt);
  stmt = NULL;

  if (delete_rows_for_id("DELETE FROM chunks WHERE download_id = ?", id) != 0 ||
      delete_rows_for_id("DELETE FROM downloads WHERE id = ?", id) != 0)
    goto rollback;
  if (sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
    goto rollback;
  result = 0;
  if (delete_file && unlink(path) != 0)
    LOG_WARN("could not delete file for download %u (%s): %s", id, path,
             strerror(errno));
  free(path);
  return result;

rollback:
  if (stmt)
    sqlite3_finalize(stmt);
  sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
  free(path);
  return result;
}

/* Download persistence */

static int insert_download(uint32_t id, const char *url,
                           const char *dest_path, const RequestOptions *opts,
                           bool reserved, bool auto_filename) {
  if (!db_ready() || !url || !dest_path)
    return -1;

  const char *sql =
      "INSERT OR REPLACE INTO downloads "
      "(id, url, dest_path, status, created_at, cookie, referrer, "
      "extra_headers, expected_sha256, speed_limit_bps, reserved_file, "
      "auto_filename, auth_user, auth_password) "
      "VALUES (?, ?, ?, 'QUEUED', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
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
  sqlite3_bind_int(stmt, 10, reserved ? 1 : 0);
  sqlite3_bind_int(stmt, 11, auto_filename ? 1 : 0);
  sqlite3_bind_text(stmt, 12, opts ? opts->auth_user : "", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 13, opts ? opts->auth_password : "", -1,
                    SQLITE_STATIC);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("insert failed for id=%u: %s", id, sqlite3_errmsg(g_db));
  }
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_insert_download(uint32_t id, const char *url, const char *dest_path,
                       const RequestOptions *opts) {
  return insert_download(id, url, dest_path, opts, false, false);
}

int db_insert_reserved_download(uint32_t id, const char *url,
                                const char *dest_path,
                                const RequestOptions *opts) {
  return insert_download(id, url, dest_path, opts, true, false);
}

int db_insert_reserved_download_auto(uint32_t id, const char *url,
                                     const char *dest_path,
                                     const RequestOptions *opts) {
  return insert_download(id, url, dest_path, opts, true, true);
}

int db_update_resolved_destination(uint32_t id, const char *dest_path) {
  if (!db_ready() || !dest_path)
    return -1;
  const char *sql = "UPDATE downloads SET dest_path = ?, auto_filename = 0 "
                    "WHERE id = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, dest_path, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, (int)id);
  int rc = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE && changed == 1 ? 0 : -1;
}

bool db_destination_exists(const char *dest_path) {
  if (!db_ready() || !dest_path)
    return false;

  const char *sql = "SELECT 1 FROM downloads WHERE dest_path = ? LIMIT 1";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return false;
  sqlite3_bind_text(stmt, 1, dest_path, -1, SQLITE_STATIC);
  bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return exists;
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

int db_update_validators(uint32_t id, const char *etag,
                         const char *last_modified) {
  if (!db_ready() || !etag || !last_modified)
    return -1;
  const char *sql = "UPDATE downloads SET etag = ?, last_modified = ? "
                    "WHERE id = ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, etag, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, last_modified, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 3, (int)id);
  int rc = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE && changed == 1 ? 0 : -1;
}

/* Chunk persistence */

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

/* Bulk queries / restore */

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
  const char *sql = "SELECT id, url, dest_path, status, total_size FROM downloads "
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
    out[n].total_size = (uint64_t)sqlite3_column_int64(stmt, 4);
    n++;
  }
  sqlite3_finalize(stmt);
  return n;
}

int db_count_downloads(int max) {
  if (max < 0)
    return -1;
  int64_t total = db_count_downloads_total();
  if (total < 0)
    return -1;
  return total > max ? max : (int)total;
}

int64_t db_count_downloads_total(void) {
  if (!db_ready())
    return -1;
  const char *sql = "SELECT COUNT(*) FROM downloads";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;

  int64_t count = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

int db_visit_downloads(DbDownloadVisitor visitor, void *ctx, int max) {
  if (max <= 0)
    return -1;
  return db_visit_downloads_page(visitor, ctx, 0, (uint32_t)max);
}

int db_visit_downloads_page(DbDownloadVisitor visitor, void *ctx,
                            uint32_t offset, uint32_t limit) {
  if (!db_ready() || !visitor || limit == 0)
    return -1;

  const char *sql = "SELECT id, url, dest_path, status, total_size FROM downloads "
                    "ORDER BY id DESC LIMIT ? OFFSET ?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)limit);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)offset);

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
    row.total_size = (uint64_t)sqlite3_column_int64(stmt, 4);

    if (visitor(&row, ctx) != 0)
      break;
    visited++;
  }
  sqlite3_finalize(stmt);
  return visited;
}

int db_find_active_by_url(const char *normalized, uint32_t *out_id) {
  if (!db_ready() || !normalized || !out_id)
    return -1;
  *out_id = 0;
  const char *sql =
      "SELECT id, url FROM downloads "
      "WHERE status IN ('QUEUED', 'ACTIVE', 'PAUSED') ORDER BY id DESC";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;

  int found = 0;
  int step;
  while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *url = (const char *)sqlite3_column_text(stmt, 1);
    char candidate[sizeof(((Download *)0)->url)];
    if (url && url_normalize(url, candidate, sizeof(candidate)) &&
        strcmp(candidate, normalized) == 0) {
      *out_id = (uint32_t)sqlite3_column_int(stmt, 0);
      found = 1;
      break;
    }
  }
  if (!found && step != SQLITE_DONE)
    found = -1;
  sqlite3_finalize(stmt);
  return found;
}

int db_get_download_details(uint32_t id, IpcDownloadDetails *out) {
  if (!db_ready() || !out)
    return -1;
  const char *sql = "SELECT cookie, referrer, extra_headers, expected_sha256, "
                    "speed_limit_bps, auth_user, auth_password != '' "
                    "FROM downloads WHERE id = ?";
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
    const char *auth_user = (const char *)sqlite3_column_text(stmt, 5);
    out->has_password = sqlite3_column_int(stmt, 6) != 0;
    strncpy(out->auth_user, auth_user ? auth_user : "",
            sizeof(out->auth_user) - 1);
    out->auth_user[sizeof(out->auth_user) - 1] = '\0';

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
                    "speed_limit_bps, reserved_file, auto_filename, etag, "
                    "last_modified, auth_user, auth_password "
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
    d->reserved_file = sqlite3_column_int(stmt, 11) != 0;
    atomic_store(&d->auto_filename, sqlite3_column_int(stmt, 12) != 0);
    const char *etag = (const char *)sqlite3_column_text(stmt, 13);
    const char *last_modified = (const char *)sqlite3_column_text(stmt, 14);
    const char *auth_user = (const char *)sqlite3_column_text(stmt, 15);
    const char *auth_password = (const char *)sqlite3_column_text(stmt, 16);

    strncpy(d->url, url ? url : "", sizeof(d->url) - 1);
    strncpy(d->dest_path, path ? path : "", sizeof(d->dest_path) - 1);
    strncpy(d->etag, etag ? etag : "", sizeof(d->etag) - 1);
    strncpy(d->last_modified, last_modified ? last_modified : "",
            sizeof(d->last_modified) - 1);
    RequestOptions options = {0};
    strncpy(options.cookie, cookie ? cookie : "", sizeof(options.cookie) - 1);
    strncpy(options.referrer, referrer ? referrer : "",
            sizeof(options.referrer) - 1);
    strncpy(options.extra_headers, headers ? headers : "",
            sizeof(options.extra_headers) - 1);
    strncpy(options.expected_sha256, sha256 ? sha256 : "",
            sizeof(options.expected_sha256) - 1);
    options.speed_limit_bps = speed_limit;
    strncpy(options.auth_user, auth_user ? auth_user : "",
            sizeof(options.auth_user) - 1);
    strncpy(options.auth_password, auth_password ? auth_password : "",
            sizeof(options.auth_password) - 1);

    if (options.cookie[0] != '\0' || options.referrer[0] != '\0' ||
        options.extra_headers[0] != '\0' ||
        options.expected_sha256[0] != '\0' || options.speed_limit_bps != 0 ||
        options.auth_user[0] != '\0' || options.auth_password[0] != '\0') {
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
    else if (strcmp(stat, "CANCELED") == 0)
      d->status = DOWNLOAD_CANCELED;
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
