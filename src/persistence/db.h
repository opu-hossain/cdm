// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PERSISTENCE_DB_H
#define PERSISTENCE_DB_H

#include "../core/download_record.h"
#include "../core/queue_manager.h"
#include "../platform/ipc_socket.h" // for IpcDownloadDetails

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Row types for bulk queries */

typedef struct {
  uint64_t range_start;
  uint64_t range_end;
  uint64_t bytes_done;
} DbChunkRow;

/* Lightweight list row; request details are fetched on demand. */
typedef DownloadListRecord DbDownloadRow;

typedef int (*DbDownloadVisitor)(const DbDownloadRow *row, void *ctx);

/* Database lifecycle. */

int db_init(const char *db_path);
void db_close(void);

/* Download persistence. */

int db_insert_download(uint32_t id, const char *url, const char *dest_path,
                       const RequestOptions *opts);
int db_insert_reserved_download(uint32_t id, const char *url,
                                const char *dest_path,
                                const RequestOptions *opts);
int db_insert_reserved_download_auto(uint32_t id, const char *url,
                                     const char *dest_path,
                                     const RequestOptions *opts);
/* 0 = deleted, 1 = ACTIVE (refused), 2 = not found, -1 = database error.
 * File deletion happens after the database transaction commits. */
int db_delete_download(uint32_t id, int delete_file);
int db_update_resolved_destination(uint32_t id, const char *dest_path);
bool db_destination_exists(const char *dest_path);
int db_update_status(uint32_t id, const char *status);
int db_update_total_size(uint32_t id, uint64_t total_size);
int db_update_validators(uint32_t id, const char *etag,
                         const char *last_modified);

/* Chunk persistence for resume support. */

int db_insert_chunk(uint32_t download_id, uint64_t range_start,
                    uint64_t range_end);
int db_update_chunk_progress(uint32_t download_id, uint64_t range_start,
                             uint64_t bytes_done);
int db_update_chunk_range(uint32_t download_id, uint64_t range_start,
                          uint64_t new_range_end);
int db_delete_chunks(uint32_t download_id);

/* Bulk queries and queue restore. */

int db_load_chunks(uint32_t download_id, DbChunkRow *out, int max);
int db_list_all_downloads(DbDownloadRow *out, int max);
int db_count_downloads(int max);
int64_t db_count_downloads_total(void);
int db_visit_downloads(DbDownloadVisitor visitor, void *ctx, int max);
int db_visit_downloads_page(DbDownloadVisitor visitor, void *ctx,
                            uint32_t offset, uint32_t limit);
uint32_t db_get_max_id(void);

/* normalized is a url_normalize() result. Returns 1 if a queued, active or
 * paused row matches, 0 if absent, -1 on query error. */
int db_find_active_by_url(const char *normalized, uint32_t *out_id);

/** Single-row fetch of the heavy request-options fields for one download.
    Returns 0 on success (found), -1 if not found or on error. */
int db_get_download_details(uint32_t id, IpcDownloadDetails *out);

/** Restore interrupted downloads from the database into memory. */
int db_restore_queue(void);
int db_set_schedule_paused(uint32_t id, bool paused);
int db_resume_scheduled_download(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* PERSISTENCE_DB_H */
