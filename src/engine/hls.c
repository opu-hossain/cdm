// SPDX-License-Identifier: MIT
#include "hls.h"

#include <curl/curl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* RFC 8216 sections 4 and 5. Parser state belongs to the caller's invocation.
 */
#define HLS_MAX_LINE 8192
#define HLS_MAX_ATTRIBUTES 32

typedef struct {
  char *name, *value;
  bool quoted;
} Attribute;
typedef struct {
  Attribute items[HLS_MAX_ATTRIBUTES];
  size_t count;
} Attributes;

static void message(char *out, size_t size, const char *value) {
  if (!out || !size)
    return;
  size_t length = strlen(value);
  if (length >= size)
    length = size - 1;
  memcpy(out, value, length);
  out[length] = '\0';
}

void hls_playlist_free(HlsPlaylist *p) {
  if (!p)
    return;
  free(p->segments);
  free(p->maps);
  memset(p, 0, sizeof(*p));
}

static bool utf8_text(const unsigned char *s, size_t length) {
  for (size_t i = 0; i < length;) {
    unsigned char c = s[i++];
    if (c < 0x80) {
      if ((c < 0x20 && c != '\n' && c != '\r') || c == 0x7f)
        return false;
      continue;
    }
    unsigned int value, remaining, minimum;
    if (c >= 0xc2 && c <= 0xdf) {
      value = c & 31u;
      remaining = 1;
      minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      value = c & 15u;
      remaining = 2;
      minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      value = c & 7u;
      remaining = 3;
      minimum = 0x10000;
    } else
      return false;
    if (remaining > length - i)
      return false;
    while (remaining--) {
      c = s[i++];
      if ((c & 0xc0) != 0x80)
        return false;
      value = (value << 6) | (c & 63u);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
      return false;
  }
  return true;
}

static bool integer(const char *s, uint64_t *out) {
  if (!*s)
    return false;
  uint64_t value = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9')
      return false;
    unsigned int digit = (unsigned int)(*s - '0');
    if (value > (UINT64_MAX - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

static bool duration(char *s, double *out) {
  char *comma = strchr(s, ',');
  if (!comma)
    return false;
  *comma = '\0';
  if (*s < '0' || *s > '9')
    return false;
  double value = 0, scale = 0.1;
  bool fraction = false, fraction_digit = false;
  for (; *s; s++) {
    if (*s == '.' && !fraction) {
      fraction = true;
      continue;
    }
    if (*s < '0' || *s > '9')
      return false;
    if (fraction) {
      value += (*s - '0') * scale;
      scale *= 0.1;
      fraction_digit = true;
    } else
      value = value * 10 + (*s - '0');
  }
  if (!isfinite(value) || value <= 0 || (fraction && !fraction_digit))
    return false;
  *out = value;
  return true;
}

/* Attribute parsing preserves commas inside quoted strings and rejects
 * duplicate names, empty values, malformed quotes and trailing commas. */
static bool attributes(char *s, Attributes *out) {
  memset(out, 0, sizeof(*out));
  while (*s) {
    if (out->count == HLS_MAX_ATTRIBUTES)
      return false;
    Attribute *a = &out->items[out->count];
    a->name = s;
    while ((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-')
      s++;
    if (s == a->name || *s != '=')
      return false;
    *s++ = '\0';
    for (size_t i = 0; i < out->count; i++)
      if (strcmp(a->name, out->items[i].name) == 0)
        return false;
    a->quoted = *s == '"';
    if (a->quoted) {
      a->value = ++s;
      while (*s && *s != '"')
        s++;
      if (*s != '"')
        return false;
      *s++ = '\0';
      if (*s && *s != ',')
        return false;
    } else {
      a->value = s;
      while (*s && *s != ',') {
        if (*s == '"' || *s == ' ' || *s == '\t')
          return false;
        s++;
      }
    }
    if (a->value == s || !*a->value)
      return false;
    if (*s == ',') {
      *s++ = '\0';
      if (!*s)
        return false;
    }
    out->count++;
  }
  return out->count > 0;
}

static const Attribute *attribute(const Attributes *a, const char *name) {
  for (size_t i = 0; i < a->count; i++)
    if (strcmp(a->items[i].name, name) == 0)
      return &a->items[i];
  return NULL;
}

/* curl's URL API resolves relative references against an existing URL, with
 * no transfer: https://curl.se/libcurl/c/curl_url_set.html */
static bool resolve(const char *base, const char *reference, char *out) {
  CURLU *url = curl_url();
  if (!url)
    return false;
  bool ok = curl_url_set(url, CURLUPART_URL, base, 0) == CURLUE_OK &&
            curl_url_set(url, CURLUPART_URL, reference, 0) == CURLUE_OK;
  char *scheme = NULL, *resolved = NULL;
  if (ok)
    ok = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
         (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0);
  if (ok)
    ok = curl_url_set(url, CURLUPART_FRAGMENT, NULL, 0) == CURLUE_OK &&
         curl_url_get(url, CURLUPART_URL, &resolved, 0) == CURLUE_OK &&
         strlen(resolved) < HLS_URL_MAX;
  if (ok)
    strcpy(out, resolved);
  curl_free(scheme);
  curl_free(resolved);
  curl_url_cleanup(url);
  return ok;
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static bool parse_iv(const char *s, unsigned char *iv) {
  if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
    return false;
  s += 2;
  size_t digits = strlen(s);
  if (!digits || digits > 32)
    return false;
  memset(iv, 0, 16);
  for (size_t i = 0; i < digits; i++) {
    int digit = hex_digit(s[i]);
    if (digit < 0)
      return false;
    size_t nibble = 32 - digits + i;
    iv[nibble / 2] |=
        (unsigned char)((unsigned int)digit << (nibble % 2 ? 0 : 4));
  }
  return true;
}

static HlsResult parse_key(char *value, const char *base, HlsKey *key,
                           bool *explicit_iv) {
  Attributes attrs;
  if (!attributes(value, &attrs))
    return HLS_INVALID;
  const Attribute *method = attribute(&attrs, "METHOD");
  if (!method || method->quoted)
    return HLS_INVALID;
  if (strcmp(method->value, "NONE") == 0) {
    if (attrs.count != 1)
      return HLS_INVALID;
    memset(key, 0, sizeof(*key));
    *explicit_iv = false;
    return HLS_OK;
  }
  if (strcmp(method->value, "AES-128") != 0)
    return HLS_UNSUPPORTED;
  const Attribute *uri = attribute(&attrs, "URI"),
                  *iv = attribute(&attrs, "IV");
  const Attribute *format = attribute(&attrs, "KEYFORMAT");
  const Attribute *versions = attribute(&attrs, "KEYFORMATVERSIONS");
  if (format && (!format->quoted || strcmp(format->value, "identity") != 0))
    return HLS_UNSUPPORTED;
  if (versions && (!versions->quoted || strcmp(versions->value, "1") != 0))
    return HLS_UNSUPPORTED;
  if (!uri || !uri->quoted || !resolve(base, uri->value, key->url))
    return HLS_INVALID;
  *explicit_iv = iv != NULL;
  if (iv && (iv->quoted || !parse_iv(iv->value, key->iv)))
    return HLS_INVALID;
  if (!iv)
    memset(key->iv, 0, sizeof(key->iv));
  key->encrypted = true;
  return HLS_OK;
}

HlsResult hls_parse(const char *text, size_t length, const char *base_url,
                    HlsPlaylist *out, char *error, size_t error_size) {
  if (out)
    memset(out, 0, sizeof(*out));
  message(error, error_size, "");
  if (!text || !base_url || !out || !length) {
    message(error, error_size, "Invalid parser argument");
    return HLS_INVALID;
  }
  if (length > HLS_MAX_PLAYLIST_BYTES) {
    message(error, error_size, "Playlist byte limit exceeded");
    return HLS_LIMIT;
  }
  char base[HLS_URL_MAX];
  if (!utf8_text((const unsigned char *)text, length) ||
      !resolve(base_url, base_url, base)) {
    message(error, error_size, "Invalid UTF-8 or base URL");
    return HLS_INVALID;
  }
  char *copy = malloc(length + 1);
  if (!copy) {
    message(error, error_size, "Out of memory");
    return HLS_NO_MEMORY;
  }
  memcpy(copy, text, length);
  copy[length] = '\0';
  HlsPlaylist p = {.version = 1};
  HlsKey key = {0};
  bool explicit_iv = false, header = false, media_tags = false;
  bool version_seen = false, target_seen = false, sequence_seen = false;
  bool pending_segment = false, pending_variant = false;
  uint64_t bandwidth = 0;
  double segment_duration = 0;
  size_t map_index = HLS_NO_MAP, segment_capacity = 0;
  HlsResult result = HLS_OK;
  const char *reason = "Malformed HLS playlist";
  char *cursor = copy;
  while (*cursor) {
    char *line = cursor, *newline = strchr(cursor, '\n');
    if (newline) {
      *newline = '\0';
      cursor = newline + 1;
    } else
      cursor += strlen(cursor);
    size_t line_length = strlen(line);
    if (line_length && line[line_length - 1] == '\r')
      line[--line_length] = '\0';
    if (line_length > HLS_MAX_LINE) {
      result = HLS_LIMIT;
      reason = "Playlist line limit exceeded";
      break;
    }
    if (strchr(line, '\r')) {
      result = HLS_INVALID;
      break;
    }
    if (!header) {
      if (strcmp(line, "#EXTM3U") != 0) {
        result = HLS_INVALID;
        break;
      }
      header = true;
      continue;
    }
    if (!*line)
      continue;
    if (*line != '#') {
      if (pending_variant) {
        char resolved[HLS_URL_MAX];
        if (!resolve(base, line, resolved)) {
          result = HLS_INVALID;
          break;
        }
        if (!p.selected_url[0] || bandwidth > p.selected_bandwidth) {
          strcpy(p.selected_url, resolved);
          p.selected_bandwidth = bandwidth;
        }
        pending_variant = false;
      } else if (pending_segment) {
        if (p.segment_count == HLS_MAX_SEGMENTS) {
          result = HLS_LIMIT;
          reason = "Segment count limit exceeded";
          break;
        }
        if (p.segment_count > UINT64_MAX - p.media_sequence) {
          result = HLS_INVALID;
          break;
        }
        if (p.segment_count == segment_capacity) {
          size_t capacity = segment_capacity ? segment_capacity * 2 : 16;
          HlsSegment *segments =
              realloc(p.segments, capacity * sizeof(*segments));
          if (!segments) {
            result = HLS_NO_MEMORY;
            break;
          }
          p.segments = segments;
          segment_capacity = capacity;
        }
        HlsSegment *segment = &p.segments[p.segment_count];
        memset(segment, 0, sizeof(*segment));
        if (!resolve(base, line, segment->url)) {
          result = HLS_INVALID;
          break;
        }
        segment->duration = segment_duration;
        segment->sequence = p.media_sequence + p.segment_count;
        segment->map_index = map_index;
        segment->key = key;
        if (key.encrypted && !explicit_iv) {
          uint64_t sequence = segment->sequence;
          for (size_t i = 0; i < 8; i++) {
            segment->key.iv[15 - i] = (unsigned char)(sequence & 255u);
            sequence >>= 8;
          }
        }
        p.segment_count++;
        pending_segment = false;
      } else {
        result = HLS_INVALID;
        break;
      }
      continue;
    }
    char *value = strchr(line, ':');
    if (value)
      *value++ = '\0';
    if (strcmp(line, "#EXTM3U") == 0) {
      result = HLS_INVALID;
      break;
    }
    if (strcmp(line, "#EXT-X-VERSION") == 0) {
      if (version_seen || !value || !integer(value, &p.version) || !p.version) {
        result = HLS_INVALID;
        break;
      }
      version_seen = true;
    } else if (strcmp(line, "#EXT-X-STREAM-INF") == 0) {
      Attributes attrs;
      if (media_tags || pending_variant || !value ||
          !attributes(value, &attrs)) {
        result = HLS_INVALID;
        break;
      }
      const Attribute *bw = attribute(&attrs, "BANDWIDTH");
      if (!bw || bw->quoted || !integer(bw->value, &bandwidth) || !bandwidth) {
        result = HLS_INVALID;
        break;
      }
      pending_variant = true;
      p.is_master = true;
    } else if (strcmp(line, "#EXTINF") == 0) {
      if (p.is_master || pending_segment || !value ||
          !duration(value, &segment_duration)) {
        result = HLS_INVALID;
        break;
      }
      pending_segment = true;
      media_tags = true;
    } else if (strcmp(line, "#EXT-X-TARGETDURATION") == 0) {
      if (p.is_master || target_seen || !value ||
          !integer(value, &p.target_duration) || !p.target_duration) {
        result = HLS_INVALID;
        break;
      }
      target_seen = true;
      media_tags = true;
    } else if (strcmp(line, "#EXT-X-MEDIA-SEQUENCE") == 0) {
      if (p.is_master || sequence_seen || p.segment_count || !value ||
          !integer(value, &p.media_sequence)) {
        result = HLS_INVALID;
        break;
      }
      sequence_seen = true;
      media_tags = true;
    } else if (strcmp(line, "#EXT-X-KEY") == 0) {
      if (p.is_master || !value) {
        result = HLS_INVALID;
        break;
      }
      result = parse_key(value, base, &key, &explicit_iv);
      if (result != HLS_OK) {
        reason = result == HLS_UNSUPPORTED
                     ? "Unsupported encryption or key format"
                     : reason;
        break;
      }
      media_tags = true;
    } else if (strcmp(line, "#EXT-X-MAP") == 0) {
      Attributes attrs;
      if (p.is_master || !value || !attributes(value, &attrs)) {
        result = HLS_INVALID;
        break;
      }
      if (attribute(&attrs, "BYTERANGE")) {
        result = HLS_UNSUPPORTED;
        reason = "Initialization byte ranges are unsupported";
        break;
      }
      const Attribute *uri = attribute(&attrs, "URI");
      HlsMap map = {.key = key};
      if (!uri || !uri->quoted || !resolve(base, uri->value, map.url) ||
          (key.encrypted && !explicit_iv)) {
        result = HLS_INVALID;
        break;
      }
      if (p.map_count == HLS_MAX_MAPS) {
        result = HLS_LIMIT;
        reason = "Initialization map limit exceeded";
        break;
      }
      HlsMap *maps = realloc(p.maps, (p.map_count + 1) * sizeof(*maps));
      if (!maps) {
        result = HLS_NO_MEMORY;
        break;
      }
      p.maps = maps;
      map_index = p.map_count;
      p.maps[p.map_count++] = map;
      media_tags = true;
    } else if (strcmp(line, "#EXT-X-ENDLIST") == 0) {
      if (p.is_master || value) {
        result = HLS_INVALID;
        break;
      }
      p.end_list = true;
      media_tags = true;
    } else if (strcmp(line, "#EXT-X-BYTERANGE") == 0 ||
               strcmp(line, "#EXT-X-DISCONTINUITY") == 0 ||
               strcmp(line, "#EXT-X-I-FRAMES-ONLY") == 0 ||
               strcmp(line, "#EXT-X-GAP") == 0 ||
               strcmp(line, "#EXT-X-SKIP") == 0) {
      result = HLS_UNSUPPORTED;
      reason = "Unsupported segment layout";
      break;
    }
    /* Ignore comments and unknown advisory tags, as specified by RFC 8216. */
  }
  if (result == HLS_OK) {
    if (!header || pending_segment || pending_variant ||
        (p.is_master ? !p.selected_url[0] : (!target_seen || !p.segment_count)))
      result = HLS_INVALID;
    for (size_t i = 0; result == HLS_OK && i < p.segment_count; i++)
      if ((long double)p.segments[i].duration >=
          (long double)p.target_duration + 0.5L)
        result = HLS_INVALID;
  }
  free(copy);
  if (result != HLS_OK) {
    hls_playlist_free(&p);
    if (result == HLS_NO_MEMORY)
      reason = "Out of memory";
    message(error, error_size, reason);
  } else
    *out = p;
  return result;
}
