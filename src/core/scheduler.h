// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

#include "download_record.h"

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
