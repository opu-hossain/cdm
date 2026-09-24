// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_CURL_CLIENT_H
#define PLATFORM_CURL_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Types. */

typedef struct {
  const char *cookie;
  const char *referrer;
  const char *extra_headers; // "Key: Value\nKey: Value" lines
} RequestContext;

typedef struct {
  uint64_t total_size; // 0 = unknown
  bool supports_ranges;
  char content_disposition[512]; // Empty if no usable header was received.
  char etag[256]; // Raw ETag value, including quotes or W/ prefix.
  char last_modified[128]; // Raw Last-Modified date.
} FileInfo;

/* Functions. */

/**
 * Probe file metadata with HEAD, falling back to a one-byte range GET.
 *
 * Follows redirects up to 10 levels.  On success, fills `out` with the
 * total size (or 0 if not reported), range support, filename suggestion, and
 * raw ETag/Last-Modified validators from the final response.
 *
 * @param url   Target URL.
 * @param ctx   Optional request context (cookies, referrer, extra headers).
 * @param out   Result structure.
 * @return 0 on success, -1 on failure.
 */
int curl_client_head(const char *url, const RequestContext *ctx, FileInfo *out);

/**
 * Build a curl_slist from a "Key: Value\nKey2: Value2" string.
 *
 * The caller must free the returned list with curl_slist_free_all().
 *
 * @param extra_headers  Newline-separated header lines (may be NULL/empty).
 * @return Heap-allocated header list, or NULL.
 */
struct curl_slist *curl_client_build_headers(const char *extra_headers);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_CURL_CLIENT_H */
