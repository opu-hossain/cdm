// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef CORE_SCHEDULER_H
#define CORE_SCHEDULER_H

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

/**
 * Report the current progress of all active downloads to connected IPC
 * clients and persist chunk progress to the database.
 */
void scheduler_report_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_SCHEDULER_H */
