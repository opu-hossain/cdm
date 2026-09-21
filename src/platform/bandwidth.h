// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_BANDWIDTH_H
#define PLATFORM_BANDWIDTH_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Refill the global bandwidth bucket.
 *
 * Call once per daemon tick (~200 ms).  Does nothing if the global speed
 * cap is 0 (unlimited).
 */
void bandwidth_tick(void);

/**
 * Block until `bytes` tokens are available from the shared global bucket,
 * then consume them.
 *
 * If the global cap is 0 (unlimited) the function returns immediately.
 * While waiting it periodically checks the optional cancel/pause flags;
 * if either becomes true the function returns false immediately without
 * acquiring the full amount.
 *
 * @param bytes        Number of bytes to acquire.
 * @param cancel_flag  Optional cancel flag (may be NULL).
 * @param pause_flag   Optional pause flag (may be NULL).
 * @return true on success, false if cancelled/paused.
 */
bool bandwidth_acquire(uint64_t bytes, const _Atomic bool *cancel_flag,
                       const _Atomic bool *pause_flag);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_BANDWIDTH_H */
