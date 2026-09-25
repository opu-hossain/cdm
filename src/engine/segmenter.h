// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef ENGINE_SEGMENTER_H
#define ENGINE_SEGMENTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_WORKERS 16

typedef struct {
  uint64_t start;
  uint64_t end;           // inclusive
  uint64_t resume_offset; // for resuming a partially‑completed range
  bool whole_file;        // true if range covers the entire file
  bool unknown_size;      // true when the whole-file response has no known size
} Range;

/**
 * Divide a file of `total_size` bytes into up to `n_workers` equal ranges.
 *
 * The last worker absorbs any remainder bytes so the entire file is
 * covered without overlap or gap.
 *
 * @param total_size  Total file size in bytes.
 * @param n_workers   Desired number of workers.
 * @param out         Output array, must have room for at least MAX_WORKERS
 *                    entries.
 * @return            Actual number of ranges produced (≤ n_workers).
 */
int segmenter_plan(uint64_t total_size, int n_workers, Range *out);

/**
 * Heuristic for choosing a reasonable number of parallel workers.
 *
 * - Unknown size → 1 (cannot plan ranges).
 * - < 1 MB        → 1 (connection overhead dominates).
 * - 1 MB – 20 MB  → up to 4.
 * - ≥ 20 MB       → the configured cap.
 *
 * @param total_size  File size in bytes; 0 means unknown.
 * @return            Worker count (always ≥ 1).
 */
int choose_worker_count(uint64_t total_size, int max_connections);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_SEGMENTER_H */
