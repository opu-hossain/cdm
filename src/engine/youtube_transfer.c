// SPDX-License-Identifier: MIT
#include "youtube_transfer.h"
#include "youtube_sabr.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  YoutubeTrackState *track;
  uint64_t expected, received, end_ms;
  uint32_t segment;
  bool active, skip, initialization;
} Pending;

static bool write_all(int fd, const unsigned char *data, size_t length) {
  while (length) {
    ssize_t count = write(fd, data, length);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    data += count;
    length -= (size_t)count;
  }
  return true;
}

bool youtube_process_ump(const unsigned char *data, size_t length,
                         const char *video_id, YoutubeTrackState *video,
                         YoutubeTrackState *audio) {
  if (!data || !video_id || !video || !audio || video->fd < 0 || audio->fd < 0 ||
      video->itag == audio->itag || !video->itag || !audio->itag) return false;
  off_t video_start = lseek(video->fd, 0, SEEK_END);
  off_t audio_start = lseek(audio->fd, 0, SEEK_END);
  if (video_start < 0 || audio_start < 0) return false;
  YoutubeTrackState saved_video = *video, saved_audio = *audio;
  Pending pending[256] = {0};
  bool valid = true;
  for (size_t offset = 0; valid && offset < length;) {
    SabrPart part;
    if (sabr_read_part(data, length, &offset, &part) != 1) {
      valid = false;
      break;
    }
    if (part.type == 44 || part.type == 46 || part.type == 43) {
      valid = false; // server error, reload, or redirect needs a fresh player response
    } else if (part.type == 42) {
      SabrFormatMetadata metadata;
      if (!sabr_decode_format_metadata(part.data, part.length, &metadata) ||
          strcmp(metadata.video_id, video_id) != 0) { valid = false; break; }
      YoutubeTrackState *track = metadata.itag == video->itag ? video :
                                 metadata.itag == audio->itag ? audio : NULL;
      if (track) {
        if (track->end_segment &&
            track->end_segment != metadata.end_segment) valid = false;
        track->end_segment = metadata.end_segment;
      }
    } else if (part.type == 20) {
      SabrMediaHeader header;
      if (!sabr_decode_media_header(part.data, part.length, &header) ||
          strcmp(header.video_id, video_id) != 0 ||
          header.header_id > 255 || !header.segment_length ||
          pending[header.header_id].active) { valid = false; break; }
      YoutubeTrackState *track = header.itag == video->itag ? video :
                                 header.itag == audio->itag ? audio : NULL;
      if (!track || (!header.is_initialization &&
                     (!header.segment_number || !header.end_ms ||
                      header.segment_number > track->last_segment + 1))) {
        valid = false; break;
      }
      pending[header.header_id] = (Pending){
          .track = track, .expected = header.segment_length,
          .end_ms = header.end_ms, .segment = header.segment_number,
          .active = true, .initialization = header.is_initialization,
          .skip = header.is_initialization ? track->initialized :
                  header.segment_number <= track->last_segment};
    } else if (part.type == 21) {
      if (part.length < 1) { valid = false; break; }
      Pending *p = &pending[part.data[0]];
      size_t chunk = part.length - 1;
      if (!p->active || chunk > p->expected - p->received ||
          (!p->skip && !write_all(p->track->fd, part.data + 1, chunk))) {
        valid = false; break;
      }
      p->received += chunk;
    } else if (part.type == 22) {
      if (part.length != 1) { valid = false; break; }
      Pending *p = &pending[part.data[0]];
      if (!p->active || p->received != p->expected) {
        valid = false; break;
      }
      if (!p->skip) {
        p->track->bytes += p->received;
        if (p->initialization) p->track->initialized = true;
        else {
          p->track->last_segment = p->segment;
          p->track->end_ms = p->end_ms;
        }
      }
      p->active = false;
    }
  }
  for (size_t i = 0; i < 256; i++)
    if (pending[i].active) valid = false;
  if (!valid) {
    ftruncate(video->fd, video_start);
    ftruncate(audio->fd, audio_start);
    lseek(video->fd, video_start, SEEK_SET);
    lseek(audio->fd, audio_start, SEEK_SET);
    *video = saved_video;
    *audio = saved_audio;
  }
  return valid;
}
