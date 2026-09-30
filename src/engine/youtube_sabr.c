// SPDX-License-Identifier: MIT
#include "youtube_sabr.h"
#include <string.h>

#define SABR_MAX_PART (8u * 1024u * 1024u)

typedef struct {
  unsigned char *data;
  size_t length;
  size_t capacity;
  bool good;
} Writer;

static bool ump_number(const unsigned char *data, size_t length,
                       size_t *position, uint32_t *value) {
  if (*position >= length) return false;
  unsigned char first = data[*position];
  size_t width = first < 128 ? 1 : first < 192 ? 2 : first < 224 ? 3
                                           : first < 240 ? 4 : 5;
  if (width > length - *position) return false;
  uint32_t number = 0;
  if (width == 1) number = first;
  else if (width == 5) {
    for (size_t i = 1; i < 5; i++)
      number |= (uint32_t)data[*position + i] << ((i - 1) * 8);
  } else {
    unsigned shift = (unsigned)(8 - width);
    number = first & ((1u << shift) - 1u);
    for (size_t i = 1; i < width; i++, shift += 8)
      number |= (uint32_t)data[*position + i] << shift;
  }
  *position += width;
  *value = number;
  return true;
}

int sabr_read_part(const unsigned char *data, size_t length, size_t *offset,
                   SabrPart *out) {
  if (!data || !offset || !out || *offset > length) return -1;
  size_t position = *offset;
  uint32_t type, size;
  if (!ump_number(data, length, &position, &type) ||
      !ump_number(data, length, &position, &size)) return 0;
  if (type > 4096 || size > SABR_MAX_PART) return -1;
  if (size > length - position) return 0;
  *out = (SabrPart){.type = type, .data = data + position, .length = size};
  *offset = position + size;
  return 1;
}

static bool pb_number(const unsigned char *data, size_t length,
                      size_t *position, uint64_t *value) {
  uint64_t number = 0;
  for (unsigned i = 0; i < 10; i++) {
    if (*position >= length) return false;
    unsigned char byte = data[(*position)++];
    if (i == 9 && byte > 1) return false;
    number |= (uint64_t)(byte & 127u) << (7 * i);
    if (!(byte & 128u)) { *value = number; return true; }
  }
  return false;
}

bool sabr_decode_media_header(const unsigned char *data, size_t length,
                              SabrMediaHeader *out) {
  if (!data || !out || length > 4096) return false;
  SabrMediaHeader result = {0};
  bool have_id = false, have_itag = false;
  for (size_t position = 0; position < length;) {
    uint64_t tag, value;
    if (!pb_number(data, length, &position, &tag) || !(tag >> 3)) return false;
    uint32_t field = (uint32_t)(tag >> 3), wire = (uint32_t)(tag & 7);
    if (wire == 0) {
      if (!pb_number(data, length, &position, &value)) return false;
      switch (field) {
      case 1: if (value > UINT32_MAX) return false;
              result.header_id = (uint32_t)value; have_id = true; break;
      case 3: if (value > UINT32_MAX) return false;
              result.itag = (uint32_t)value; have_itag = true; break;
      case 8: result.is_initialization = value != 0; break;
      case 9: if (value > UINT32_MAX) return false;
              result.segment_number = (uint32_t)value; break;
      case 11: result.start_ms = value; break;
      case 12: result.duration_ms = value; break;
      case 14: result.segment_length = value; break;
      default: break;
      }
    } else if (wire == 2) {
      if (!pb_number(data, length, &position, &value) ||
          value > length - position) return false;
      if (field == 2) {
        if (value == 0 || value >= sizeof(result.video_id) ||
            memchr(data + position, '\0', (size_t)value)) return false;
        memcpy(result.video_id, data + position, (size_t)value);
        result.video_id[value] = '\0';
      }
      position += (size_t)value;
    } else if (wire == 1 || wire == 5) {
      size_t bytes = wire == 1 ? 8 : 4;
      if (bytes > length - position) return false;
      position += bytes;
    } else return false;
  }
  if (!have_id || !have_itag || !result.video_id[0]) return false;
  *out = result;
  return true;
}

static void raw(Writer *w, const void *data, size_t length) {
  if (!w->good || length > w->capacity - w->length) { w->good = false; return; }
  memcpy(w->data + w->length, data, length);
  w->length += length;
}

static void number(Writer *w, uint64_t value) {
  unsigned char bytes[10];
  size_t count = 0;
  do {
    unsigned char byte = value & 127u;
    value >>= 7;
    bytes[count++] = byte | (value ? 128u : 0u);
  } while (value);
  raw(w, bytes, count);
}

static void field_number(Writer *w, uint32_t field, uint64_t value) {
  number(w, (uint64_t)field * 8);
  number(w, value);
}

static void field_bytes(Writer *w, uint32_t field,
                        const void *data, size_t length) {
  number(w, (uint64_t)field * 8 + 2);
  number(w, length);
  raw(w, data, length);
}

bool sabr_encode_request(uint32_t video_itag, uint32_t audio_itag,
                         uint32_t height, uint64_t player_ms,
                         const unsigned char *config, size_t config_length,
                         unsigned char *out, size_t capacity,
                         size_t *out_length) {
  if (!out_length) return false;
  *out_length = 0;
  if (!out || !config || !video_itag || !audio_itag || !height ||
      height > 4320 || !config_length || config_length > 32768) return false;
  unsigned char abr[128], video[16], audio[16], client[128], context[160];
  Writer a = {abr, 0, sizeof(abr), true};
  field_number(&a, 21, height);
  field_number(&a, 28, player_ms);
  field_number(&a, 34, 1);
  number(&a, 35 * 8 + 5);
  static const unsigned char one_float[] = {0, 0, 0x80, 0x3f};
  raw(&a, one_float, sizeof(one_float));
  field_number(&a, 40, 0);
  Writer v = {video, 0, sizeof(video), true};
  Writer au = {audio, 0, sizeof(audio), true};
  field_number(&v, 1, video_itag);
  field_number(&au, 1, audio_itag);
  Writer c = {client, 0, sizeof(client), true};
  field_number(&c, 16, 3); // Android player response supplies this stream.
  field_bytes(&c, 17, "21.26.364", sizeof("21.26.364") - 1);
  field_bytes(&c, 18, "Android", sizeof("Android") - 1);
  field_bytes(&c, 19, "11", sizeof("11") - 1);
  Writer s = {context, 0, sizeof(context), true};
  field_bytes(&s, 1, c.data, c.length);
  Writer w = {out, 0, capacity, true};
  if (a.good && v.good && au.good && c.good && s.good) {
    field_bytes(&w, 1, a.data, a.length);
    field_bytes(&w, 5, config, config_length);
    field_bytes(&w, 16, au.data, au.length);
    field_bytes(&w, 17, v.data, v.length);
    field_bytes(&w, 19, s.data, s.length);
  }
  if (!a.good || !v.good || !au.good || !c.good || !s.good || !w.good)
    return false;
  *out_length = w.length;
  return true;
}
