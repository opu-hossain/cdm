// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "db.h"
#include "../engine/hls.h"
#include "../engine/dash.h"

#include "../core/queue_manager.h"
#include "../platform/thread.h"
#include "../utils/log.h"
#include "../utils/url.h"
#include "sqlite3.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
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
      "CREATE TABLE IF NOT EXISTS categories ("
      "  id INTEGER PRIMARY KEY,"
      "  name TEXT NOT NULL UNIQUE,"
      "  extensions TEXT NOT NULL DEFAULT '',"
      "  default_dir TEXT NOT NULL DEFAULT '',"
      "  created_at INTEGER NOT NULL DEFAULT 0"
      ");"
      "CREATE TABLE IF NOT EXISTS queues ("
      "  id INTEGER PRIMARY KEY,"
      "  name TEXT NOT NULL UNIQUE,"
      "  priority INTEGER NOT NULL DEFAULT 0,"
      "  max_concurrent INTEGER NOT NULL DEFAULT 0,"
      "  schedule_start TEXT NOT NULL DEFAULT '',"
      "  schedule_stop TEXT NOT NULL DEFAULT '',"
      "  post_action TEXT NOT NULL DEFAULT 'none',"
      "  post_action_arg TEXT NOT NULL DEFAULT '',"
      "  post_action_pending_since INTEGER NOT NULL DEFAULT 0,"
      "  post_action_fired INTEGER NOT NULL DEFAULT 0,"
      "  created_at INTEGER NOT NULL DEFAULT 0"
      ");"
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
      "  auth_password   TEXT DEFAULT '',"
      "  schedule_paused INTEGER NOT NULL DEFAULT 0,"
      "  queue_id INTEGER DEFAULT 1 REFERENCES queues(id) ON DELETE SET NULL,"
      "  category_id INTEGER NOT NULL DEFAULT 1 REFERENCES categories(id),"
      "  requires_browser_context INTEGER NOT NULL DEFAULT 0,"
      "  media_kind INTEGER NOT NULL DEFAULT 0 CHECK(media_kind BETWEEN 0 AND 3),"
      "  companion_path TEXT NOT NULL DEFAULT '',"
      "  site_grab INTEGER NOT NULL DEFAULT 0 CHECK(site_grab IN (0,1)),"
      "  last_error TEXT NOT NULL DEFAULT ''"
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
                                          "auth_user", "auth_password",
                                          "queue_id", "schedule_paused",
                                          "category_id", "requires_browser_context", "media_kind", "companion_path", "site_grab", "last_error"};
  static const char *migration_types[] = {"TEXT DEFAULT ''", "TEXT DEFAULT ''",
                                          "TEXT DEFAULT ''", "TEXT DEFAULT ''",
                                          "INTEGER DEFAULT 0", "INTEGER DEFAULT 0",
                                          "TEXT DEFAULT ''", "TEXT DEFAULT ''",
                                          "INTEGER DEFAULT 0", "TEXT DEFAULT ''",
                                          "TEXT DEFAULT ''",
                                          "INTEGER DEFAULT 1 REFERENCES queues(id) ON DELETE SET NULL",
                                          "INTEGER NOT NULL DEFAULT 0",
                                          "INTEGER NOT NULL DEFAULT 1 REFERENCES categories(id)",
                                          "INTEGER NOT NULL DEFAULT 0",
                                          "INTEGER NOT NULL DEFAULT 0 CHECK(media_kind BETWEEN 0 AND 3)",
                                          "TEXT NOT NULL DEFAULT ''",
                                          "INTEGER NOT NULL DEFAULT 0 CHECK(site_grab IN (0,1))",
                                          "TEXT NOT NULL DEFAULT ''"};

  char *migration_error = NULL;
  /* SQLite requires a NULL default when adding REFERENCES with FK checks
   * enabled. Disable enforcement before BEGIN, then validate and restore it. */
  bool adding_reference = !db_column_exists("downloads", "queue_id") ||
                          !db_column_exists("downloads", "category_id");
  if (adding_reference &&
      sqlite3_exec(g_db, "PRAGMA foreign_keys=OFF;", NULL, NULL, NULL) !=
          SQLITE_OK) {
    db_close();
    return -1;
  }
  rc = sqlite3_exec(g_db, "BEGIN;", NULL, NULL, &migration_error);
  if (rc != SQLITE_OK) {
    LOG_ERROR("could not begin database migration: %s",
              migration_error ? migration_error : "unknown error");
    sqlite3_free(migration_error);
    db_close();
    return -1;
  }
  sqlite3_free(migration_error);

  rc = sqlite3_exec(g_db,
      "INSERT OR IGNORE INTO categories(id,name,extensions,default_dir,created_at) "
      "VALUES(1,'Default','','',strftime('%s','now'));",
      NULL, NULL, &migration_error);
  if (rc != SQLITE_OK)
    goto action_migration_failed;
  sqlite3_free(migration_error);

  rc = sqlite3_exec(g_db,
      "INSERT OR IGNORE INTO queues(id,name,priority,max_concurrent,"
      "schedule_start,schedule_stop,post_action,post_action_arg,created_at) "
      "VALUES(1,'Default',0,0,'','','none','',strftime('%s','now'));",
      NULL, NULL, &migration_error);
  if (rc != SQLITE_OK) {
    LOG_ERROR("could not seed default queue: %s",
              migration_error ? migration_error : "unknown error");
    sqlite3_free(migration_error);
    sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
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

  if (!db_column_exists("queues", "post_action_pending_since") &&
      sqlite3_exec(g_db,
          "ALTER TABLE queues ADD COLUMN post_action_pending_since "
          "INTEGER NOT NULL DEFAULT 0", NULL, NULL, &migration_error) !=
          SQLITE_OK)
    goto action_migration_failed;
  if (!db_column_exists("queues", "post_action_fired") &&
      sqlite3_exec(g_db,
          "ALTER TABLE queues ADD COLUMN post_action_fired "
          "INTEGER NOT NULL DEFAULT 0", NULL, NULL, &migration_error) !=
          SQLITE_OK)
    goto action_migration_failed;
  if (sqlite3_exec(g_db,
      "CREATE INDEX IF NOT EXISTS downloads_queue_status_idx "
      "ON downloads(COALESCE(queue_id,1),status);"
      "CREATE TRIGGER IF NOT EXISTS queue_action_insert "
      "AFTER INSERT ON downloads BEGIN "
      "UPDATE queues SET post_action_pending_since=0,post_action_fired=0 "
      "WHERE id=COALESCE(NEW.queue_id,1); END;"
      "CREATE TRIGGER IF NOT EXISTS queue_action_incomplete "
      "AFTER UPDATE OF status,queue_id ON downloads "
      "WHEN NEW.status<>'DONE' OR NEW.queue_id IS NOT OLD.queue_id BEGIN "
      "UPDATE queues SET post_action_pending_since=0,post_action_fired=0 "
      "WHERE id=COALESCE(NEW.queue_id,1); END;"
      "CREATE TRIGGER IF NOT EXISTS queue_action_changed "
      "AFTER UPDATE OF post_action,post_action_arg ON queues "
      "WHEN NEW.post_action IS NOT OLD.post_action OR "
      "NEW.post_action_arg IS NOT OLD.post_action_arg BEGIN "
      "UPDATE queues SET post_action_pending_since=0,post_action_fired=0 "
      "WHERE id=NEW.id; END;",
      NULL, NULL, &migration_error) != SQLITE_OK)
    goto action_migration_failed;

  sqlite3_stmt *fk_check = NULL;
  if (sqlite3_prepare_v2(g_db, "PRAGMA foreign_key_check", -1,
                         &fk_check, NULL) != SQLITE_OK ||
      sqlite3_step(fk_check) != SQLITE_DONE) {
    LOG_ERROR("database migration failed foreign key check");
    sqlite3_finalize(fk_check);
    sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
    db_close();
    return -1;
  }
  sqlite3_finalize(fk_check);

  rc = sqlite3_exec(g_db, "PRAGMA user_version = 13; COMMIT;", NULL, NULL,
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
  if (adding_reference &&
      sqlite3_exec(g_db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL) !=
          SQLITE_OK) {
    db_close();
    return -1;
  }

  LOG_INFO("opened %s", db_path);
  return 0;

action_migration_failed:
  LOG_ERROR("post-action migration failed: %s",
            migration_error ? migration_error : "unknown error");
  sqlite3_free(migration_error);
  sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
  db_close();
  return -1;
}

static bool category_valid(const Category *category) {
  if (!category || !memchr(category->name, 0, sizeof(category->name)) ||
      !category->name[0] ||
      !memchr(category->extensions, 0, sizeof(category->extensions)) ||
      !memchr(category->default_dir, 0, sizeof(category->default_dir)))
    return false;
  const char *extensions = category->extensions;
  size_t length = strlen(extensions);
  if (length && (extensions[0] == ',' || extensions[length - 1] == ','))
    return false;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)extensions[i];
    if (c == ',') {
      if (i && extensions[i - 1] == ',')
        return false;
    } else if (!islower(c) && !isdigit(c))
      return false;
  }
  return true;
}

