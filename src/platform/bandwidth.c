// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "bandwidth.h"
#include "../utils/config.h"
#include "thread.h"

#ifdef _WIN32
#include <windows.h>
static uint64_t now_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#include <time.h>
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}
#endif

/* Token bucket constants */

/* Cap the bucket at ~0.5 s worth of the configured rate so the aggregate
   rate tracks the cap closely while allowing small bursts. */
#define BUCKET_FRACTION_MS 500

/* Global state (accessed only from daemon thread + workers) */

static _Atomic int64_t g_tokens = 0;
static uint64_t g_last_refill_ms = 0; // daemon thread only

/* Public API */

void bandwidth_tick(void) {
  uint64_t cap = config_get_max_speed_bytes_per_sec();
  if (cap == 0)
    return;

  uint64_t now = now_ms();
  if (g_last_refill_ms == 0) {
    g_last_refill_ms = now;
    return;
  }
  uint64_t elapsed_ms = now - g_last_refill_ms;
  if (elapsed_ms == 0)
    return;
  g_last_refill_ms = now;

  int64_t add = (int64_t)((cap * elapsed_ms) / 1000ULL);
  int64_t max_bucket = (int64_t)((cap * BUCKET_FRACTION_MS) / 1000ULL);
  if (max_bucket < 1)
    max_bucket = 1;

  int64_t old_tokens, new_tokens;
  do {
    old_tokens = atomic_load(&g_tokens);
    new_tokens = old_tokens + add;
    if (new_tokens > max_bucket)
      new_tokens = max_bucket;
  } while (!atomic_compare_exchange_weak(&g_tokens, &old_tokens, new_tokens));
}

bool bandwidth_acquire(uint64_t bytes, const _Atomic bool *cancel_flag,
                       const _Atomic bool *pause_flag) {
  uint64_t cap = config_get_max_speed_bytes_per_sec();
  if (cap == 0)
    return true;

  uint64_t remaining = bytes;
  while (remaining > 0) {
    if ((cancel_flag && atomic_load(cancel_flag)) ||
        (pause_flag && atomic_load(pause_flag))) {
      return false;
    }

    int64_t old_tokens = atomic_load(&g_tokens);
    if (old_tokens <= 0) {
      dm_thread_sleep_ms(20); // wait for next refill tick
      continue;
    }

    uint64_t take =
        (uint64_t)old_tokens < remaining ? (uint64_t)old_tokens : remaining;
    int64_t new_tokens = old_tokens - (int64_t)take;
    if (!atomic_compare_exchange_weak(&g_tokens, &old_tokens, new_tokens))
      continue;

    remaining -= take;
  }
  return true;
}
