// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

#include "download_record.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called periodically by the daemon event loop.
 *
 * Starts up to the configured maximum number of queued downloads by
 * spawning worker threads.
 */
void scheduler_tick(void);

typedef enum {
  SCHEDULE_ALWAYS,
  SCHEDULED_ACTIVE,
  SCHEDULED_IDLE,
} SchedulerQueueState;

/* The start minute is inclusive, stop minute exclusive; local wall clock. */
SchedulerQueueState scheduler_queue_state(const Queue *queue, time_t now);
/* Evaluate transitions at an injected wall clock for deterministic tests. */
void scheduler_schedule_tick_at(time_t now);
/* Evaluate queue completion actions at an injected clock for tests. */
void scheduler_post_actions_tick_at(time_t now);

/** Cancel and join all scheduler workers before shared teardown. */
void scheduler_shutdown(void);

/**
 * Report the current progress of all active downloads to connected IPC
 * clients and persist chunk progress to the database.
 */
void scheduler_report_progress(void);

/* Advance a per-download speed/ETA sample using monotonic milliseconds. */
DownloadTransferMetrics scheduler_advance_transfer_metrics(
    DownloadTransferMetrics previous, uint64_t bytes_received,
    uint64_t total_bytes, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* CORE_SCHEDULER_H */
