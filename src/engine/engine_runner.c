// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "engine_runner.h"

#include "../core/queue_manager.h"
#include "../persistence/db.h"
#include "../platform/curl_client.h"
#include "../platform/file_io.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "finalize.h"
#include "segmenter.h"
#include "worker_pool.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

/* Types & callback */

typedef struct {
  struct Download *d;
  _Atomic uint64_t **progress_slots;
} SplitCallbackCtx;

/**
 * Called by the rebalance pool when it decides to split a chunk.
 * Shrinks the victim chunk and appends a new chunk at the end of the
 * array, persisting both changes to the database.
 */
static void on_rebalance_split(void *userdata, uint64_t victim_start,
                               uint64_t new_start, uint64_t new_end) {
  SplitCallbackCtx *sctx = (SplitCallbackCtx *)userdata;
  struct Download *d = sctx->d;

  LOG_INFO("Download %u: rebalance split — chunk tail [%llu-%llu) handed off\n",
           d->id, (unsigned long long)new_start,
           (unsigned long long)(new_end - 1));

  /* shrink the victim */
  for (int j = 0; j < d->chunk_count; j++) {
    if (d->chunks[j].range_start == victim_start) {
      d->chunks[j].range_end = new_start - 1;
      db_update_chunk_range(d->id, victim_start, new_start - 1);
      break;
    }
  }

  if (d->chunk_count >= QM_MAX_CHUNKS)
    return;

  /* append new chunk */
  int idx = d->chunk_count;
  d->chunks[idx] = (DownloadChunk){new_start, new_end - 1, 0};
  d->chunk_count++;
  atomic_store(&d->chunk_live_bytes[idx], 0);
  if (sctx->progress_slots)
    sctx->progress_slots[idx] = &d->chunk_live_bytes[idx];
  db_insert_chunk(d->id, new_start, new_end - 1);
}

/* Helpers */

