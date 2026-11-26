// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef ENGINE_WORKER_POOL_H
#define ENGINE_WORKER_POOL_H

#include "../platform/curl_client.h" // for RequestContext
#include "segmenter.h"               // for Range, MAX_WORKERS

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rebalance pool types. */

/**
 * Callback invoked when a rebalance pool splits a chunk.
 *
 * @param userdata     User-supplied pointer.
 * @param victim_start Original start byte of the chunk whose tail was stolen.
 * @param new_start    Start byte of the new (stolen) chunk.
 * @param new_end      End byte (exclusive) of the new chunk.
 */
typedef void (*RebalanceSplitFn)(void *userdata, uint64_t victim_start,
                                 uint64_t new_start, uint64_t new_end);
typedef void (*RebalanceMutationLockFn)(void *userdata);

/** Opaque handle for the rebalance pool. */
typedef struct RebalancePool RebalancePool;

/**
 * Create a pool that lets idle workers steal remaining work from busy
 * workers, splitting chunks dynamically.
 *
 * @param ranges            Initial byte ranges (one per worker).
 * @param n_ranges          Number of initial ranges.
 * @param progress_slots    Per‑range progress counters (may be NULL).
 * @param min_steal_bytes   Minimum remaining bytes before a chunk is worth
 *                          stealing.
 * @param on_split          Callback invoked when a chunk is split.
 * @param lock_mutation     Lock protecting the split callback's shared state.
 * @param unlock_mutation   Release that lock after the split callback.
 * @param userdata          Opaque pointer forwarded to the callback.
 * @return                  New pool, or NULL on allocation failure.
 */
RebalancePool *rebalance_pool_create(const Range *ranges, int n_ranges,
                                     _Atomic uint64_t **progress_slots,
                                     uint64_t min_steal_bytes,
                                     RebalanceSplitFn on_split,
                                     RebalanceMutationLockFn lock_mutation,
                                     RebalanceMutationLockFn unlock_mutation,
                                     void *userdata);

/** Free a rebalance pool. */
void rebalance_pool_destroy(RebalancePool *pool);

/* Worker pool result. */

typedef struct {
  bool all_succeeded;
  bool range_invalidated; // A conditional range request received HTTP 200.
  bool chunk_succeeded[MAX_WORKERS]; // per‑worker success flag
  uint64_t total_bytes_downloaded;
} WorkerPoolResult;

/**
 * Run a download using multiple worker threads with optional rebalancing.
 *
 * Each worker handles a byte range; completed ranges may be dynamically
 * split and reassigned to idle workers.  On parallel failure, the caller
 * can fall back to a single‑connection retry.
 *
 * @param url                    Download URL.
 * @param ranges                 Byte ranges for each worker.
 * @param n_workers              Number of workers (≤ MAX_WORKERS).
 * @param dest_path              Output file path.
 * @param total_bytes_downloaded Global counter (updated atomically by all
 *                               workers).
 * @param cancel_flag            Shared cancel flag.
 * @param pause_flag             Shared pause flag.
 * @param chunk_progress_slots   Per‑range progress counters.
 * @param total_speed_limit_bps  Global speed limit (divided among workers).
 * @param ctx_in                 Request metadata (headers, cookies, etc.).
 * @param rebalance              Optional rebalance pool; NULL disables
 *                               dynamic work stealing.
 * @return                       Aggregated result structure.
 */
WorkerPoolResult
worker_pool_run(const char *url, const Range *ranges, int n_workers,
                const char *dest_path, _Atomic uint64_t *total_bytes_downloaded,
                _Atomic bool *cancel_flag, _Atomic bool *pause_flag,
                _Atomic uint64_t **chunk_progress_slots,
                uint64_t total_speed_limit_bps, const RequestContext *ctx_in,
                RebalancePool *rebalance);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_WORKER_POOL_H */
