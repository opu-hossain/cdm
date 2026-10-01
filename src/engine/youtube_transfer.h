// SPDX-License-Identifier: MIT
#ifndef CDM_YOUTUBE_TRANSFER_H
#define CDM_YOUTUBE_TRANSFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* One bounded server response, backed by an unlinked temporary file. */
typedef struct {
  FILE *file;
  const unsigned char *data;
  size_t length;
} YoutubeResponseFile;
bool youtube_response_open(YoutubeResponseFile *response, const char *destination);
bool youtube_response_append(YoutubeResponseFile *response, const void *data,
                              size_t length);
bool youtube_response_map(YoutubeResponseFile *response);
void youtube_response_close(YoutubeResponseFile *response);

typedef struct {
  int fd;
  uint32_t itag, first_segment, last_segment, end_segment;
  uint64_t end_ms, bytes;
  bool initialized, has_segment, has_end;
} YoutubeTrackState;

/* Append only complete, ordered media segments. On malformed or incomplete
 * responses, both files and state roll back to their starting offsets. */
bool youtube_process_ump(const unsigned char *data, size_t length,
                         const char *video_id, YoutubeTrackState *video,
                         YoutubeTrackState *audio);
bool youtube_process_ump_ex(const unsigned char *data, size_t length,
                            const char *video_id, YoutubeTrackState *video,
                            YoutubeTrackState *audio, const char **error);

#endif