static bool read_category_row(sqlite3_stmt *stmt, Category *out) {
  memset(out, 0, sizeof(*out));
  out->id = (uint32_t)sqlite3_column_int64(stmt, 0);
  char *fields[] = {out->name, out->extensions, out->default_dir};
  size_t sizes[] = {sizeof(out->name), sizeof(out->extensions),
                    sizeof(out->default_dir)};
  for (int i = 0; i < 3; ++i) {
    const char *value = (const char *)sqlite3_column_text(stmt, i + 1);
    int bytes = sqlite3_column_bytes(stmt, i + 1);
    if (!value || bytes < 0 || (size_t)bytes >= sizes[i])
      return false;
    memcpy(fields[i], value, (size_t)bytes);
  }
  out->created_at = sqlite3_column_int64(stmt, 4);
  return true;
}

int db_category_list(Category **out, size_t *count) {
  if (!db_ready() || !out || !count)
    return -1;
  *out = NULL;
  *count = 0;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM categories", -1,
                         &stmt, NULL) != SQLITE_OK)
    return -1;
  int step = sqlite3_step(stmt);
  int64_t total = step == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  if (total < 0 || (uint64_t)total > SIZE_MAX / sizeof(Category))
    return -1;
  Category *rows = calloc((size_t)total ? (size_t)total : 1,
                          sizeof(Category));
  if (!rows)
    return -1;
  if (sqlite3_prepare_v2(g_db,
      "SELECT id,name,extensions,default_dir,created_at FROM categories "
      "ORDER BY id", -1, &stmt, NULL) != SQLITE_OK) {
    free(rows);
    return -1;
  }
  size_t n = 0;
  while ((step = sqlite3_step(stmt)) == SQLITE_ROW && n < (size_t)total) {
    if (!read_category_row(stmt, &rows[n]))
      break;
    n++;
  }
  sqlite3_finalize(stmt);
  if (step != SQLITE_DONE) {
    free(rows);
    return -1;
  }
  *out = rows;
  *count = n;
  return 0;
}

