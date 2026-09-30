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

int sabr_protection_status(const unsigned char *ump, size_t length) {
  if (!ump) return -1;
  int status = 0;
  size_t offset = 0;
  while (offset < length) {
    SabrPart part;
    if (sabr_read_part(ump, length, &offset, &part) != 1) return -1;
    if (part.type != 58) continue;
    if (part.length < 2 || part.data[0] != 0x08 ||
        part.data[1] < 1 || part.data[1] > 3) return -1;
    status = part.data[1];
  }
  return status;
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

static bool pb_format_itag(const unsigned char *data, size_t length,
                           uint32_t *itag) {
  for (size_t position = 0; position < length;) {
    uint64_t tag, value;
    if (!pb_number(data, length, &position, &tag) || !(tag >> 3)) return false;
    if ((tag & 7) == 0) {
      if (!pb_number(data, length, &position, &value)) return false;
      if ((tag >> 3) == 1) {
        if (value > UINT32_MAX) return false;
        *itag = (uint32_t)value;
      }
    } else if ((tag & 7) == 2) {
      if (!pb_number(data, length, &position, &value) ||
          value > length - position) return false;
      position += (size_t)value;
    } else if ((tag & 7) == 1 || (tag & 7) == 5) {
      size_t bytes = (tag & 7) == 1 ? 8 : 4;
      if (bytes > length - position) return false;
      position += bytes;
    } else return false;
  }
  return true;
}

typedef struct {
  uint32_t number, wire;
  uint64_t integer;
  const unsigned char *bytes;
  size_t length;
} PbField;

static bool pb_next_field(const unsigned char *data, size_t length,
                          size_t *position, PbField *out) {
  uint64_t tag, value;
  if (!pb_number(data, length, position, &tag) || !tag ||
      tag >> 3 > 1024) return false;
  PbField field = {.number = (uint32_t)(tag >> 3),
                   .wire = (uint32_t)(tag & 7)};
  if (field.wire == 0) {
    if (!pb_number(data, length, position, &field.integer)) return false;
  } else if (field.wire == 2) {
    if (!pb_number(data, length, position, &value) ||
        value > length - *position) return false;
    field.bytes = data + *position;
    field.length = (size_t)value;
    *position += (size_t)value;
  } else if (field.wire == 1 || field.wire == 5) {
    size_t count = field.wire == 1 ? 8 : 4;
    if (count > length - *position) return false;
    field.bytes = data + *position;
    field.length = count;
    *position += count;
  } else return false;
  *out = field;
  return true;
}

bool sabr_parse_captured_request(const unsigned char *data, size_t length,
                                 uint32_t video_itag,
                                 SabrCapturedRequest *out) {
  if (!data || !out || !length || length > 16000 ||
      !video_itag || video_itag > 100000) return false;
  SabrCapturedRequest parsed = {0};
  for (size_t position = 0; position < length;) {
    PbField field;
    if (!pb_next_field(data, length, &position, &field)) return false;
    if (field.wire != 2) continue;
    if (field.number == 5 && field.length &&
        field.length <= sizeof(parsed.config)) {
      memcpy(parsed.config, field.bytes, field.length);
      parsed.config_length = field.length;
    } else if (field.number == 19 && field.length &&
               field.length <= sizeof(parsed.context)) {
      memcpy(parsed.context, field.bytes, field.length);
      parsed.context_length = field.length;
    } else if ((field.number == 16 || field.number == 17) &&
               field.length && field.length <= sizeof(parsed.video_format)) {
      uint32_t itag = 0;
      if (!pb_format_itag(field.bytes, field.length, &itag)) continue;
      if (field.number == 17 && itag == video_itag &&
          !parsed.video_format_length) {
        memcpy(parsed.video_format, field.bytes, field.length);
        parsed.video_format_length = field.length;
      } else if (field.number == 16 &&
                 (itag == 140 || itag == 251 || itag == 250) &&
                 (!parsed.audio_format_length ||
                  (itag == 140 && parsed.audio_itag != 140) ||
                  (itag == 251 && parsed.audio_itag == 250))) {
        memcpy(parsed.audio_format, field.bytes, field.length);
        parsed.audio_format_length = field.length;
        parsed.audio_itag = itag;
      }
    }
  }
  if (!parsed.config_length || !parsed.context_length ||
      !parsed.audio_format_length) return false;
  bool have_token = false;
  for (size_t position = 0; position < parsed.context_length;) {
    PbField field;
    if (!pb_next_field(parsed.context, parsed.context_length,
                       &position, &field)) return false;
    if (field.number == 2 && field.wire == 2 &&
        field.length >= 32 && field.length <= 1024) have_token = true;
  }
  if (!have_token) return false;
  *out = parsed;
  return true;
}

bool sabr_decode_format_metadata(const unsigned char *data, size_t length,
                                 SabrFormatMetadata *out) {
  if (!data || !out || length > 8192) return false;
  SabrFormatMetadata result = {0};
  for (size_t position = 0; position < length;) {
    uint64_t tag, value;
    if (!pb_number(data, length, &position, &tag) || !(tag >> 3)) return false;
    if ((tag & 7) == 0) {
      if (!pb_number(data, length, &position, &value)) return false;
      if ((tag >> 3) == 4) {
        if (value > UINT32_MAX) return false;
        result.end_segment = (uint32_t)value;
      }
    } else if ((tag & 7) == 2) {
      if (!pb_number(data, length, &position, &value) ||
          value > length - position) return false;
      if ((tag >> 3) == 1) {
        if (!value || value >= sizeof(result.video_id) ||
            memchr(data + position, '\0', (size_t)value)) return false;
        memcpy(result.video_id, data + position, (size_t)value);
        result.video_id[value] = '\0';
      } else if ((tag >> 3) == 2 &&
                 !pb_format_itag(data + position, (size_t)value,
                                 &result.itag)) return false;
      position += (size_t)value;
    } else if ((tag & 7) == 1 || (tag & 7) == 5) {
      size_t bytes = (tag & 7) == 1 ? 8 : 4;
      if (bytes > length - position) return false;
      position += bytes;
    } else return false;
  }
  if (!result.video_id[0] || !result.itag || !result.end_segment) return false;
  *out = result;
  return true;
}

static bool pb_time_range(const unsigned char *data, size_t length,
                          uint64_t *end_ms) {
  uint64_t start = 0, duration = 0, timescale = 0;
  for (size_t position = 0; position < length;) {
    uint64_t tag, value;
    if (!pb_number(data, length, &position, &tag) || !(tag >> 3)) return false;
    if ((tag & 7) != 0 ||
        !pb_number(data, length, &position, &value)) return false;
    if ((tag >> 3) == 1) start = value;
    if ((tag >> 3) == 2) duration = value;
    if ((tag >> 3) == 3) timescale = value;
  }
  if (!timescale || timescale > 1000000000 ||
      start > 1000000000000ULL || duration > 1000000000000ULL ||
      duration > 1000000000000ULL - start) return false;
  *end_ms = (start + duration) * 1000 / timescale;
  return true;
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
      } else if (field == 15 &&
                 !pb_time_range(data + position, (size_t)value,
                                &result.end_ms)) return false;
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

bool sabr_update_playback_context(SabrCapturedRequest *captured,
                                  const unsigned char *ump, size_t length) {
  if (!captured || !ump || !captured->context_length) return false;
  const unsigned char *cookie = NULL;
  size_t cookie_length = 0, offset = 0;
  while (offset < length) {
    SabrPart part;
    if (sabr_read_part(ump, length, &offset, &part) != 1) return false;
    if (part.type != 35) continue;
    for (size_t position = 0; position < part.length;) {
      PbField field;
      if (!pb_next_field(part.data, part.length, &position, &field))
        return false;
      if (field.number == 7 && field.wire == 2 && field.length > 0 &&
          field.length <= 1024) {
        cookie = field.bytes;
        cookie_length = field.length;
      }
    }
  }
  if (!cookie) return true;
  unsigned char next[sizeof(captured->context)];
  Writer writer = {next, 0, sizeof(next), true};
  for (size_t position = 0; position < captured->context_length;) {
    size_t start = position;
    PbField field;
    if (!pb_next_field(captured->context, captured->context_length,
                       &position, &field)) return false;
    if (field.number != 3)
      raw(&writer, captured->context + start, position - start);
  }
  field_bytes(&writer, 3, cookie, cookie_length);
  if (!writer.good) return false;
  memcpy(captured->context, next, writer.length);
  captured->context_length = writer.length;
  return true;
}

static void format_id(Writer *w, const SabrFormatId *format) {
  if (format->raw) {
    if (!format->raw_length || format->raw_length > 560) {
      w->good = false;
      return;
    }
    raw(w, format->raw, format->raw_length);
    return;
  }
  field_number(w, 1, format->itag);
  if (format->last_modified)
    field_number(w, 2, format->last_modified);
  if (format->xtags && format->xtags[0]) {
    size_t length = strnlen(format->xtags, 513);
    if (length > 512) { w->good = false; return; }
    field_bytes(w, 3, format->xtags, length);
  }
}

bool sabr_encode_request_with_context(const SabrFormatId *video_format,
                         const SabrFormatId *audio_format,
                         uint32_t height, uint64_t player_ms,
                         const unsigned char *config, size_t config_length,
                         const SabrBufferedRange *ranges, size_t range_count,
                         const unsigned char *browser_context,
                         size_t browser_context_length,
                         unsigned char *out, size_t capacity,
                         size_t *out_length) {
  if (!out_length) return false;
  *out_length = 0;
  if (!out || !config || !video_format || !audio_format ||
      !video_format->itag || !audio_format->itag || !height ||
      height > 4320 || !config_length || config_length > 32768 ||
      range_count > 2 || (range_count && !ranges) ||
      browser_context_length > 2048 ||
      (browser_context_length && !browser_context)) return false;
  unsigned char abr[128], video[560], audio[560], client[128], context[160];
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
  format_id(&v, video_format);
  format_id(&au, audio_format);
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
    for (size_t i = 0; i < range_count; i++) {
      const SabrBufferedRange *range = &ranges[i];
      if (!range->format.itag || !range->end_segment || !range->end_ms)
        return false;
      unsigned char fid[560], encoded[640];
      Writer f = {fid, 0, sizeof(fid), true};
      Writer r = {encoded, 0, sizeof(encoded), true};
      format_id(&f, &range->format);
      field_bytes(&r, 1, f.data, f.length);
      field_number(&r, 2, 0);
      field_number(&r, 3, range->end_ms);
      field_number(&r, 4, 1);
      field_number(&r, 5, range->end_segment);
      if (!f.good || !r.good) return false;
      field_bytes(&w, 3, r.data, r.length);
    }
    field_bytes(&w, 5, config, config_length);
    field_bytes(&w, 16, au.data, au.length);
    field_bytes(&w, 17, v.data, v.length);
    field_bytes(&w, 19,
        browser_context_length ? browser_context : s.data,
        browser_context_length ? browser_context_length : s.length);
  }
  if (!a.good || !v.good || !au.good || !c.good || !s.good || !w.good)
    return false;
  *out_length = w.length;
  return true;
}

bool sabr_encode_request_with_ranges(const SabrFormatId *video,
    const SabrFormatId *audio, uint32_t height, uint64_t player_ms,
    const unsigned char *config, size_t config_length,
    const SabrBufferedRange *ranges, size_t range_count,
    unsigned char *out, size_t capacity, size_t *out_length) {
  return sabr_encode_request_with_context(video, audio, height, player_ms,
      config, config_length, ranges, range_count, NULL, 0,
      out, capacity, out_length);
}

bool sabr_encode_request(const SabrFormatId *video, const SabrFormatId *audio,
                         uint32_t height, uint64_t player_ms,
                         const unsigned char *config, size_t config_length,
                         unsigned char *out, size_t capacity,
                         size_t *out_length) {
  return sabr_encode_request_with_ranges(video, audio, height, player_ms,
      config, config_length, NULL, 0, out, capacity, out_length);
}
