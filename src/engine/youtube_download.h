// SPDX-License-Identifier: MIT
#ifndef CDM_YOUTUBE_DOWNLOAD_H
#define CDM_YOUTUBE_DOWNLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "youtube_sabr.h"

typedef struct {
  char video_id[33];
  char stream_url[2048];
  unsigned char config[32768];
  size_t config_length;
  uint32_t video_itag, audio_itag, height;
  uint64_t video_last_modified, audio_last_modified;
  char video_xtags[513], audio_xtags[513];
  uint64_t expected_size;
  SabrCapturedRequest browser_capture;
  bool from_browser;
} YoutubeSelection;

/* Parse the public Android player response for one advertised MP4 video track
 * and a compatible MP4 audio track. No signed URL or cookies are persisted. */
bool youtube_parse_player(const char *json, const char *video_id,
                          uint32_t selected_itag, YoutubeSelection *out);
bool youtube_sequence_url(const char *base, unsigned sequence,
                          char *out, size_t capacity);

struct Download;
int youtube_run_download(struct Download *download);

#endif
