// SPDX-License-Identifier: MIT
#ifndef CDM_YOUTUBE_TRANSFER_H
#define CDM_YOUTUBE_TRANSFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int fd;
  uint32_t itag, last_segment, end_segment;
  uint64_t end_ms, bytes;
  bool initialized;
} YoutubeTrackState;

/* Append only complete, ordered media segments. On malformed or incomplete
 * responses, both files and state roll back to their starting offsets. */
bool youtube_process_ump(const unsigned char *data, size_t length,
                         const char *video_id, YoutubeTrackState *video,
                         YoutubeTrackState *audio);

#endif
