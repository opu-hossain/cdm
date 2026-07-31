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
 * @return   0 on success, -1 on retryable failure, -2 on non‑retryable
 *           failure (verification mismatch – partial file removed).
 */
int engine_run_download(struct Download *d);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_RUNNER_H */
