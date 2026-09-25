// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef CORE_QUEUE_MANAGER_H
#define CORE_QUEUE_MANAGER_H

#include "download_record.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QM_MAX_CHUNKS 16 // keep in sync with segmenter.h's MAX_WORKERS

/* Request options (supplied by the user) */
typedef struct {
  char cookie[1024];
  char referrer[2048];
  char extra_headers[4096]; // raw "Key: Value" lines separated by '\n'
  char expected_sha256[65]; // 64 hex chars + NUL, empty = no check
  uint64_t speed_limit_bps; // 0 = use daemon default
  char auth_user[128];
  char auth_password[256];
} RequestOptions;

/* Chunk descriptor (one segment of a download) */
typedef struct {
  uint64_t range_start;
  uint64_t range_end;  // inclusive
  uint64_t bytes_done; // >= (range_end - range_start + 1) → complete
} DownloadChunk;

/* Download state enumeration */
typedef enum {
  DOWNLOAD_QUEUED,
  DOWNLOAD_ACTIVE,
  DOWNLOAD_PAUSED,
  DOWNLOAD_DONE,
  DOWNLOAD_ERROR,
  DOWNLOAD_CANCELED,
} DownloadStatus;

/* Full download entry */
typedef struct Download {
  uint32_t id;
  char url[2048];
  char dest_path[1024];
  char etag[256];
  char last_modified[128];
  uint64_t total_size;
  DownloadStatus status;
  int priority;
  float progress;
  struct Download *next;

  _Atomic bool cancel_requested;
  _Atomic bool pause_requested;
  _Atomic uint64_t bytes_downloaded;
  _Atomic uint64_t chunk_live_bytes[QM_MAX_CHUNKS];
  DownloadTransferMetrics transfer_metrics; // guarded by queue mutex

  DownloadChunk chunks[QM_MAX_CHUNKS];
  int chunk_count;
  bool reserved_file;
  _Atomic bool auto_filename; // daemon may replace URL-derived name after probe
  int retry_count;
  time_t next_retry_at;

  RequestOptions *request;
} Download;

/* Progress snapshots (for external reporting) */
typedef struct {
  uint32_t id;
  uint64_t bytes_downloaded;
  uint64_t total_size;
  DownloadTransferMetrics transfer_metrics;
} DownloadProgressSnapshot;

typedef struct {
  uint32_t download_id;
  uint64_t range_start;
  uint64_t bytes_done;
} ChunkProgressSnapshot;

typedef struct {
  DownloadStatus status;
  uint64_t total_size;
  uint64_t bytes_downloaded;
  char dest_path[1024];
  int chunk_count;
  DownloadChunk chunks[QM_MAX_CHUNKS];
  DownloadTransferMetrics transfer_metrics;
} DownloadRuntimeSnapshot;

/* Queue lifecycle. */

/** Add a new download to the queue. Returns its ID (0 on failure). */
uint32_t queue_manager_add(const char *url, const char *dest_path,
                           const RequestOptions *opts);
uint32_t queue_manager_add_auto(const char *url, const char *dest_path,
                                const RequestOptions *opts);

/** Insert an already-constructed Download (used on daemon restore). */
void queue_manager_add_existing(Download *d);

/** Make sure new IDs start at least from min_next_id. */
void queue_manager_seed_next_id(uint32_t min_next_id);

/** Remove a download from the queue (thread‑safe). */
void queue_manager_remove(uint32_t id);

/* Queue queries. */

/** Return the Download structure for the given ID, or NULL. */
Download *queue_manager_find_by_id(uint32_t id);

/** Return the highest‑priority QUEUED download, or NULL. */
Download *queue_manager_find_next_queued(void);

/** Count downloads with a specific status (locks internally). */
int queue_manager_count_by_status(DownloadStatus s);

/** Same as above, but caller must hold the queue mutex. */
int queue_manager_count_by_status_locked(DownloadStatus s);

/** Read a download status without exposing the queue entry pointer. */
bool queue_manager_get_status(uint32_t id, DownloadStatus *out_status);
bool queue_manager_get_runtime_snapshot(uint32_t id,
                                        DownloadRuntimeSnapshot *out);

/** Fill `out` with up to `max` active download progress snapshots. */
int queue_manager_snapshot_active_progress(DownloadProgressSnapshot *out,
                                           int max);

/* Persist a daemon progress sample under the queue mutex. */
void queue_manager_set_transfer_metrics(uint32_t id,
                                        DownloadTransferMetrics metrics);

/** Fill `out` with up to `max` chunk progress snapshots. */
int queue_manager_snapshot_chunk_progress(ChunkProgressSnapshot *out, int max);

/* Status and control. */

/** Change the status of a download (thread‑safe). */
void queue_manager_update_status(uint32_t id, DownloadStatus new_status);

/** Request cancellation of a download. Returns true if found. */
bool queue_manager_cancel(uint32_t id);

/** Discard in-memory resume data after a canceled file is removed. */
void queue_manager_clear_resume_state(uint32_t id);

/** Request pause. Returns true if found. */
bool queue_manager_pause(uint32_t id);

/** Request resume. Returns true if found. */
bool queue_manager_resume(uint32_t id);

/* Internal access. */

/** Return the global queue mutex (for external locking). */
void *queue_manager_get_mutex(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_QUEUE_MANAGER_H */