int db_category_create(const Category *category, uint32_t *out_id) {
  if (!db_ready() || !category_valid(category) || !out_id)
    return -1;
  *out_id = 0;
  sqlite3_stmt *limit_stmt = NULL;
  if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM categories", -1,
                         &limit_stmt, NULL) != SQLITE_OK)
    return -1;
  int limit_step = sqlite3_step(limit_stmt);
  int64_t count = limit_step == SQLITE_ROW
                      ? sqlite3_column_int64(limit_stmt, 0) : INT64_MAX;
  sqlite3_finalize(limit_stmt);
  if (count >= IPC_CATEGORY_MAX)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db,
      "INSERT INTO categories(name,extensions,default_dir,created_at) "
      "VALUES(?,?,?,?)", -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, category->name, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, category->extensions, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, category->default_dir, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
  int step = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (step != SQLITE_DONE)
    return -1;
  *out_id = (uint32_t)sqlite3_last_insert_rowid(g_db);
  return *out_id ? 0 : -1;
}

int db_category_update(const Category *category) {
  if (!db_ready() || !category_valid(category) || category->id <= 1)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db,
      "UPDATE categories SET name=?,extensions=?,default_dir=? WHERE id=?",
      -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, category->name, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, category->extensions, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, category->default_dir, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)category->id);
  int step = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return step == SQLITE_DONE && changed == 1 ? 0 : -1;
}

int db_category_delete(uint32_t id) {
  if (!db_ready() || id <= 1)
    return -1;
  if (sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db,
      "UPDATE downloads SET category_id=1 WHERE category_id=?", -1,
      &stmt, NULL) != SQLITE_OK)
    goto category_delete_failed;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
  int step = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  stmt = NULL;
  if (step != SQLITE_DONE ||
      sqlite3_prepare_v2(g_db, "DELETE FROM categories WHERE id=?", -1,
                         &stmt, NULL) != SQLITE_OK)
    goto category_delete_failed;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
  step = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  stmt = NULL;
  if (step == SQLITE_DONE && changed == 1 &&
      sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
    return 0;
category_delete_failed:
  sqlite3_finalize(stmt);
  sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
  return -1;
}

