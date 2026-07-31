// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_SHA256_H
#define PLATFORM_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_BLOCK_SIZE 32 /* bytes in a SHA-256 digest */

typedef struct {
  uint8_t data[64];
  uint32_t datalen;
  uint64_t bitlen;
  uint32_t state[8];
} SHA256_CTX;

/**
 * Initialize a SHA‑256 context.
 */
void sha256_init(SHA256_CTX *ctx);

/**
 * Feed `len` bytes of `data` into the hash.
 */
void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len);

/**
 * Finalize the hash and write the raw digest into `hash` (must have room for
 * SHA256_BLOCK_SIZE bytes).
 */
void sha256_final(SHA256_CTX *ctx, uint8_t hash[SHA256_BLOCK_SIZE]);

/**
 * Convert a raw digest into a null‑terminated lowercase hex string.
 *
 * `out` must point to at least 65 bytes (64 hex chars + NUL).
 */
void sha256_to_hex(const uint8_t hash[SHA256_BLOCK_SIZE], char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_SHA256_H */
