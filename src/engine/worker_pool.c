// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "worker_pool.h"

#include "../platform/bandwidth.h"
#include "../platform/file_io.h"
#include "../platform/thread.h"

#include <curl/curl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  RebalancePool – internal slot type                                */
/* ------------------------------------------------------------------ */

typedef struct {
  uint64_t start;
  _Atomic uint64_t write_pos;      // next byte to write
  _Atomic uint64_t live_end;       // exclusive upper bound (may shrink)
  _Atomic bool claimed;            // currently assigned to a worker
  _Atomic bool exhausted;          // write_pos == live_end
  _Atomic uint64_t *progress_slot; // shared with the parent chunk
} PoolSlot;

struct RebalancePool {
  dm_mutex_t mutex;
  PoolSlot slots[MAX_WORKERS];
  int slot_count;
  uint64_t min_steal_bytes;
  RebalanceSplitFn on_split;
  void *userdata;
};

/* ------------------------------------------------------------------ */
/*  RebalancePool – public API                                        */
/* ------------------------------------------------------------------ */

RebalancePool *rebalance_pool_create(const Range *ranges, int n_ranges,
                                     _Atomic uint64_t **progress_slots,
                                     uint64_t min_steal_bytes,
                                     RebalanceSplitFn on_split,
                                     void *userdata) {
  RebalancePool *pool = calloc(1, sizeof(RebalancePool));
  if (!pool)
    return NULL;

  dm_mutex_init(&pool->mutex);
  pool->min_steal_bytes = min_steal_bytes;
  pool->on_split = on_split;
  pool->userdata = userdata;
  pool->slot_count = (n_ranges > MAX_WORKERS) ? MAX_WORKERS : n_ranges;

  for (int i = 0; i < pool->slot_count; i++) {
    pool->slots[i].start = ranges[i].start + ranges[i].resume_offset;
    atomic_init(&pool->slots[i].write_pos, pool->slots[i].start);
    atomic_init(&pool->slots[i].live_end, ranges[i].end + 1);
    atomic_init(&pool->slots[i].claimed, true);
    atomic_init(&pool->slots[i].exhausted, false);
    pool->slots[i].progress_slot = progress_slots ? progress_slots[i] : NULL;
  }
  return pool;
}

void rebalance_pool_destroy(RebalancePool *pool) {
  if (!pool)
    return;
  dm_mutex_destroy(&pool->mutex);
  free(pool);
}

/* ------------------------------------------------------------------ */
/*  RebalancePool – internal helpers                                  */
/* ------------------------------------------------------------------ */

/**
 * Attempt to acquire a pool slot for an idle worker.
 *
 * First checks for unclaimed non‑exhausted slots, then finds the busiest
 * slot and steals its tail if enough bytes remain.
 *
 * @return slot index (≥ 0) and fills *out_range, *out_progress_slot,
 *         or -1 if nothing worth stealing.
 */
static int rebalance_pool_acquire(RebalancePool *pool, Range *out_range,
                                  _Atomic uint64_t **out_progress_slot) {
  if (!pool)
    return -1;

  dm_mutex_lock(&pool->mutex);

  /* 1. Pick up an orphaned slot (should be rare). */
  for (int i = 0; i < pool->slot_count; i++) {
    if (!atomic_load(&pool->slots[i].exhausted) &&
        !atomic_load(&pool->slots[i].claimed)) {
      atomic_store(&pool->slots[i].claimed, true);
      out_range->start = atomic_load(&pool->slots[i].write_pos);
      out_range->end = atomic_load(&pool->slots[i].live_end) - 1;
      out_range->resume_offset = 0;
      out_range->whole_file = false;
      *out_progress_slot = pool->slots[i].progress_slot;
      dm_mutex_unlock(&pool->mutex);
      return i;
    }
  }

  /* 2. Find the busiest claimed, non‑exhausted slot. */
  int victim = -1;
  uint64_t victim_remaining = 0;
  for (int i = 0; i < pool->slot_count; i++) {
    if (atomic_load(&pool->slots[i].exhausted))
      continue;
    if (!atomic_load(&pool->slots[i].claimed))
      continue;
    uint64_t wp = atomic_load(&pool->slots[i].write_pos);
    uint64_t le = atomic_load(&pool->slots[i].live_end);
    uint64_t remaining = (le > wp) ? (le - wp) : 0;
    if (remaining > victim_remaining) {
      victim_remaining = remaining;
      victim = i;
    }
  }

  if (victim == -1 || victim_remaining < pool->min_steal_bytes ||
      pool->slot_count >= MAX_WORKERS) {
    dm_mutex_unlock(&pool->mutex);
    return -1;
  }

  /* 3. Split the victim at its midpoint. */
  uint64_t wp = atomic_load(&pool->slots[victim].write_pos);
  uint64_t le = atomic_load(&pool->slots[victim].live_end);
  uint64_t split = wp + (le - wp) / 2;

  atomic_store(&pool->slots[victim].live_end, split);

  int new_idx = pool->slot_count++;
  pool->slots[new_idx].start = split;
  atomic_init(&pool->slots[new_idx].write_pos, split);
  atomic_init(&pool->slots[new_idx].live_end, le);
  atomic_init(&pool->slots[new_idx].claimed, true);
  atomic_init(&pool->slots[new_idx].exhausted, false);
  pool->slots[new_idx].progress_slot = pool->slots[victim].progress_slot;

  if (pool->on_split)
    pool->on_split(pool->userdata, pool->slots[victim].start, split, le);

  out_range->start = split;
  out_range->end = le - 1;
  out_range->resume_offset = 0;
  out_range->whole_file = false;
  *out_progress_slot = pool->slots[new_idx].progress_slot;

  dm_mutex_unlock(&pool->mutex);
  return new_idx;
}