int db_queue_post_action_due(uint32_t queue_id, int64_t now_seconds) {
  if (!db_ready() || !queue_id || now_seconds < 5)
    return -1;
  const char *eligible =
      "EXISTS(SELECT 1 FROM downloads d WHERE "
      "COALESCE(d.queue_id,1)=queues.id) AND "
      "NOT EXISTS(SELECT 1 FROM downloads d WHERE "
      "COALESCE(d.queue_id,1)=queues.id AND d.status<>'DONE')";
  char sql[1024];
  int written = snprintf(sql, sizeof(sql),
      "UPDATE queues SET post_action_pending_since="
      "CASE WHEN %s THEN CASE WHEN post_action_pending_since=0 "
      "THEN ? ELSE post_action_pending_since END ELSE 0 END,"
      "post_action_fired=CASE WHEN %s THEN post_action_fired ELSE 0 END "
      "WHERE id=?", eligible, eligible);
  if (written < 0 || (size_t)written >= sizeof(sql))
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int64(stmt, 1, now_seconds);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)queue_id);
  int step = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (step != SQLITE_DONE)
    return -1;

  written = snprintf(sql, sizeof(sql),
      "UPDATE queues SET post_action_fired=1 WHERE id=? AND "
      "post_action_fired=0 AND post_action_pending_since>0 AND "
      "post_action_pending_since<=? AND %s", eligible);
  if (written < 0 || (size_t)written >= sizeof(sql) ||
      sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)queue_id);
  sqlite3_bind_int64(stmt, 2, now_seconds - 5);
  step = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return step == SQLITE_DONE ? changed : -1;
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
  char *path = NULL, *companion = NULL;
  int media_kind = DOWNLOAD_MEDIA_NONE;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db,
                         "SELECT status, dest_path, media_kind, companion_path FROM downloads WHERE id = ?",
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
  media_kind = sqlite3_column_int(stmt, 2);
  if (status && strcmp(status, "ACTIVE") == 0) {
    result = 1;
    goto rollback;
  }
  if (!stored_path || !(path = strdup(stored_path)))
    goto rollback;
  const char *stored_companion = (const char *)sqlite3_column_text(stmt, 3);
  companion = strdup(stored_companion ? stored_companion : "");
  if (!companion) goto rollback;
  sqlite3_finalize(stmt);
  stmt = NULL;

  if (delete_rows_for_id("DELETE FROM chunks WHERE download_id = ?", id) != 0 ||
      delete_rows_for_id("DELETE FROM downloads WHERE id = ?", id) != 0)
    goto rollback;
  if (sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
    goto rollback;
  result = 0;
  if (media_kind == DOWNLOAD_MEDIA_HLS) hls_discard_state(path);
  if (media_kind == DOWNLOAD_MEDIA_DASH) dash_discard_state(path);
  if (delete_file && companion[0] && unlink(companion) != 0)
    LOG_WARN("could not delete companion audio for download %u", id);
  if (delete_file && unlink(path) != 0)
    LOG_WARN("could not delete file for download %u (%s): %s", id, path,
             strerror(errno));
  free(companion);
  free(path);
  return result;

rollback:
  if (stmt)
    sqlite3_finalize(stmt);
  sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
  free(companion);
  free(path);
  return result;
}

static void read_queue_row(sqlite3_stmt *stmt, Queue *out) {
  memset(out, 0, sizeof(*out));
  out->id = (uint32_t)sqlite3_column_int64(stmt, 0);
  const char *name = (const char *)sqlite3_column_text(stmt, 1);
  out->priority = sqlite3_column_int(stmt, 2);
  out->max_concurrent = sqlite3_column_int(stmt, 3);
  const char *start = (const char *)sqlite3_column_text(stmt, 4);
  const char *stop = (const char *)sqlite3_column_text(stmt, 5);
  const char *action = (const char *)sqlite3_column_text(stmt, 6);
  const char *arg = (const char *)sqlite3_column_text(stmt, 7);
  out->created_at = sqlite3_column_int64(stmt, 8);
  snprintf(out->name, sizeof(out->name), "%s", name ? name : "");
  snprintf(out->schedule_start, sizeof(out->schedule_start), "%s",
           start ? start : "");
  snprintf(out->schedule_stop, sizeof(out->schedule_stop), "%s",
           stop ? stop : "");
  snprintf(out->post_action, sizeof(out->post_action), "%s",
           action ? action : "");
  snprintf(out->post_action_arg, sizeof(out->post_action_arg), "%s",
           arg ? arg : "");
}

int queue_list(Queue **out, size_t *count) {
  if (!db_ready() || !out || !count)
    return -1;
  *out = NULL;
  *count = 0;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM queues", -1,
                         &stmt, NULL) != SQLITE_OK)
    return -1;
  int step = sqlite3_step(stmt);
  int64_t total = step == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  if (total < 0 || (uint64_t)total > SIZE_MAX / sizeof(Queue))
    return -1;
  Queue *rows = calloc((size_t)total ? (size_t)total : 1, sizeof(Queue));
  if (!rows)
    return -1;
  const char *sql = "SELECT id,name,priority,max_concurrent,schedule_start,"
                    "schedule_stop,post_action,post_action_arg,created_at "
                    "FROM queues ORDER BY priority DESC,created_at ASC,id ASC";
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    free(rows);
    return -1;
  }
  size_t n = 0;
  while ((step = sqlite3_step(stmt)) == SQLITE_ROW && n < (size_t)total)
    read_queue_row(stmt, &rows[n++]);
  sqlite3_finalize(stmt);
  if (step != SQLITE_DONE) {
    free(rows);
    return -1;
  }
  *out = rows;
  *count = n;
  return 0;
}

