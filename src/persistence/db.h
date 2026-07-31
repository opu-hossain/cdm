// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PERSISTENCE_DB_H
#define PERSISTENCE_DB_H

#include "../core/queue_manager.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Row types for bulk queries                                        */
/* ------------------------------------------------------------------ */

typedef struct {
  uint64_t range_start;
  uint64_t range_end;
  uint64_t bytes_done;
} DbChunkRow;

typedef struct {
  uint32_t id;
  char url[2048];
  char dest_path[1024];
  char status[16];
  uint64_t total_size;
} DbDownloadRow;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/** Open (or create) the database at `db_path` and create schema. */
int db_init(const char *db_path);
void db_close(void);

/* ------------------------------------------------------------------ */
/*  Download persistence                                              */
/* ------------------------------------------------------------------ */

int db_insert_download(uint32_t id, const char *url, const char *dest_path,
                       const RequestOptions *opts);
int db_update_status(uint32_t id, const char *status);
int db_update_total_size(uint32_t id, uint64_t total_size);

/* ------------------------------------------------------------------ */
/*  Chunk persistence (resume support)                                */
/* ------------------------------------------------------------------ */

int db_insert_chunk(uint32_t download_id, uint64_t range_start,
                    uint64_t range_end);
int db_update_chunk_progress(uint32_t download_id, uint64_t range_start,
                             uint64_t bytes_done);
int db_update_chunk_range(uint32_t download_id, uint64_t range_start,
                          uint64_t new_range_end);
int db_delete_chunks(uint32_t download_id);

/* ------------------------------------------------------------------ */
/*  Bulk queries / restore                                            */
/* ------------------------------------------------------------------ */

int db_load_chunks(uint32_t download_id, DbChunkRow *out, int max);
int db_list_all_downloads(DbDownloadRow *out, int max);
uint32_t db_get_max_id(void);

/** Restore interrupted downloads from the database into memory. */
int db_restore_queue(void);

#ifdef __cplusplus
}
#endif

#endif /* PERSISTENCE_DB_H */