/**
 * Mark a slot as exhausted and release its claim.
 */
static void rebalance_pool_mark_done(RebalancePool *pool, int slot_index) {
  if (!pool)
    return;
  dm_mutex_lock(&pool->mutex);
  atomic_store(&pool->slots[slot_index].exhausted, true);
  atomic_store(&pool->slots[slot_index].claimed, false);
  dm_mutex_unlock(&pool->mutex);
}

/* ------------------------------------------------------------------ */
/*  Worker context & callbacks                                        */
/* ------------------------------------------------------------------ */

typedef struct {
  const char *url;
  Range range; // current segment assignment
  FileHandle fd;
  _Atomic uint64_t bytes_done; // bytes written for this segment
  bool succeeded;
  _Atomic uint64_t *download_bytes_done;
  _Atomic bool *cancel_flag;
  _Atomic bool *pause_flag;
  _Atomic uint64_t *chunk_progress_slot;
  int total_workers;
  uint64_t speed_limit_bps;
  const RequestContext *request_ctx;
  RebalancePool *pool;
  int slot_index;
  bool truncated; // stopped early due to rebalance shrink
} WorkerContext;

/**
 * libcurl write callback.
 *
 * Writes received data to the correct file offset, respecting speed
 * limits and rebalancing boundaries.
 */
static size_t worker_write_callback(void *data, size_t size, size_t nmemb,
                                    void *userdata) {
  WorkerContext *ctx = (WorkerContext *)userdata;
  size_t total = size * nmemb;

  if ((ctx->cancel_flag && atomic_load(ctx->cancel_flag)) ||
      (ctx->pause_flag && atomic_load(ctx->pause_flag))) {
    return 0;
  }

  if (!bandwidth_acquire((uint64_t)total, ctx->cancel_flag, ctx->pause_flag))
    return 0;

  uint64_t current_offset = ctx->range.start + ctx->range.resume_offset +
                            atomic_load(&ctx->bytes_done);

  size_t writable = total;
  if (ctx->pool) {
    uint64_t live_end =
        atomic_load(&ctx->pool->slots[ctx->slot_index].live_end);
    if (current_offset >= live_end) {
      ctx->truncated = true;
      return 0;
    }
    uint64_t allowed = live_end - current_offset;
    if ((uint64_t)writable > allowed) {
      writable = (size_t)allowed;
      ctx->truncated = true;
    }
  }

  if (file_pwrite(ctx->fd, data, writable, current_offset) != 0)
    return 0;

  atomic_fetch_add(&ctx->bytes_done, writable);
  if (ctx->pool)
    atomic_fetch_add(&ctx->pool->slots[ctx->slot_index].write_pos, writable);
  if (ctx->download_bytes_done)
    atomic_fetch_add(ctx->download_bytes_done, writable);
  if (ctx->chunk_progress_slot)
    atomic_fetch_add(ctx->chunk_progress_slot, writable);

  return writable; // < total signals curl to stop
}

/**
 * Execute a single HTTP request for the current segment.
 */
