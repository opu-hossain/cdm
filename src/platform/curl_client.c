// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "curl_client.h"

#include "../utils/log.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

/* Internal helpers */

/**
 * libcurl header callback – called once per response header line.
 *
 * We use it only to detect the Accept-Ranges: bytes header.  The userdata
 * pointer points to the FileInfo struct being filled.
 */
static size_t head_header_callback(void *data, size_t size, size_t nmemb,
                                   void *userdata) {
  FileInfo *info = (FileInfo *)userdata;
  size_t total = size * nmemb;

  if (total >= 19 &&
      strncasecmp((char *)data, "Accept-Ranges: bytes", 19) == 0) {
    info->supports_ranges = true;
  }
  return total; // must return the number of bytes consumed
}

/* Public API */

int curl_client_head(const char *url, const RequestContext *ctx,
                     FileInfo *out) {
  memset(out, 0, sizeof(*out));

  CURL *curl = curl_easy_init();
  if (!curl) {
    LOG_ERROR("curl_easy_init failed for %s", url);
    return -1;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, head_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, out);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "downloadmgr/0.1");
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

  if (res == CURLE_OK && http_status >= 200 && http_status < 300) {
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

  if (headers)
    curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return (res == CURLE_OK && http_status >= 200 && http_status < 300) ? 0
                                                                        : -1;
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
