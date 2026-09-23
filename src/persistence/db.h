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
bool db_destination_exists(const char *dest_path);
int db_update_status(uint32_t id, const char *status);
int db_update_total_size(uint32_t id, uint64_t total_size);

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
int db_visit_downloads(DbDownloadVisitor visitor, void *ctx, int max);
uint32_t db_get_max_id(void);

/** Single-row fetch of the heavy request-options fields for one download.
    Returns 0 on success (found), -1 if not found or on error. */
int db_get_download_details(uint32_t id, IpcDownloadDetails *out);

/** Restore interrupted downloads from the database into memory. */
int db_restore_queue(void);

#ifdef __cplusplus
}
#endif

#endif /* PERSISTENCE_DB_H */
