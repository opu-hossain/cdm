// SPDX-License-Identifier: MIT
#include "youtube_transfer.h"
#include "youtube_sabr.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

bool youtube_response_open(YoutubeResponseFile *response, const char *destination) {
  if (!response) return false;
  *response = (YoutubeResponseFile){0};
  if (destination) {
    char path[1100];
    int count = snprintf(path, sizeof(path), "%s.cdm-response-XXXXXX", destination);
    if (count < 0 || (size_t)count >= sizeof(path)) return false;
    int fd = mkstemp(path);
    if (fd < 0) return false;
    if (unlink(path) != 0) { close(fd); return false; }
    response->file = fdopen(fd, "w+b");
    if (!response->file) close(fd);
  } else response->file = tmpfile();
  if (response->file && fcntl(fileno(response->file), F_SETFD, FD_CLOEXEC) == -1) {
    fclose(response->file);
    response->file = NULL;
  }
  return response->file != NULL;
}

bool youtube_response_append(YoutubeResponseFile *response, const void *data,
                              size_t length) {
  const size_t limit = 1024u * 1024u * 1024u;
  if (!response || !response->file || response->data || !data ||
      response->length > limit || length > limit - response->length)
    return false;
  if (fwrite(data, 1, length, response->file) != length) return false;
  response->length += length;
  const unsigned char *bytes = data;
  for (size_t offset = 0; offset < length && !response->invalid_prefix;) {
    if (!response->remaining) {
      if (response->prefix_length == sizeof(response->prefix)) {
        response->invalid_prefix = true;
        break;
      }
      response->prefix[response->prefix_length++] = bytes[offset++];
      size_t consumed = 0;
      int rc = sabr_read_part_prefix(response->prefix, response->prefix_length,
          &consumed, &response->part_type, &response->remaining);
      if (rc < 0) response->invalid_prefix = true;
      if (rc != 1) continue;
      response->prefix_length = 0;
      response->skip_media_id = response->part_type == 21;
    } else {
      size_t count = length - offset;
      if (count > response->remaining) count = response->remaining;
      if (response->part_type == 21) {
        response->media_bytes += count - (response->skip_media_id ? 1 : 0);
        response->skip_media_id = false;
      }
      response->remaining -= (uint32_t)count;
      offset += count;
    }
  }
  return true;
}

bool youtube_response_map(YoutubeResponseFile *response) {
  if (!response || !response->file || !response->length || response->data ||
      fflush(response->file) != 0) return false;
  void *data = mmap(NULL, response->length, PROT_READ, MAP_PRIVATE,
                    fileno(response->file), 0);
  if (data == MAP_FAILED) return false;
  response->data = data;
  return true;
}

void youtube_response_close(YoutubeResponseFile *response) {
  if (!response) return;
  if (response->data) munmap((void *)response->data, response->length);
  if (response->file) fclose(response->file);
  *response = (YoutubeResponseFile){0};
}

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

bool youtube_process_ump_ex(const unsigned char *data, size_t length,
                         const char *video_id, YoutubeTrackState *video,
                         YoutubeTrackState *audio, const char **error) {
  if (error) *error = "Invalid YouTube media response";
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
      if (error) *error = "Truncated or oversized YouTube media frame";
      valid = false;
      break;
    }
    if (part.type == 44 || part.type == 46 || part.type == 43) {
      if (error) *error = "YouTube requested fresh playback context; play the video and offer it again";
      valid = false; // server error, reload, or redirect needs a fresh player response
    } else if (part.type == 42) {
      SabrFormatMetadata metadata;
      if (!sabr_decode_format_metadata(part.data, part.length, &metadata) ||
          strcmp(metadata.video_id, video_id) != 0) {
        if (error) *error = "YouTube format metadata does not match the selected video";
        valid = false; break;
      }
      YoutubeTrackState *track = metadata.itag == video->itag ? video :
                                 metadata.itag == audio->itag ? audio : NULL;
      if (track) {
        if (track->has_end &&
            track->end_segment != metadata.end_segment) valid = false;
        track->end_segment = metadata.end_segment;
        track->has_end = true;
      }
    } else if (part.type == 20) {
      SabrMediaHeader header;
      if (!sabr_decode_media_header(part.data, part.length, &header) ||
          strcmp(header.video_id, video_id) != 0 ||
          header.header_id > 255 ||
          pending[header.header_id].active) {
        if (error) *error = "Unsupported YouTube segment header";
        valid = false; break;
      }
      YoutubeTrackState *track = header.itag == video->itag ? video :
                                 header.itag == audio->itag ? audio : NULL;
      if (!track || (!header.is_initialization &&
                     (!header.end_ms ||
                      (track->has_segment ?
                       header.segment_number > track->last_segment + 1 :
                       header.segment_number > 1)))) {
        if (error) *error = "YouTube returned an unexpected track or segment gap";
        valid = false; break;
      }
      pending[header.header_id] = (Pending){
          .track = track, .expected = header.segment_length,
          .end_ms = header.end_ms, .segment = header.segment_number,
          .active = true, .initialization = header.is_initialization,
          .skip = header.is_initialization ? track->initialized :
                  track->has_segment && header.segment_number <= track->last_segment};
    } else if (part.type == 21) {
      if (part.length < 1) { valid = false; break; }
      Pending *p = &pending[part.data[0]];
      size_t chunk = part.length - 1;
      if (!p->active || (p->expected && chunk > p->expected - p->received) ||
          (!p->skip && !write_all(p->track->fd, part.data + 1, chunk))) {
        if (error) *error = "YouTube segment payload is invalid or cannot be written";
        valid = false; break;
      }
      p->received += chunk;
    } else if (part.type == 22) {
      if (part.length != 1) { valid = false; break; }
      Pending *p = &pending[part.data[0]];
      if (!p->active || !p->received ||
          (p->expected && p->received != p->expected)) {
        if (error) *error = "YouTube segment ended before its expected size";
        valid = false; break;
      }
      if (!p->skip) {
        p->track->bytes += p->received;
        if (p->initialization) p->track->initialized = true;
        else {
          if (!p->track->has_segment) p->track->first_segment = p->segment;
          p->track->has_segment = true;
          p->track->last_segment = p->segment;
          p->track->end_ms = p->end_ms;
        }
      }
      p->active = false;
    }
  }
  for (size_t i = 0; i < 256; i++)
    if (pending[i].active) {
      if (error) *error = "YouTube response ended with an incomplete segment";
      valid = false;
    }
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

bool youtube_process_ump(const unsigned char *data, size_t length,
                         const char *video_id, YoutubeTrackState *video,
                         YoutubeTrackState *audio) {
  return youtube_process_ump_ex(data, length, video_id, video, audio, NULL);
}