int queue_get(uint32_t id, Queue *out) {
  if (!db_ready() || !id || !out)
    return -1;
  const char *sql = "SELECT id,name,priority,max_concurrent,schedule_start,"
                    "schedule_stop,post_action,post_action_arg,created_at "
                    "FROM queues WHERE id=?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
  int step = sqlite3_step(stmt);
  if (step == SQLITE_ROW)
    read_queue_row(stmt, out);
  sqlite3_finalize(stmt);
  return step == SQLITE_ROW ? 0 : -1;
}

static int schedule_minutes(const char *value) {
  if (strlen(value) != 5 || value[2] != ':' || value[0] < '0' ||
      value[0] > '9' || value[1] < '0' || value[1] > '9' ||
      value[3] < '0' || value[3] > '9' || value[4] < '0' ||
      value[4] > '9')
    return -1;
  int hour = (value[0] - '0') * 10 + (value[1] - '0');
  int minute = (value[3] - '0') * 10 + (value[4] - '0');
  return hour < 24 && minute < 60 ? hour * 60 + minute : -1;
}

static bool queue_fields_valid(const Queue *q) {
  if (!q || !memchr(q->schedule_start, 0, sizeof(q->schedule_start)) ||
      !memchr(q->schedule_stop, 0, sizeof(q->schedule_stop)) ||
      !memchr(q->post_action, 0, sizeof(q->post_action)) ||
      !memchr(q->post_action_arg, 0, sizeof(q->post_action_arg)))
    return false;
  bool always = !q->schedule_start[0] && !q->schedule_stop[0];
  int start = always ? 0 : schedule_minutes(q->schedule_start);
  int stop = always ? 0 : schedule_minutes(q->schedule_stop);
  const char *action = q->post_action[0] ? q->post_action : "none";
  bool action_valid = strcmp(action, "none") == 0 ||
                      strcmp(action, "shutdown") == 0 ||
                      strcmp(action, "sleep") == 0 ||
                      (strcmp(action, "command") == 0 &&
                       q->post_action_arg[0]);
  return q && memchr(q->name, '\0', sizeof(q->name)) && q->name[0] &&
         (always || (start >= 0 && stop >= 0 && start != stop)) &&
         memchr(q->post_action, '\0', sizeof(q->post_action)) &&
         memchr(q->post_action_arg, '\0', sizeof(q->post_action_arg)) &&
         action_valid &&
         q->max_concurrent >= 0 && q->max_concurrent <= 64;
}

static void bind_queue_fields(sqlite3_stmt *stmt, const Queue *q) {
  sqlite3_bind_text(stmt, 1, q->name, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, q->priority);
  sqlite3_bind_int(stmt, 3, q->max_concurrent);
  sqlite3_bind_text(stmt, 4, q->schedule_start, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 5, q->schedule_stop, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 6, q->post_action[0] ? q->post_action : "none", -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 7, q->post_action_arg, -1, SQLITE_STATIC);
}

int queue_create(const Queue *q, uint32_t *out_id) {
  if (!db_ready() || !queue_fields_valid(q) || !out_id)
    return -1;
  *out_id = 0;
  const char *sql = "INSERT INTO queues(name,priority,max_concurrent,"
                    "schedule_start,schedule_stop,post_action,"
                    "post_action_arg,created_at) VALUES(?,?,?,?,?,?,?,?)";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  bind_queue_fields(stmt, q);
  sqlite3_bind_int64(stmt, 8, (sqlite3_int64)time(NULL));
  int step = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (step != SQLITE_DONE)
    return -1;
  *out_id = (uint32_t)sqlite3_last_insert_rowid(g_db);
  return *out_id ? 0 : -1;
}

int queue_update(const Queue *q) {
  if (!db_ready() || !queue_fields_valid(q) || !q->id)
    return -1;
  const char *sql = "UPDATE queues SET name=?,priority=?,max_concurrent=?,"
                    "schedule_start=?,schedule_stop=?,post_action=?,"
                    "post_action_arg=? WHERE id=?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  bind_queue_fields(stmt, q);
  sqlite3_bind_int64(stmt, 8, (sqlite3_int64)q->id);
  int step = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return step == SQLITE_DONE && changed == 1 ? 0 : -1;
}

int queue_delete(uint32_t id) {
  if (!db_ready() || id <= 1)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, "DELETE FROM queues WHERE id=?", -1,
                         &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
  int step = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return step == SQLITE_DONE && changed == 1 ? 0 : -1;
}

int queue_reorder(uint32_t id, int new_priority) {
  if (!db_ready() || !id)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, "UPDATE queues SET priority=? WHERE id=?",
                         -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, new_priority);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)id);
  int step = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return step == SQLITE_DONE && changed == 1 ? 0 : -1;
}

