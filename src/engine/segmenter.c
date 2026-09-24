// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "segmenter.h"

#include <stddef.h>
#include <stdint.h>

int segmenter_plan(uint64_t total_size, int n_workers, Range *out) {
  if (total_size == 0 || n_workers <= 0)
    return 0;

  /* Don't create more workers than bytes (avoids empty ranges). */
  if ((uint64_t)n_workers > total_size)
    n_workers = (int)total_size;

  uint64_t chunk = total_size / (uint64_t)n_workers;

  for (int i = 0; i < n_workers; i++) {
    out[i].start = (uint64_t)i * chunk;
    out[i].resume_offset = 0;
    out[i].whole_file = false;
    out[i].unknown_size = false;

    if (i == n_workers - 1) {
      /* Last worker picks up the remainder. */
      out[i].end = total_size - 1;
    } else {
      out[i].end = (uint64_t)(i + 1) * chunk - 1;
    }
  }
  return n_workers;
}

int choose_worker_count(uint64_t total_size) {
  const uint64_t ONE_MB = 1024ULL * 1024ULL;

  if (total_size == 0)
    return 1;
  if (total_size < ONE_MB)
    return 1;
  if (total_size < 20 * ONE_MB)
    return 4;
  return 8;
}
