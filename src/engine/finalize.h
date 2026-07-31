// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef ENGINE_FINALIZE_H
#define ENGINE_FINALIZE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Verify the downloaded file against expected size and/or SHA‑256 hash.
 *
 * If expected_size is non‑zero, the actual file size must match exactly.
 * If expected_sha256_hex is non‑empty, the file’s SHA‑256 digest is
 * computed and compared (case‑insensitive).
 *
 * @param dest_path            Path to the downloaded file.
 * @param expected_size        Expected file size (0 = skip size check).
 * @param expected_sha256_hex  Expected hex digest (empty = skip hash check).
 * @return  0 on success, -1 on mismatch or I/O error.
 */
int engine_finalize(const char *dest_path, uint64_t expected_size,
                    const char *expected_sha256_hex);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_FINALIZE_H */