int db_set_schedule_paused(uint32_t id, bool paused) {
  if (!db_ready() || !id)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db,
          "UPDATE downloads SET schedule_paused=? WHERE id=?", -1,
          &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, paused ? 1 : 0);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)id);
  int step = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return step == SQLITE_DONE && changed == 1 ? 0 : -1;
}

int db_resume_scheduled_download(uint32_t id) {
  if (!db_ready() || !id)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db,
          "UPDATE downloads SET schedule_paused=0,status='QUEUED' "
          "WHERE id=? AND schedule_paused=1 AND status='PAUSED'", -1,
          &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
  int step = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return step == SQLITE_DONE && changed == 1 ? 0 : -1;
}

/* Download persistence */

static int insert_download(uint32_t id, const char *url,
                           const char *dest_path, const RequestOptions *opts,
                           bool reserved, bool auto_filename) {
  if (!db_ready() || !url || !dest_path)
    return -1;

  Category category = {0};
  const char *filename = strrchr(dest_path, '/');
  if (!category_for_filename(filename ? filename + 1 : dest_path, &category))
    return -1;

  const char *sql =
      "INSERT OR REPLACE INTO downloads "
      "(id, url, dest_path, status, created_at, cookie, referrer, "
      "extra_headers, expected_sha256, speed_limit_bps, reserved_file, "
      "auto_filename, auth_user, auth_password, queue_id, category_id, "
      "requires_browser_context, media_kind, site_grab) "
      "VALUES (?, ?, ?, 'QUEUED', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }
  sqlite3_bind_int(stmt, 1, (int)id);
  sqlite3_bind_text(stmt, 2, url, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, dest_path, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
  bool ephemeral = opts && opts->browser_context;
  sqlite3_bind_text(stmt, 5, opts && !ephemeral ? opts->cookie : "", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 6, opts && !ephemeral ? opts->referrer : "", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 7, opts && !ephemeral ? opts->extra_headers : "", -1,
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
  sqlite3_bind_int64(stmt, 14,
                     opts && opts->queue_id ? (sqlite3_int64)opts->queue_id : 1);
  sqlite3_bind_int64(stmt, 15, (sqlite3_int64)category.id);
  sqlite3_bind_int(stmt, 16, ephemeral ? 1 : 0);
  sqlite3_bind_int(stmt, 17, opts ? (int)opts->media_kind : 0);
  sqlite3_bind_int(stmt, 18, opts && opts->site_grab ? 1 : 0);

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

int db_complete_media_outputs(uint32_t id, const char *path, const char *companion, uint64_t size) {
  if (!db_ready() || !path || !companion || strlen(path) >= 1024 || strlen(companion) >= 1024 || size > INT64_MAX) return -1;
  /* Publication is not completion: the scheduler marks DONE after scanning. */
  const char *sql = "UPDATE downloads SET dest_path=?, companion_path=?, total_size=?, auto_filename=0 WHERE id=?";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, companion, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)size);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)id);
  int result = sqlite3_step(stmt), changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return result == SQLITE_DONE && changed == 1 ? 0 : -1;
}
int db_get_companion_path(uint32_t id, char *out, size_t out_size) {
  if (!db_ready() || !out || out_size == 0)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, "SELECT companion_path FROM downloads WHERE id=?",
                         -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
  int rc = sqlite3_step(stmt);
  const unsigned char *value = rc == SQLITE_ROW ? sqlite3_column_text(stmt, 0) : NULL;
  size_t len = value ? strlen((const char *)value) : 0;
  if (rc != SQLITE_ROW || len >= out_size) {
    sqlite3_finalize(stmt);
    return -1;
  }
  memcpy(out, value, len + 1);
  sqlite3_finalize(stmt);
  return 0;
}
int db_update_output_paths(uint32_t id, const char *path, const char *companion) {
  if (!db_ready() || !path || !companion || strlen(path) >= 1024 ||
      strlen(companion) >= 1024)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db,
      "UPDATE downloads SET dest_path=?, companion_path=? WHERE id=?",
      -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, companion, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)id);
  int rc = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE && changed == 1 ? 0 : -1;
}
int db_update_site_error(uint32_t id, const char *redacted_error) {
 if (!db_ready() || !redacted_error || strlen(redacted_error)>=256) return -1;
 sqlite3_stmt *stmt=NULL;
 if(sqlite3_prepare_v2(g_db,"UPDATE downloads SET last_error=? WHERE id=?",-1,&stmt,NULL)!=SQLITE_OK)return -1;
 sqlite3_bind_text(stmt,1,redacted_error,-1,SQLITE_STATIC);
 sqlite3_bind_int64(stmt,2,(sqlite3_int64)id);
 int rc=sqlite3_step(stmt),changed=sqlite3_changes(g_db);sqlite3_finalize(stmt);
 return rc==SQLITE_DONE&&changed==1?0:-1;
}
int db_update_media_output(uint32_t id, const char *path, uint64_t size) {
  return db_complete_media_outputs(id, path, "", size);
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

int db_refresh_download(uint32_t id, const char *url, uint64_t total_size,
                         const char *etag, const char *last_modified) {
  if (!db_ready() || !url || !etag || !last_modified || total_size > INT64_MAX)
    return -1;
  const char *sql = "UPDATE downloads SET url=?, total_size=?, etag=?, "
                    "last_modified=? WHERE id=? AND status<>'ACTIVE'";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)total_size);
  sqlite3_bind_text(stmt, 3, etag, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, last_modified, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 5, id);
  int rc = sqlite3_step(stmt);
  int changed = sqlite3_changes(g_db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE && changed == 1 ? 0 : -1;
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
  const char *sql = "SELECT id, url, dest_path, status, total_size, category_id FROM downloads "
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
    out[n].category_id = (uint32_t)sqlite3_column_int64(stmt, 5);
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

  const char *sql = "SELECT id, url, dest_path, status, total_size, category_id, "
                    "media_kind, site_grab, requires_browser_context FROM downloads "
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
    row.category_id = (uint32_t)sqlite3_column_int64(stmt, 5);
    row.media_kind = (uint32_t)sqlite3_column_int(stmt, 6);
    row.site_grab = (uint32_t)sqlite3_column_int(stmt, 7);
    row.requires_browser_context = (uint32_t)sqlite3_column_int(stmt, 8);

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
      "WHERE status IN ('QUEUED', 'ACTIVE', 'PAUSED') OR "
      "(status='ERROR' AND requires_browser_context=1) ORDER BY id DESC";
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
                    "last_modified, auth_user, auth_password, "
                    "COALESCE(queue_id,1), created_at, schedule_paused, "
                    "requires_browser_context, media_kind, site_grab, last_error "
                    "FROM downloads WHERE status != 'DONE'";

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    LOG_ERROR("prepare failed: %s", sqlite3_errmsg(g_db));
    return -1;
  }

  int restored = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (queue_manager_find_by_id((uint32_t)sqlite3_column_int(stmt, 0)))
      continue;
    Download *d = calloc(1, sizeof(Download));
    if (!d)
      continue;

    d->id = (uint32_t)sqlite3_column_int(stmt, 0);
    d->total_size = (uint64_t)sqlite3_column_int64(stmt, 3);
    d->priority = sqlite3_column_int(stmt, 5);
    d->queue_id = (uint32_t)sqlite3_column_int(stmt, 17);
    d->created_at = (time_t)sqlite3_column_int64(stmt, 18);
    d->schedule_paused = sqlite3_column_int(stmt, 19) != 0;
    d->requires_browser_context = sqlite3_column_int(stmt, 20) != 0;
    d->media_kind = (DownloadMediaKind)sqlite3_column_int(stmt, 21);
    d->site_grab = sqlite3_column_int(stmt, 22) != 0;
    const char *stored_error = (const char *)sqlite3_column_text(stmt, 23);
    if (stored_error) strncpy(d->last_error, stored_error, sizeof(d->last_error)-1);

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

    if (!d->requires_browser_context &&
        (options.cookie[0] != '\0' || options.referrer[0] != '\0' ||
        options.extra_headers[0] != '\0' ||
        options.expected_sha256[0] != '\0' || options.speed_limit_bps != 0 ||
        options.auth_user[0] != '\0' || options.auth_password[0] != '\0')) {
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

    if (d->requires_browser_context && d->status != DOWNLOAD_CANCELED) {
      d->status = DOWNLOAD_PAUSED;
      db_update_status(d->id, "PAUSED");
    }

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

static int create_import_backup(void) {
  const char *source = sqlite3_db_filename(g_db, "main");
  if (!source || !source[0])
    return -1;
  char path[1200];
  int fd = -1;
  for (unsigned attempt = 0; attempt < 100; attempt++) {
    int n = snprintf(path, sizeof(path), "%s.backup.%lld.%u", source,
                     (long long)time(NULL), attempt);
    if (n < 0 || (size_t)n >= sizeof(path))
      return -1;
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC,
              0600);
    if (fd >= 0)
      break;
    if (errno != EEXIST)
      return -1;
  }
  if (fd < 0)
    return -1;
  close(fd);
  sqlite3 *destination = NULL;
  int rc = sqlite3_open_v2(path, &destination,
                           SQLITE_OPEN_READWRITE, NULL);
  sqlite3_backup *backup = rc == SQLITE_OK
                               ? sqlite3_backup_init(destination, "main", g_db,
                                                     "main")
                               : NULL;
  int copied = backup ? sqlite3_backup_step(backup, -1) : SQLITE_ERROR;
  int finished = backup ? sqlite3_backup_finish(backup) : SQLITE_ERROR;
  if (destination)
    sqlite3_close(destination);
  if (copied == SQLITE_DONE && finished == SQLITE_OK)
    return 0;
  unlink(path);
  return -1;
}

