// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "curl_client.h"

#include "../utils/log.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Internal helpers */

/**
 * libcurl header callback – called once per response header line.
 *
 * Capture range support and the complete size from a one-byte range reply.
 */
typedef struct {
  FileInfo *info;
  uint64_t range_total;
  bool has_range_total;
  bool body_aborted;
} ProbeState;

static size_t probe_header_callback(void *data, size_t size, size_t nmemb,
                                    void *userdata) {
  ProbeState *state = userdata;
  size_t total = size * nmemb;

  /* Redirects and intermediate HTTP responses may have different headers. */
  if (total >= 5 && strncasecmp(data, "HTTP/", 5) == 0) {
    memset(state->info, 0, sizeof(*state->info));
    state->has_range_total = false;
  }
  if (total >= 19 &&
      strncasecmp((char *)data, "Accept-Ranges: bytes", 19) == 0) {
    state->info->supports_ranges = true;
  }
  if (total > 20 && strncasecmp(data, "Content-Disposition:", 20) == 0) {
    const char *value = (const char *)data + 20;
    size_t n = total - 20;
    while (n && (*value == ' ' || *value == '\t')) {
      value++;
      n--;
    }
    while (n && (value[n - 1] == '\r' || value[n - 1] == '\n'))
      n--;
    if (n < sizeof(state->info->content_disposition)) {
      memcpy(state->info->content_disposition, value, n);
      state->info->content_disposition[n] = '\0';
    }
  }
  if (total > 14 && strncasecmp(data, "Content-Range:", 14) == 0) {
    char value[96];
    size_t n = total - 14;
    if (n >= sizeof(value))
      return total;
    memcpy(value, (char *)data + 14, n);
    value[n] = '\0';
    char *cursor = value;
    while (*cursor == ' ' || *cursor == '\t')
      cursor++;
    if (strncasecmp(cursor, "bytes ", 6) != 0)
      return total;
    cursor += 6;
    errno = 0;
    char *end = NULL;
    unsigned long long first = strtoull(cursor, &end, 10);
    if (errno || end == cursor || *end != '-')
      return total;
    cursor = end + 1;
    unsigned long long last = strtoull(cursor, &end, 10);
    if (errno || end == cursor || *end != '/')
      return total;
    cursor = end + 1;
    unsigned long long size_bytes = strtoull(cursor, &end, 10);
    if (errno || end == cursor || first != 0 || last != 0 ||
        size_bytes == 0 || (*end != '\r' && *end != '\n' && *end != '\0'))
      return total;
    state->range_total = (uint64_t)size_bytes;
    state->has_range_total = true;
  }
  return total; // must return the number of bytes consumed
}

static size_t probe_body_callback(char *data, size_t size, size_t nmemb,
                                  void *userdata) {
  (void)data;
  ProbeState *state = userdata;
  state->body_aborted = true;
  (void)size;
  (void)nmemb;
  return 0; /* Headers are enough; never download an ignored full response. */
}

/* Public API */

int curl_client_head(const char *url, const RequestContext *ctx,
                     FileInfo *out) {
  memset(out, 0, sizeof(*out));
  ProbeState state = {.info = out};

  CURL *curl = curl_easy_init();
  if (!curl) {
    LOG_ERROR("curl_easy_init failed for %s", url);
    return -1;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, probe_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cdm/0.1");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  struct curl_slist *headers = NULL;
  if (ctx) {
    if (ctx->cookie && ctx->cookie[0])
      curl_easy_setopt(curl, CURLOPT_COOKIE, ctx->cookie);
    if (ctx->referrer && ctx->referrer[0])
      curl_easy_setopt(curl, CURLOPT_REFERER, ctx->referrer);
    headers = curl_client_build_headers(ctx->extra_headers);
    if (headers)
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  CURLcode res = curl_easy_perform(curl);

  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

  bool success = res == CURLE_OK && http_status >= 200 && http_status < 300;
  if (success) {
    curl_off_t content_length = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                      &content_length);
    out->total_size = (content_length > 0) ? (uint64_t)content_length : 0;
  } else if (res != CURLE_OK) {
    LOG_WARN("HEAD request failed for %s: %s", url,
             curl_easy_strerror(res));
  } else {
    LOG_WARN("HEAD request returned HTTP %ld for %s", http_status, url);
  }

  if (!success) {
    memset(out, 0, sizeof(*out));
    state.has_range_total = false;
    state.body_aborted = false;
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, probe_body_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    success = (res == CURLE_OK ||
               (res == CURLE_WRITE_ERROR && state.body_aborted)) &&
              (http_status == 206 || http_status == 200);
    if (success && http_status == 206) {
      success = state.has_range_total;
      if (success) {
        out->total_size = state.range_total;
        out->supports_ranges = true;
      }
    } else if (success) {
      curl_off_t content_length = -1;
      curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                        &content_length);
      out->total_size = (content_length > 0) ? (uint64_t)content_length : 0;
      out->supports_ranges = false;
    }
    if (!success)
      LOG_WARN("GET range probe failed for %s: HTTP %ld, %s", url,
               http_status, curl_easy_strerror(res));
  }

  if (headers)
    curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (!success)
    memset(out, 0, sizeof(*out));
  return success ? 0 : -1;
}

struct curl_slist *curl_client_build_headers(const char *extra_headers) {
  if (!extra_headers || extra_headers[0] == '\0')
    return NULL;

  struct curl_slist *list = NULL;
  char buf[4096];
  strncpy(buf, extra_headers, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *saveptr = NULL;
  char *line = strtok_r(buf, "\n", &saveptr);
  while (line) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\r') // tolerate CRLF
      line[len - 1] = '\0';
    if (line[0] != '\0')
      list = curl_slist_append(list, line);
    line = strtok_r(NULL, "\n", &saveptr);
  }
  return list;
}
