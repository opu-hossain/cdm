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
  uint64_t segment_length;
  bool is_initialization;
  char video_id[33];
} SabrMediaHeader;

/* Returns 1 for a complete frame, 0 for more bytes, -1 for invalid framing.
 * offset is unchanged on partial or invalid data. Payload remains caller-owned. */
int sabr_read_part(const unsigned char *data, size_t length, size_t *offset,
                   SabrPart *out);
bool sabr_decode_media_header(const unsigned char *data, size_t length,
                              SabrMediaHeader *out);
bool sabr_encode_request(uint32_t video_itag, uint32_t audio_itag,
                         uint32_t height, uint64_t player_ms,
                         const unsigned char *config, size_t config_length,
                         unsigned char *out, size_t capacity,
                         size_t *out_length);

#endif
