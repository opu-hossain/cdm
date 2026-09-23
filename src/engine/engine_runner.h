// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef ENGINE_RUNNER_H
#define ENGINE_RUNNER_H

#ifdef __cplusplus
extern "C" {
#endif

struct Download;

/**
 * Download a file using the parallel engine.
 *
 * Orchestrates HEAD probing, range planning, worker pool execution,
 * retry with a single connection on parallel failure, and final
 * verification.
 *
 * @param d  Pointer to a Download structure populated with url and
 *           dest_path (and optional RequestOptions).
 * @return 0 on success, -1 on failure (retryable)
 *         -2 on verification failure (non-retryable — size or checksum
 *          mismatch; the partial file has already been removed)
 *         -3 on destination-already-exists (non-retryable — the target
 *          file existed before this attempt started; nothing was created
 *          or needs cleanup)
 *         -4 on missing resume file (non-retryable — stale ranges are cleared)
 */
int engine_run_download(struct Download *d);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_RUNNER_H */