int db_import_history(const DbImportRow *rows, size_t count, bool replace) {
  if (!db_ready() || (!rows && count))
    return -1;
  dm_mutex_t *mutex = queue_manager_get_mutex();
  dm_mutex_lock(mutex);
  if (queue_manager_count_by_status_locked(DOWNLOAD_ACTIVE) > 0) {
    dm_mutex_unlock(mutex);
    return -2;
  }
  sqlite3_stmt *old = NULL;
  uint32_t *old_ids = NULL;
  size_t old_count = 0, old_capacity = 0;
  int result = -1;
  if (replace && sqlite3_prepare_v2(g_db, "SELECT id FROM downloads WHERE status!='DONE'",
                                    -1, &old, NULL) != SQLITE_OK)
    goto finish;
  int step = SQLITE_DONE;
  while (replace && (step = sqlite3_step(old)) == SQLITE_ROW) {
    if (old_count == old_capacity) {
      size_t next = old_capacity ? old_capacity * 2 : 64;
      if (next < old_capacity || next > SIZE_MAX / sizeof(*old_ids))
        goto finish;
      uint32_t *grown = realloc(old_ids, next * sizeof(*old_ids));
      if (!grown)
        goto finish;
      old_ids = grown;
      old_capacity = next;
    }
    old_ids[old_count++] = (uint32_t)sqlite3_column_int64(old, 0);
  }
  if (replace && step != SQLITE_DONE)
    goto finish;
  sqlite3_finalize(old);
  old = NULL;
  if (replace && create_import_backup() != 0)
    goto finish;
  if (sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
    goto finish;
  if (replace &&
      sqlite3_exec(g_db, "DELETE FROM downloads", NULL, NULL, NULL) !=
          SQLITE_OK)
    goto rollback;
  sqlite3_stmt *insert = NULL;
  const char *sql =
      "INSERT OR IGNORE INTO downloads"
      "(id,url,dest_path,status,total_size,created_at,cookie,referrer,"
      "extra_headers,auth_user,media_kind,site_grab,requires_browser_context) "
      "VALUES(?,?,?,?,?,strftime('%s','now'),?,?,?,?,?,?,?)";
  if (sqlite3_prepare_v2(g_db, sql, -1, &insert, NULL) != SQLITE_OK)
    goto rollback;
  for (size_t i = 0; i < count; i++) {
    sqlite3_bind_int64(insert, 1, (sqlite3_int64)rows[i].id);
    sqlite3_bind_text(insert, 2, rows[i].url, -1, SQLITE_STATIC);
    sqlite3_bind_text(insert, 3, rows[i].dest_path, -1, SQLITE_STATIC);
    sqlite3_bind_text(insert, 4, rows[i].status, -1, SQLITE_STATIC);
    sqlite3_bind_int64(insert, 5, (sqlite3_int64)rows[i].total_size);
    sqlite3_bind_text(insert, 6, rows[i].cookie ? rows[i].cookie : "", -1,
                      SQLITE_STATIC);
    sqlite3_bind_text(insert, 7, rows[i].referrer ? rows[i].referrer : "", -1,
                      SQLITE_STATIC);
    sqlite3_bind_text(insert, 8,
                      rows[i].extra_headers ? rows[i].extra_headers : "", -1,
                      SQLITE_STATIC);
    sqlite3_bind_text(insert, 9, rows[i].auth_user ? rows[i].auth_user : "", -1,
                      SQLITE_STATIC);
    sqlite3_bind_int(insert, 10, (int)rows[i].media_kind);
    sqlite3_bind_int(insert, 11, rows[i].site_grab ? 1 : 0);
    sqlite3_bind_int(insert, 12, rows[i].requires_browser_context ? 1 : 0);
    if (sqlite3_step(insert) != SQLITE_DONE) {
      sqlite3_finalize(insert);
      goto rollback;
    }
    sqlite3_reset(insert);
    sqlite3_clear_bindings(insert);
  }
  sqlite3_finalize(insert);
  if (sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
    goto rollback;
  if (replace)
    for (size_t i = 0; i < old_count; i++)
      queue_manager_forget_locked(old_ids[i]);
  result = 0;
  goto finish;
rollback:
  sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
finish:
  sqlite3_finalize(old);
  free(old_ids);
  dm_mutex_unlock(mutex);
  if (result == 0) {
    db_restore_queue();
    queue_manager_seed_next_id(db_get_max_id() + 1);
  }
  return result;
}