static bool is_valid_url(const char *url) {
  return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static bool chunk_is_complete(const DownloadChunk *c) {
  return c->bytes_done >= (c->range_end - c->range_start + 1);
}

/**
 * Discard all chunk state for a download.  If `also_delete_file` is true
 * the destination file is removed from disk.
 */
static void clear_resume_state(struct Download *d, bool also_delete_file) {
  memset(d->chunks, 0, sizeof(d->chunks));
  d->chunk_count = 0;
  d->total_size = 0;
  d->progress = 0.0f;
  atomic_store(&d->bytes_downloaded, 0);
  for (int i = 0; i < QM_MAX_CHUNKS; i++)
    atomic_store(&d->chunk_live_bytes[i], 0);
  db_delete_chunks(d->id);
  db_update_total_size(d->id, 0);
  if (also_delete_file && d->dest_path[0] != '\0')
    unlink(d->dest_path);
}

/* Public API */

int engine_run_download(struct Download *d) {
  const RequestOptions *request = d->request;

  if (d->chunk_count > 0 && access(d->dest_path, F_OK) != 0 &&
      errno == ENOENT) {
    LOG_ERROR("Resume file for download %u is missing: %s", d->id,
              d->dest_path);
    clear_resume_state(d, false);
    return -4;
  }

  LOG_INFO("Starting download: %s\n", d->url);

  if (request && request->expected_sha256[0])
    LOG_INFO("Download %u will verify SHA-256: %s\n", d->id,
             request->expected_sha256);

  if (!is_valid_url(d->url)) {
    LOG_ERROR("Invalid URL scheme (only http/https allowed): %s\n", d->url);
    return -1;
  }

  RequestContext req_ctx = {
      .cookie = request && request->cookie[0] ? request->cookie : NULL,
      .referrer = request && request->referrer[0] ? request->referrer : NULL,
      .extra_headers = request && request->extra_headers[0]
                 ? request->extra_headers
                 : NULL,
  };

  FileInfo info;
  if (curl_client_head(d->url, &req_ctx, &info) != 0) {
    LOG_ERROR("HEAD request failed for: %s\n", d->url);
    return -1;
  }

  LOG_INFO("Size: %llu bytes, Ranges: %s\n",
           (unsigned long long)info.total_size,
           info.supports_ranges ? "yes" : "no");

  /* --- Determine whether we are resuming and sanity‑check state --- */
  bool resuming = (d->chunk_count > 0);

  if (resuming) {
    uint64_t on_disk = file_get_size(d->dest_path);
    bool stale = (d->total_size != 0 && info.total_size != d->total_size);
    bool impossible = on_disk > info.total_size;
    if (stale || impossible) {
      LOG_ERROR("Resume data for download %u is stale (size mismatch), "
                "restarting from scratch\n",
                d->id);
      clear_resume_state(d, true);
      resuming = false;
    }
  }

  d->total_size = info.total_size;
  db_update_total_size(d->id, info.total_size);

  /* --- Build the list of ranges to fetch --- */
  Range ranges[MAX_WORKERS];
  int n_ranges = 0;
  int chunk_slot_for_range[MAX_WORKERS];

  if (resuming) {
    atomic_store(&d->bytes_downloaded, 0);
    for (int i = 0; i < d->chunk_count && n_ranges < MAX_WORKERS; i++) {
      DownloadChunk *c = &d->chunks[i];
      if (chunk_is_complete(c)) {
        atomic_fetch_add(&d->bytes_downloaded,
                         c->range_end - c->range_start + 1);
        atomic_store(&d->chunk_live_bytes[i], c->bytes_done);
        continue;
      }
      bool force_whole_file = (!info.supports_ranges && c->range_start == 0 &&
                               c->range_end == info.total_size - 1);

      ranges[n_ranges].start = c->range_start;
      ranges[n_ranges].end = c->range_end;
      ranges[n_ranges].whole_file = force_whole_file;
      ranges[n_ranges].unknown_size = false;
      ranges[n_ranges].resume_offset = force_whole_file ? 0 : c->bytes_done;

      if (!force_whole_file)
        atomic_fetch_add(&d->bytes_downloaded, c->bytes_done);
      atomic_store(&d->chunk_live_bytes[i], c->bytes_done);
      chunk_slot_for_range[n_ranges] = i;
      n_ranges++;
    }
    LOG_INFO("Resuming download %u: %d of %d range(s) remaining\n", d->id,
             n_ranges, d->chunk_count);

    if (n_ranges == 0) {
      if (engine_finalize(d->dest_path, info.total_size,
                          request ? request->expected_sha256 : NULL) != 0) {
        clear_resume_state(d, true);
        return -2;
      }
      LOG_INFO("Download complete: %s\n", d->dest_path);
      return 0;
    }
  } else {
    int n_workers = choose_worker_count(info.total_size);
    if (!info.supports_ranges)
      n_workers = 1;

    if (info.total_size > 0 && info.supports_ranges) {
      n_ranges = segmenter_plan(info.total_size, n_workers, ranges);
    } else {
      n_ranges = 1;
      ranges[0].start = 0;
      ranges[0].end = (info.total_size > 0) ? (info.total_size - 1) : 0;
      ranges[0].resume_offset = 0;
      ranges[0].whole_file = true;
      ranges[0].unknown_size = info.total_size == 0;
    }

    LOG_INFO("Using %d worker(s)\n", n_ranges);

    int prealloc_rc = d->reserved_file && access(d->dest_path, F_OK) == 0
                          ? file_preallocate_reserved(d->dest_path,
                                                     info.total_size)
                          : file_preallocate(d->dest_path, info.total_size);
    if (prealloc_rc == -2) {
      LOG_ERROR("Destination already exists, not retrying: %s\n", d->dest_path);
      return -3; // non-retryable, distinct from -2 (checksum/verification)
    }
    if (prealloc_rc != 0) {
      LOG_ERROR("Failed to create output file: %s\n", d->dest_path);
      return -1;
    }

    /* Persist the fresh plan so a later resume can pick it up. */
    db_delete_chunks(d->id);
    d->chunk_count = 0;
    for (int i = 0; i < n_ranges && i < QM_MAX_CHUNKS; i++) {
      db_insert_chunk(d->id, ranges[i].start, ranges[i].end);
      d->chunks[i] = (DownloadChunk){ranges[i].start, ranges[i].end, 0};
      d->chunk_count++;
      atomic_store(&d->chunk_live_bytes[i], 0);
      chunk_slot_for_range[i] = i;
    }

    atomic_store(&d->bytes_downloaded, 0);
  }

  /* --- Run the worker pool (optionally with rebalancing) --- */
  _Atomic uint64_t *chunk_progress_slots[MAX_WORKERS];
  for (int i = 0; i < n_ranges; i++)
    chunk_progress_slots[i] = &d->chunk_live_bytes[chunk_slot_for_range[i]];

  uint64_t speed_limit = request ? request->speed_limit_bps : 0;

  SplitCallbackCtx split_ctx = {.d = d, .progress_slots = chunk_progress_slots};
  RebalancePool *pool = NULL;
  if (n_ranges > 1) {
    pool = rebalance_pool_create(ranges, n_ranges, chunk_progress_slots,
                                 1024ULL * 1024ULL, /* 1 MB steal threshold */
                                 on_rebalance_split, &split_ctx);
  }

  WorkerPoolResult result = worker_pool_run(
      d->url, ranges, n_ranges, d->dest_path, &d->bytes_downloaded,
      &d->cancel_requested, &d->pause_requested, chunk_progress_slots,
      speed_limit, &req_ctx, pool);

  /* Flush per‑chunk progress to DB regardless of outcome. */
  for (int i = 0; i < d->chunk_count; i++) {
    uint64_t bytes = atomic_load(&d->chunk_live_bytes[i]);
    d->chunks[i].bytes_done = bytes;
    db_update_chunk_progress(d->id, d->chunks[i].range_start, bytes);
  }

  rebalance_pool_destroy(pool);
  pool = NULL;

  bool user_stopped =
      atomic_load(&d->cancel_requested) || atomic_load(&d->pause_requested);

  /* --- Fallback: single‑connection retry if parallel failed --- */
  if (!result.all_succeeded && n_ranges > 1 && !user_stopped) {
    LOG_ERROR(
        "Parallel download failed, retrying with a single connection...\n");
    atomic_store(&d->bytes_downloaded, 0);
    Range single_range = {
        .start = 0, .end = 0, .resume_offset = 0, .whole_file = true,
        .unknown_size = info.total_size == 0};
    result =
        worker_pool_run(d->url, &single_range, 1, d->dest_path,
                        &d->bytes_downloaded, &d->cancel_requested,
                        &d->pause_requested, NULL, speed_limit, &req_ctx, NULL);
  }

  if (!result.all_succeeded) {
    if (user_stopped) {
      LOG_WARN("Download %u %s by user\n", d->id,
               atomic_load(&d->cancel_requested) ? "canceled" : "paused");
    } else {
      LOG_ERROR("One or more workers failed — resume data retained "
                "for download %u\n",
                d->id);
    }
    return -1;
  }

  LOG_INFO("Downloaded %llu bytes\n",
           (unsigned long long)result.total_bytes_downloaded);

  if (engine_finalize(d->dest_path, info.total_size,
                      request ? request->expected_sha256 : NULL) != 0) {
    clear_resume_state(d, true);
    return -2;
  }

  LOG_INFO("Download complete: %s\n", d->dest_path);
  return 0;
}
