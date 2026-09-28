// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "finalize.h"

#include "../platform/file_io.h"
#include "../platform/sha256.h"
#include "../utils/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HASH_READ_BUF_SIZE 65536

/* Helpers */

/**
 * Case‑insensitive hexadecimal string equality.
 *
 * Portable – avoids strcasecmp (POSIX) / _stricmp (MSVC) branching.
 *
 * @return true if the two strings are equal ignoring case.
 */
static bool hex_equals_ci(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z')
      ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z')
      cb = (char)(cb - 'A' + 'a');
    if (ca != cb)
      return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

/**
 * Compute the SHA‑256 hash of a file and return its hex representation.
 *
 * Reads the file in fixed‑size chunks to avoid excessive memory usage.
 *
 * @param path     File to hash.
 * @param out_hex  Output buffer (at least 65 bytes).  Filled with the
 *                 null‑terminated hex digest.
 * @return 0 on success, -1 on read error.
 */
static int compute_sha256_hex(const char *path, char out_hex[65]) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return -1;

  SHA256_CTX ctx;
  sha256_init(&ctx);

  uint8_t buf[HASH_READ_BUF_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    sha256_update(&ctx, buf, n);

  bool read_error = ferror(fp) != 0;
  fclose(fp);
  if (read_error)
    return -1;

  uint8_t digest[SHA256_BLOCK_SIZE];
  sha256_final(&ctx, digest);
  sha256_to_hex(digest, out_hex);
  return 0;
}

/* Public API */

int engine_finalize(const char *dest_path, uint64_t expected_size,
                    const char *expected_sha256_hex) {
  if (expected_size != 0) {
    uint64_t actual_size = file_get_size(dest_path);
    if (actual_size != expected_size) {
      LOG_ERROR("Size mismatch! Expected %llu bytes, got %llu bytes.\n",
                (unsigned long long)expected_size,
                (unsigned long long)actual_size);
      return -1;
    }
  }

  if (expected_sha256_hex && expected_sha256_hex[0] != '\0') {
    char actual_hex[65];
    if (compute_sha256_hex(dest_path, actual_hex) != 0) {
      LOG_ERROR("Could not read %s to verify checksum\n", dest_path);
      return -1;
    }
    LOG_INFO("Actual SHA256: %s", actual_hex);
    LOG_INFO("Expected SHA256: %s", expected_sha256_hex);
    if (!hex_equals_ci(actual_hex, expected_sha256_hex)) {
      LOG_ERROR("Checksum mismatch for %s: expected %s, got %s\n", dest_path,
                expected_sha256_hex, actual_hex);
      return -1;
    }
    LOG_INFO("Checksum verified for %s\n", dest_path);
  }

  return 0;
}

int engine_quarantine_output(const char *path, char *out, size_t out_size) {
  if (!path || !out || !out_size)
    return -1;
  const char *slash = strrchr(path, '/');
  const char *name = slash ? slash + 1 : path;
  if (!*name)
    return -1;
  char directory[1024];
  size_t parent_len = slash ? (size_t)(slash - path) : 0;
  const char *parent = slash ? (parent_len ? path : "/") : ".";
  size_t copy_len = slash ? (parent_len ? parent_len : 1) : 1;
  if (copy_len + sizeof("/.quarantine") > sizeof(directory))
    return -1;
  memcpy(directory, parent, copy_len);
  directory[copy_len] = '\0';
  if (copy_len > 1 || directory[0] != '/')
    strcat(directory, "/");
  strcat(directory, ".quarantine");
  if (mkdir(directory, 0700) != 0 && errno != EEXIST)
    return -1;
  struct stat st;
  if (lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) ||
      (st.st_mode & 0077) != 0)
    return -1;
  for (unsigned int suffix = 0; suffix < 1000000; suffix++) {
    int n = suffix ? snprintf(out, out_size, "%s/%s.%u", directory,
                              name, suffix)
                   : snprintf(out, out_size, "%s/%s", directory, name);
    if (n < 0 || (size_t)n >= out_size)
      return -1;
    if (link(path, out) == 0) {
      if (unlink(path) == 0)
        return 0;
      unlink(out);
      return -1;
    }
    if (errno != EEXIST)
      return -1;
  }
  return -1;
}
