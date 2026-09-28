// SPDX-License-Identifier: MIT
#ifndef ENGINE_SITE_GRAB_H
#define ENGINE_SITE_GRAB_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  uint64_t downloaded_bytes;
  uint64_t total_bytes; /* 0 = unknown */
  uint64_t speed_bps;   /* 0 = unknown */
  uint64_t eta_seconds; /* UINT64_MAX = unknown */
  double percent;       /* -1 = unknown */
} SiteGrabProgress;
bool site_grab_url_allowed(const char *url);
bool site_grab_host_allowed(const char *host);
bool site_grab_parse_progress(const char *line, SiteGrabProgress *out);
struct Download;
int site_grab_run_download(struct Download *download);
#endif
