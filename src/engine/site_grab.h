// SPDX-License-Identifier: MIT
#ifndef ENGINE_SITE_GRAB_H
#define ENGINE_SITE_GRAB_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
typedef struct {
  uint64_t downloaded_bytes;
  uint64_t total_bytes; /* 0 = unknown */
  uint64_t speed_bps;   /* 0 = unknown */
  uint64_t eta_seconds; /* UINT64_MAX = unknown */
  double percent;       /* -1 = unknown */
} SiteGrabProgress;
bool site_grab_url_allowed(const char *url);
bool site_grab_public_url_allowed(const char *url);
bool site_grab_host_allowed(const char *host);
bool site_grab_format_id_valid(const char *id);
bool site_grab_parse_progress(const char *line, SiteGrabProgress *out);
#define SITE_GRAB_PROBE_MAX_FORMATS 64
typedef struct {
  char id[64];
  char ext[16];
  uint32_t width;
  uint32_t height;
  uint64_t size_bytes; /* 0 = unknown */
  bool size_estimated;
  bool has_video;
  bool has_audio;
} SiteGrabFormat;
typedef struct {
  char title[256];
  size_t format_count;
  SiteGrabFormat formats[SITE_GRAB_PROBE_MAX_FORMATS];
} SiteGrabProbe;
/* Pure parser for bounded yt-dlp JSON. json[length] must be NUL. */
bool site_grab_parse_probe_json(const char *json, size_t length, SiteGrabProbe *out);
/* Parse two JSON values printed by yt-dlp: title string, then projected formats. */
bool site_grab_parse_probe_output(const char *text, size_t length, SiteGrabProbe *out);
/* Read-only metadata probe; existing HTTPS site allowlist and tool opt-in apply. */
int site_grab_probe(const char *url, const _Atomic bool *cancel, SiteGrabProbe *out);
int site_grab_probe_with_consent(const char *url, bool public_site,
                                const _Atomic bool *cancel, SiteGrabProbe *out);
struct Download;
int site_grab_run_download(struct Download *download);
#endif
