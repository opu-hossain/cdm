// SPDX-License-Identifier: MIT
#ifndef CDM_YOUTUBE_SABR_H
#define CDM_YOUTUBE_SABR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t type;
  const unsigned char *data;
  size_t length;
} SabrPart;

typedef struct {
  uint32_t header_id;
  uint32_t itag;
  uint32_t segment_number;
  uint64_t start_ms;
  uint64_t duration_ms;
  uint64_t end_ms; // derived from time_range ticks when present
  uint64_t segment_length;
  bool is_initialization;
  char video_id[33];
} SabrMediaHeader;

typedef struct {
  uint32_t itag;
  uint32_t end_segment;
  char video_id[33];
} SabrFormatMetadata;

typedef struct {
  uint32_t itag;
  uint64_t last_modified;
  const char *xtags; // optional UTF-8 tags; copied into the request
  const unsigned char *raw; // validated browser FormatId, if available
  size_t raw_length;
} SabrFormatId;

typedef struct {
  unsigned char config[11000];
  size_t config_length;
  unsigned char context[2048];
  size_t context_length;
  unsigned char video_format[560];
  size_t video_format_length;
  unsigned char audio_format[560];
  size_t audio_format_length;
  uint32_t audio_itag;
} SabrCapturedRequest;

typedef struct {
  SabrFormatId format;
  uint64_t end_ms;
  uint32_t end_segment;
  uint32_t start_segment;
} SabrBufferedRange;

/* Returns 1 for a complete frame, 0 for more bytes, -1 for invalid framing.
 * offset is unchanged on partial or invalid data. Payload remains caller-owned. */
int sabr_read_part(const unsigned char *data, size_t length, size_t *offset,
                   SabrPart *out);
bool sabr_decode_media_header(const unsigned char *data, size_t length,
                              SabrMediaHeader *out);
bool sabr_decode_format_metadata(const unsigned char *data, size_t length,
                                 SabrFormatMetadata *out);
/* Rejects malformed or unauthenticated browser requests; never stores them. */
bool sabr_parse_captured_request(const unsigned char *data, size_t length,
                                 uint32_t video_itag,
                                 SabrCapturedRequest *out);
/* 0 = absent, 1 = accepted, 2 = pending, 3 = rejected, -1 = malformed. */
int sabr_protection_status(const unsigned char *ump, size_t length);
bool sabr_update_playback_context(SabrCapturedRequest *captured,
                                  const unsigned char *ump, size_t length);
bool sabr_encode_request(const SabrFormatId *video, const SabrFormatId *audio,
                         uint32_t height, uint64_t player_ms,
                         const unsigned char *config, size_t config_length,
                         unsigned char *out, size_t capacity,
                         size_t *out_length);
bool sabr_encode_request_with_ranges(const SabrFormatId *video,
    const SabrFormatId *audio, uint32_t height, uint64_t player_ms,
    const unsigned char *config, size_t config_length,
    const SabrBufferedRange *ranges, size_t range_count,
    unsigned char *out, size_t capacity, size_t *out_length);
bool sabr_encode_request_with_context(const SabrFormatId *video,
    const SabrFormatId *audio, uint32_t height, uint64_t player_ms,
    const unsigned char *config, size_t config_length,
    const SabrBufferedRange *ranges, size_t range_count,
    const unsigned char *context, size_t context_length,
    unsigned char *out, size_t capacity, size_t *out_length);

#endif