static void run_one_segment(WorkerContext *ctx) {
  ctx->succeeded = false;
  ctx->truncated = false;
  atomic_store(&ctx->bytes_done, 0);

  CURL *curl = curl_easy_init();
  if (!curl)
    return;

  curl_easy_setopt(curl, CURLOPT_URL, ctx->url);
  if (!ctx->range.whole_file) {
    char range_header[128];
    snprintf(range_header, sizeof(range_header), "%llu-%llu",
             (unsigned long long)(ctx->range.start + ctx->range.resume_offset),
             (unsigned long long)ctx->range.end);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_header);
  }
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, worker_write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "downloadmgr/0.1");

  struct curl_slist *headers = NULL;
  if (ctx->request_ctx) {
    if (ctx->request_ctx->cookie && ctx->request_ctx->cookie[0])
      curl_easy_setopt(curl, CURLOPT_COOKIE, ctx->request_ctx->cookie);
    if (ctx->request_ctx->referrer && ctx->request_ctx->referrer[0])
      curl_easy_setopt(curl, CURLOPT_REFERER, ctx->request_ctx->referrer);
    headers = curl_client_build_headers(ctx->request_ctx->extra_headers);
    if (headers)
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

  if (ctx->speed_limit_bps > 0) {
    curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                     (curl_off_t)ctx->speed_limit_bps);
    long low_speed_limit = 1024L;
    if (ctx->speed_limit_bps < 1024) {
      low_speed_limit = (long)(ctx->speed_limit_bps / 2);
      if (low_speed_limit < 1)
        low_speed_limit = 1;
    }
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, low_speed_limit);
  } else {
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  }

  CURLcode res = curl_easy_perform(curl);

  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

  uint64_t intended_write_offset = ctx->range.start + ctx->range.resume_offset;
  bool ok_200 = (http_status == 200) &&
                (ctx->range.whole_file ||
                 (ctx->total_workers == 1 && intended_write_offset == 0));
  bool http_ok = (http_status == 206 || ok_200);

  ctx->succeeded = ctx->truncated || (res == CURLE_OK && http_ok);

  if (headers)
    curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

/**
 * Worker thread entry point – loops, acquiring new segments from the
 * rebalance pool until no work remains or a fatal error occurs.
 */
static int worker_thread_function(void *arg) {
  WorkerContext *ctx = (WorkerContext *)arg;
  bool overall_ok = true;

  for (;;) {
    run_one_segment(ctx);

    if (!ctx->succeeded) {
      overall_ok = false;
      break;
    }

    if (ctx->pool)
      rebalance_pool_mark_done(ctx->pool, ctx->slot_index);

    if (!ctx->pool)
      break; // one-shot mode

    Range next_range;
    _Atomic uint64_t *next_progress_slot = NULL;
    int next_slot =
        rebalance_pool_acquire(ctx->pool, &next_range, &next_progress_slot);
    if (next_slot < 0)
      break;

    ctx->range = next_range;
    ctx->slot_index = next_slot;
    ctx->chunk_progress_slot = next_progress_slot;
  }

  ctx->succeeded = overall_ok;
  return overall_ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

WorkerPoolResult
worker_pool_run(const char *url, const Range *ranges, int n_workers,
                const char *dest_path, _Atomic uint64_t *total_bytes_downloaded,
                _Atomic bool *cancel_flag, _Atomic bool *pause_flag,
                _Atomic uint64_t **chunk_progress_slots,
                uint64_t total_speed_limit_bps, const RequestContext *ctx_in,
                RebalancePool *rebalance) {

  WorkerPoolResult result = {.all_succeeded = true,
                             .total_bytes_downloaded = 0};

  FileHandle fd = file_open_rw(dest_path);
  if (fd == INVALID_FILE_HANDLE) {
    result.all_succeeded = false;
    return result;
  }

  WorkerContext *contexts = calloc((size_t)n_workers, sizeof(WorkerContext));
  dm_thread_t *threads = calloc((size_t)n_workers, sizeof(dm_thread_t));
  if (!contexts || !threads) {
    file_close(fd);
    free(contexts);
    free(threads);
    result.all_succeeded = false;
    return result;
  }

  uint64_t per_worker_limit = 0;
  if (total_speed_limit_bps > 0) {
    per_worker_limit = total_speed_limit_bps / (uint64_t)n_workers;
    if (per_worker_limit == 0)
      per_worker_limit = 1;
  }

  for (int i = 0; i < n_workers; i++) {
    contexts[i].url = url;
    contexts[i].range = ranges[i];
    contexts[i].fd = fd;
    contexts[i].download_bytes_done = total_bytes_downloaded;
    contexts[i].cancel_flag = cancel_flag;
    contexts[i].pause_flag = pause_flag;
    contexts[i].chunk_progress_slot =
        chunk_progress_slots ? chunk_progress_slots[i] : NULL;
    contexts[i].total_workers = n_workers;
    contexts[i].speed_limit_bps = per_worker_limit;
    contexts[i].request_ctx = ctx_in;
    contexts[i].pool = rebalance;
    contexts[i].slot_index = i;
    dm_thread_create(&threads[i], worker_thread_function, &contexts[i]);
  }

  for (int i = 0; i < n_workers; i++) {
    int thread_result;
    dm_thread_join(&threads[i], &thread_result);
    if (!contexts[i].succeeded) {
      result.all_succeeded = false;
    }
    if (i < MAX_WORKERS) {
      result.chunk_succeeded[i] = contexts[i].succeeded;
    }
  }

  /* Total bytes rely on the global atomic counter (already updated by
     every worker via download_bytes_done). */
  result.total_bytes_downloaded =
      total_bytes_downloaded ? atomic_load(total_bytes_downloaded) : 0;

  file_close(fd);
  free(contexts);
  free(threads);
  return result;
}
