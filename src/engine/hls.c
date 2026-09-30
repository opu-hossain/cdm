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

/* VOD transfer implementation. Each outer job owns one file and invokes the
 * existing byte worker pool with a single whole-file slot. The orchestrator
 * alone reads/writes resume JSON after joining the bounded job batch. */
#include "../core/queue_manager.h"
#include "../persistence/db.h"
#include "../platform/bandwidth.h"
#include "../platform/curl_client.h"
#include "../platform/file_io.h"
#include "../platform/spawn.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../utils/path.h"
#include "worker_pool.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>

#ifndef _WIN32
#include "../vendor/cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define HLS_PATH_MAX 1200
#define HLS_STATE_MAX (4 * 1024 * 1024)

typedef struct {
  unsigned char *data;
  size_t size, limit;
  Download *download;
} HlsBody;
typedef struct {
  bool valid;
  char sha256[65], key_sha256[65], etag[256], modified[128];
} HlsSaved;
typedef struct {
  Download *download;
  const char *url;
  const HlsKey *key;
  const RequestContext *context;
  DownloadManagerConfig config;
  char path[HLS_PATH_MAX];
  HlsSaved saved;
  uint64_t speed_limit;
  bool success;
} HlsJob;

static bool stopped(const Download *d) {
  return atomic_load(&d->cancel_requested) || atomic_load(&d->pause_requested);
}

static size_t body_write(char *data, size_t size, size_t count, void *arg) {
  HlsBody *body = arg;
  if (size && count > SIZE_MAX / size)
    return 0;
  size_t bytes = size * count;
  if (stopped(body->download) || bytes > body->limit - body->size)
    return 0;
  if (!bandwidth_acquire(bytes, &body->download->cancel_requested,
                         &body->download->pause_requested))
    return 0;
  memcpy(body->data + body->size, data, bytes);
  body->size += bytes;
  return bytes;
}

static int body_progress(void *arg, curl_off_t total, curl_off_t now,
                         curl_off_t upload_total, curl_off_t uploaded) {
  (void)total;
  (void)now;
  (void)upload_total;
  (void)uploaded;
  return stopped(arg) ? 1 : 0;
}

static bool same_origin(const char *a, const char *b) {
  CURLU *left = curl_url(), *right = curl_url();
  bool equal = left && right &&
               curl_url_set(left, CURLUPART_URL, a, 0) == CURLUE_OK &&
               curl_url_set(right, CURLUPART_URL, b, 0) == CURLUE_OK;
  CURLUPart parts[] = {CURLUPART_SCHEME, CURLUPART_HOST, CURLUPART_PORT};
  for (size_t i = 0; equal && i < 3; i++) {
    char *x = NULL, *y = NULL;
    unsigned int flags = parts[i] == CURLUPART_PORT ? CURLU_DEFAULT_PORT : 0;
    equal = curl_url_get(left, parts[i], &x, flags) == CURLUE_OK &&
            curl_url_get(right, parts[i], &y, flags) == CURLUE_OK &&
            strcasecmp(x, y) == 0;
    curl_free(x);
    curl_free(y);
  }
  curl_url_cleanup(left);
  curl_url_cleanup(right);
  return equal;
}

static bool context_allows(const Download *d, const RequestContext *ctx,
                           const char *url) {
  return !ctx->sensitive || same_origin(d->url, url);
}

/* GET bodies are bounded even when Content-Length is absent or gzip expands. */
static bool fetch_body(Download *d, const RequestContext *ctx, const char *url,
                       HlsBody *body, char *effective) {
  if (!context_allows(d, ctx, url) || stopped(d))
    return false;
  CURL *curl = curl_easy_init();
  if (!curl)
    return false;
  DownloadManagerConfig config;
  config_get(&config);
  struct curl_slist *headers = curl_client_build_headers(ctx->extra_headers);
  bool ok = (!ctx->extra_headers || !*ctx->extra_headers || headers) &&
            curl_apply_proxy(curl, &config) == CURLE_OK &&
            curl_apply_basic_auth(curl, ctx) == CURLE_OK;
#define HLS_SET(option, value)                                                 \
  do {                                                                         \
    if (curl_easy_setopt(curl, option, value) != CURLE_OK)                     \
      ok = false;                                                              \
  } while (0)
  HLS_SET(CURLOPT_URL, url);
  HLS_SET(CURLOPT_PROTOCOLS_STR, "http,https");
  HLS_SET(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  HLS_SET(CURLOPT_FOLLOWLOCATION, ctx->sensitive ? 0L : 1L);
  HLS_SET(CURLOPT_MAXREDIRS, 10L);
  HLS_SET(CURLOPT_NOSIGNAL, 1L);
  HLS_SET(CURLOPT_FAILONERROR, 1L);
  HLS_SET(CURLOPT_TIMEOUT, (long)config.transfer_timeout_sec);
  HLS_SET(CURLOPT_CONNECTTIMEOUT, (long)config.connect_timeout_sec);
  HLS_SET(CURLOPT_USERAGENT,
          ctx->user_agent ? ctx->user_agent : config.user_agent);
  HLS_SET(CURLOPT_WRITEFUNCTION, body_write);
  HLS_SET(CURLOPT_WRITEDATA, body);
  HLS_SET(CURLOPT_XFERINFOFUNCTION, body_progress);
  HLS_SET(CURLOPT_XFERINFODATA, d);
  HLS_SET(CURLOPT_NOPROGRESS, 0L);
  HLS_SET(CURLOPT_ACCEPT_ENCODING, "");
  if (ctx->cookie)
    HLS_SET(CURLOPT_COOKIE, ctx->cookie);
  if (ctx->referrer)
    HLS_SET(CURLOPT_REFERER, ctx->referrer);
  if (headers)
    HLS_SET(CURLOPT_HTTPHEADER, headers);
#undef HLS_SET
  long status = 0;
  char *final = NULL;
  ok = ok && curl_easy_perform(curl) == CURLE_OK &&
       curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
       status == 200;
  if (ok && effective) {
    ok = curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final) == CURLE_OK &&
         final && strlen(final) < HLS_URL_MAX;
    if (ok)
      strcpy(effective, final);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return ok;
}

static bool digest_bytes(const void *data, size_t length, char *hex) {
  unsigned char digest[32];
  unsigned int size = 0;
  if (EVP_Digest(data, length, digest, &size, EVP_sha256(), NULL) != 1 ||
      size != 32)
    return false;
  const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    hex[i * 2] = digits[digest[i] >> 4];
    hex[i * 2 + 1] = digits[digest[i] & 15];
  }
  hex[64] = '\0';
  return true;
}

static bool safe_file(int fd) {
  struct stat st;
  return fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
         st.st_uid == getuid() && st.st_nlink == 1;
}

static int create_file(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (!safe_file(fd) || ftruncate(fd, 0) != 0) {
    if (fd >= 0)
      close(fd);
    return -1;
  }
  return fd;
}

static bool digest_file(const char *path, char *hex, uint64_t *bytes) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (!safe_file(fd)) {
    if (fd >= 0)
      close(fd);
    return false;
  }
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
  unsigned char buffer[16384], digest[32];
  unsigned int size = 0;
  uint64_t count = 0;
  while (ok) {
    ssize_t got = read(fd, buffer, sizeof(buffer));
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0) {
      ok = false;
      break;
    }
    if (!got)
      break;
    count += (uint64_t)got;
    ok = EVP_DigestUpdate(ctx, buffer, (size_t)got) == 1;
  }
  ok = ok && EVP_DigestFinal_ex(ctx, digest, &size) == 1 && size == 32;
  if (ok) {
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
      hex[i * 2] = digits[digest[i] >> 4];
      hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = '\0';
    *bytes = count;
  }
  EVP_MD_CTX_free(ctx);
  close(fd);
  return ok;
}

static bool write_all(int fd, const void *data, size_t size) {
  const unsigned char *p = data;
  while (size) {
    ssize_t sent = write(fd, p, size);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      return false;
    p += sent;
    size -= (size_t)sent;
  }
  return true;
}

/* EVP's default padding is PKCS#7, as required for HLS AES-128 CBC. Keys
 * and decrypted buffers remain in memory or the private per-download directory.
 */
static bool decrypt_file(HlsJob *job, const unsigned char *key) {
  char temporary[HLS_PATH_MAX];
  int n = snprintf(temporary, sizeof(temporary), "%s.dec", job->path);
  if (n < 0 || (size_t)n >= sizeof(temporary))
    return false;
  int input = open(job->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC),
      output = create_file(temporary);
  EVP_CIPHER_CTX *cipher = EVP_CIPHER_CTX_new();
  bool ok = safe_file(input) && output >= 0 && cipher &&
            EVP_DecryptInit_ex(cipher, EVP_aes_128_cbc(), NULL, key,
                               job->key->iv) == 1;
  unsigned char data[16384], clear[16384 + EVP_MAX_BLOCK_LENGTH];
  while (ok) {
    if (stopped(job->download)) {
      ok = false;
      break;
    }
    ssize_t got = read(input, data, sizeof(data));
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0) {
      ok = false;
      break;
    }
    if (!got)
      break;
    int length = 0;
    ok = EVP_DecryptUpdate(cipher, clear, &length, data, (int)got) == 1 &&
         write_all(output, clear, (size_t)length);
  }
  int length = 0;
  ok = ok && EVP_DecryptFinal_ex(cipher, clear, &length) == 1 &&
       write_all(output, clear, (size_t)length) && fsync(output) == 0;
  OPENSSL_cleanse(clear, sizeof(clear));
  EVP_CIPHER_CTX_free(cipher);
  if (input >= 0)
    close(input);
  if (output >= 0)
    close(output);
  if (ok)
    ok = rename(temporary, job->path) == 0;
  if (!ok)
    unlink(temporary);
  return ok;
}

static bool retry_delay(HlsJob *job, uint64_t attempt) {
  uint64_t delay = (uint64_t)job->config.retry_base_delay_sec;
  uint64_t cap = (uint64_t)job->config.retry_max_delay_sec;
  for (uint64_t i = 0; i < attempt && delay < cap; i++)
    delay = delay > cap / 2 ? cap : delay * 2;
  if (delay > cap)
    delay = cap;
  for (uint64_t tick = 0; tick < delay * 10; tick++) {
    if (stopped(job->download))
      return false;
    dm_thread_sleep_ms(100);
  }
  return !stopped(job->download);
}

static int segment_job(void *arg) {
  HlsJob *job = arg;
  HlsSaved old = job->saved;
  job->saved.valid = false;
  if (!context_allows(job->download, job->context, job->url) ||
      (job->key->encrypted &&
       !context_allows(job->download, job->context, job->key->url)))
    return -1;
  for (uint64_t attempt = 0;
       attempt <= (uint64_t)job->config.retry_max_attempts; attempt++) {
    if (stopped(job->download) || (attempt && !retry_delay(job, attempt - 1)))
      break;
    unsigned char key[16] = {0};
    char key_digest[65] = "";
    bool ready = true;
    if (job->key->encrypted) {
      HlsBody body = {.data = key, .limit = 16, .download = job->download};
      ready =
          fetch_body(job->download, job->context, job->key->url, &body, NULL) &&
          body.size == 16 && digest_bytes(key, sizeof(key), key_digest);
    }
    FileInfo info = {0};
    ready = ready && curl_client_head(job->url, job->context, &info) == 0;
    const char *validator = info.etag[0] && strncmp(info.etag, "W/", 2) != 0
                                ? info.etag
                                : info.last_modified;
    bool validator_matches =
        info.etag[0] && strncmp(info.etag, "W/", 2) != 0
            ? strcmp(old.etag, info.etag) == 0
            : strcmp(old.modified, info.last_modified) == 0;
    char hash[65];
    uint64_t size = 0;
    if (ready && old.valid && *validator && validator_matches &&
        strcmp(old.key_sha256, key_digest) == 0 &&
        digest_file(job->path, hash, &size) && strcmp(hash, old.sha256) == 0) {
      job->saved = old;
      job->success = true;
      atomic_fetch_add(&job->download->bytes_downloaded, size);
      OPENSSL_cleanse(key, sizeof(key));
      return 0;
    }
    _Atomic uint64_t progress = 0;
    _Atomic uint64_t *slots[] = {&progress};
    int fd = ready ? create_file(job->path) : -1;
    if (fd >= 0)
      close(fd);
    else
      ready = false;
    if (ready) {
      Range range = {.whole_file = true, .unknown_size = true};
      RequestContext transfer = *job->context;
      char headers[4608];
      if (*validator) {
        int n = snprintf(
            headers, sizeof(headers), "%s%s%s: %s",
            transfer.extra_headers ? transfer.extra_headers : "",
            transfer.extra_headers && *transfer.extra_headers ? "\n" : "",
            info.etag[0] && strncmp(info.etag, "W/", 2) != 0
                ? "If-Match"
                : "If-Unmodified-Since",
            validator);
        if (n < 0 || (size_t)n >= sizeof(headers)) {
          OPENSSL_cleanse(key, sizeof(key));
          return -1;
        }
        transfer.extra_headers = headers;
      }
      WorkerPoolResult result = worker_pool_run(
          job->url, &range, 1, job->path, &job->download->bytes_downloaded,
          &job->download->cancel_requested, &job->download->pause_requested,
          slots, job->speed_limit, &transfer, NULL);
      ready = result.all_succeeded &&
              (!info.total_size || atomic_load(&progress) == info.total_size);
    }
    uint64_t received = atomic_load(&progress);
    if (ready && job->key->encrypted)
      ready = decrypt_file(job, key);
    OPENSSL_cleanse(key, sizeof(key));
    ready = ready && digest_file(job->path, hash, &size);
    atomic_fetch_sub(&job->download->bytes_downloaded, received);
    if (ready) {
      strcpy(job->saved.sha256, hash);
      strcpy(job->saved.key_sha256, key_digest);
      strcpy(job->saved.etag, info.etag);
      strcpy(job->saved.modified, info.last_modified);
      job->saved.valid = true;
      job->success = true;
      atomic_fetch_add(&job->download->bytes_downloaded, size);
      return 0;
    }
  }
  return -1;
}

static bool saved_string(cJSON *root, const char *name, char *out,
                         size_t size) {
  cJSON *field = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!cJSON_IsString(field) || strlen(field->valuestring) >= size)
    return false;
  strcpy(out, field->valuestring);
  return true;
}

static void load_state(const char *path, const char *fingerprint,
                       HlsSaved *saved, size_t count) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st;
  if (!safe_file(fd) || fstat(fd, &st) != 0 || st.st_size <= 0 ||
      st.st_size > HLS_STATE_MAX) {
    if (fd >= 0)
      close(fd);
    return;
  }
  size_t size = (size_t)st.st_size;
  char *text = malloc(size + 1);
  if (!text) {
    close(fd);
    return;
  }
  size_t received = 0;
  while (received < size) {
    ssize_t got = read(fd, text + received, size - received);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      break;
    received += (size_t)got;
  }
  close(fd);
  text[received] = '\0';
  cJSON *root =
      received == size && !memchr(text, '\0', size) ? cJSON_Parse(text) : NULL;
  cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
  cJSON *hash = cJSON_GetObjectItemCaseSensitive(root, "manifest");
  cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
  if (cJSON_IsNumber(version) && version->valuedouble == 1 &&
      cJSON_IsString(hash) && strcmp(hash->valuestring, fingerprint) == 0 &&
      cJSON_IsArray(items) && (size_t)cJSON_GetArraySize(items) == count) {
    for (size_t i = 0; i < count; i++) {
      cJSON *item = cJSON_GetArrayItem(items, (int)i);
      saved[i].valid =
          saved_string(item, "sha256", saved[i].sha256,
                       sizeof(saved[i].sha256)) &&
          saved_string(item, "key", saved[i].key_sha256,
                       sizeof(saved[i].key_sha256)) &&
          saved_string(item, "etag", saved[i].etag, sizeof(saved[i].etag)) &&
          saved_string(item, "modified", saved[i].modified,
                       sizeof(saved[i].modified));
    }
  }
  cJSON_Delete(root);
  free(text);
}

static bool save_state(const char *path, const char *fingerprint,
                       HlsSaved *saved, size_t count) {
  cJSON *root = cJSON_CreateObject(), *items = NULL;
  bool ok = root && cJSON_AddNumberToObject(root, "version", 1) &&
            cJSON_AddStringToObject(root, "manifest", fingerprint) &&
            (items = cJSON_AddArrayToObject(root, "items"));
  for (size_t i = 0; ok && i < count; i++) {
    cJSON *item = saved[i].valid ? cJSON_CreateObject() : cJSON_CreateNull();
    if (!item) {
      ok = false;
      break;
    }
    if (saved[i].valid)
      ok = cJSON_AddStringToObject(item, "sha256", saved[i].sha256) &&
           cJSON_AddStringToObject(item, "key", saved[i].key_sha256) &&
           cJSON_AddStringToObject(item, "etag", saved[i].etag) &&
           cJSON_AddStringToObject(item, "modified", saved[i].modified);
    if (!ok || !cJSON_AddItemToArray(items, item)) {
      cJSON_Delete(item);
      ok = false;
    }
  }
  char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
  char temporary[HLS_PATH_MAX];
  int n = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  int fd = text && n >= 0 && (size_t)n < sizeof(temporary)
               ? create_file(temporary)
               : -1;
  ok = fd >= 0 && write_all(fd, text, strlen(text)) && fsync(fd) == 0;
  if (fd >= 0)
    close(fd);
  if (ok)
    ok = rename(temporary, path) == 0;
  if (n >= 0 && (size_t)n < sizeof(temporary) && !ok)
    unlink(temporary);
  cJSON_free(text);
  cJSON_Delete(root);
  return ok;
}

static bool item_path(const char *directory, size_t index, char *out,
                      size_t size) {
  int n = snprintf(out, size, "%s/item-%zu", directory, index);
  return n >= 0 && (size_t)n < size;
}

static bool append_file(int output, const char *path, Download *d) {
  int input = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (!safe_file(input)) {
    if (input >= 0)
      close(input);
    return false;
  }
  unsigned char buffer[16384];
  bool ok = true;
  while (ok && !stopped(d)) {
    ssize_t got = read(input, buffer, sizeof(buffer));
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0) {
      ok = false;
      break;
    }
    if (!got)
      break;
    ok = write_all(output, buffer, (size_t)got);
  }
  close(input);
  return ok && !stopped(d);
}

void hls_discard_state(const char *destination) {
  char directory[HLS_PATH_MAX], state[HLS_PATH_MAX], path[HLS_PATH_MAX];
  int n = snprintf(directory, sizeof(directory), "%s.hlsparts", destination);
  if (n < 0 || (size_t)n >= sizeof(directory))
    return;
  struct stat st;
  if (lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != getuid() || (st.st_mode & 077))
    return;
  n = snprintf(path, sizeof(path), "%s/lock", directory);
  if (n < 0 || (size_t)n >= sizeof(path))
    return;
  int lock = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  if (!safe_file(lock) || flock(lock, LOCK_EX | LOCK_NB) != 0) {
    if (lock >= 0)
      close(lock);
    return;
  }
  for (size_t i = 0; i < HLS_MAX_SEGMENTS + HLS_MAX_MAPS; i++) {
    if (item_path(directory, i, path, sizeof(path)))
      unlink(path);
    n = snprintf(path, sizeof(path), "%s/item-%zu.dec", directory, i);
    if (n >= 0 && (size_t)n < sizeof(path))
      unlink(path);
  }
  n = snprintf(path, sizeof(path), "%s/remux.mp4", directory);
  if (n >= 0 && (size_t)n < sizeof(path))
    unlink(path);
  n = snprintf(path, sizeof(path), "%s/concat", directory);
  if (n >= 0 && (size_t)n < sizeof(path))
    unlink(path);
  n = snprintf(state, sizeof(state), "%s.hlsstate", destination);
  if (n >= 0 && (size_t)n < sizeof(state))
    unlink(state);
  n = snprintf(state, sizeof(state), "%s.hlsstate.tmp", destination);
  if (n >= 0 && (size_t)n < sizeof(state))
    unlink(state);
  n = snprintf(path, sizeof(path), "%s/lock", directory);
  if (n >= 0 && (size_t)n < sizeof(path))
    unlink(path);
  rmdir(directory);
  close(lock);
}

static int remux_output(Download *d, const char *directory) {
  if (!spawn_ffmpeg_available()) {
    LOG_WARN("HLS download %u: ffmpeg unavailable; retained native output",
             d->id);
    return 0;
  }
  if (d->request && d->request->expected_sha256[0]) {
    LOG_INFO("HLS download %u: retain checksum-verified native output", d->id);
    return 0;
  }
  char temporary[HLS_PATH_MAX];
  int n = snprintf(temporary, sizeof(temporary), "%s/remux.mp4", directory);
  if (n < 0 || (size_t)n >= sizeof(temporary))
    return 0;
  int fd = create_file(temporary);
  if (fd < 0)
    return 0;
  close(fd);
  DownloadManagerConfig config;
  config_get(&config);
  int rc = spawn_ffmpeg_remux(d->dest_path, temporary, &d->cancel_requested,
                              &d->pause_requested, config.transfer_timeout_sec);
  if (rc != 0) {
    unlink(temporary);
    if (stopped(d))
      return -1;
    LOG_WARN("HLS download %u: ffmpeg failed (%d); retained native output",
             d->id, rc);
    return 0;
  }
  char hash[65];
  uint64_t bytes = 0;
  if (!digest_file(temporary, hash, &bytes) || !bytes) {
    unlink(temporary);
    LOG_WARN(
        "HLS download %u: empty or unreadable remux; retained native output",
        d->id);
    return 0;
  }
  fd = open(temporary, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  bool durable = safe_file(fd) && fsync(fd) == 0;
  if (fd >= 0)
    close(fd);
  if (!durable || stopped(d)) {
    unlink(temporary);
    return stopped(d) ? -1 : 0;
  }
  char requested[sizeof(d->dest_path)], published[sizeof(d->dest_path)];
  size_t length = strlen(d->dest_path);
  const char *slash = strrchr(d->dest_path, '/'),
             *extension = strrchr(d->dest_path, '.');
  if (extension && (!slash || extension > slash))
    length = (size_t)(extension - d->dest_path);
  if (length + sizeof(".mp4") > sizeof(requested)) {
    unlink(temporary);
    return 0;
  }
  memcpy(requested, d->dest_path, length);
  memcpy(requested + length, ".mp4", sizeof(".mp4"));
  bool linked = false;
  for (int attempt = 0; attempt < 1000000; attempt++) {
    if (!path_make_unique(requested, published, sizeof(published)))
      break;
    if (link(temporary, published) == 0) {
      linked = true;
      break;
    }
    if (errno != EEXIST)
      break;
  }
  if (!linked) {
    unlink(temporary);
    LOG_WARN("HLS download %u: cannot publish MP4; retained native output",
             d->id);
    return 0;
  }
  char original[sizeof(d->dest_path)];
  strcpy(original, d->dest_path);
  dm_mutex_t *mutex = queue_manager_get_mutex();
  dm_mutex_lock(mutex);
  int saved = stopped(d) ? -1 : db_update_media_output(d->id, published, bytes);
  if (saved == 0) {
    strcpy(d->dest_path, published);
    d->total_size = bytes;
    atomic_store(&d->bytes_downloaded, bytes);
    atomic_store(&d->auto_filename, false);
  }
  dm_mutex_unlock(mutex);
  unlink(temporary);
  if (saved != 0) {
    unlink(published);
    LOG_WARN("HLS download %u: cannot persist MP4 destination; retained native "
             "output",
             d->id);
    return stopped(d) ? -1 : 0;
  }
  if (unlink(original) != 0)
    LOG_WARN("HLS download %u: could not remove native output after remux",
             d->id);
  return 0;
}

static int run_asset_playlist(Download *d, const char *destination,
                              const HlsPlaylist *provided, const char *identity,
                              uint64_t *output_bytes) {
  char original_destination[HLS_PATH_MAX];
  if (!destination || strlen(destination) >= sizeof(original_destination))
    return -1;
  strcpy(original_destination, destination);
  const RequestOptions *r = d->request;
  RequestContext context = {
      .cookie = r && r->cookie[0] ? r->cookie : NULL,
      .referrer = r && r->referrer[0] ? r->referrer : NULL,
      .user_agent = r && r->user_agent[0] ? r->user_agent : NULL,
      .extra_headers = r && r->extra_headers[0] ? r->extra_headers : NULL,
      .auth_user = r && r->auth_user[0] ? r->auth_user : NULL,
      .auth_password = r && r->auth_password[0] ? r->auth_password : NULL,
      .sensitive = r && r->browser_context};
  if (d->requires_browser_context && !context.sensitive)
    return -5;
  int result = -1, output = -1, lock = -1;
  char state_path[HLS_PATH_MAX], directory[HLS_PATH_MAX],
      lock_path[HLS_PATH_MAX];
  int n = snprintf(state_path, sizeof(state_path), "%s.hlsstate", destination);
  if (n < 0 || (size_t)n >= sizeof(state_path))
    return -1;
  n = snprintf(directory, sizeof(directory), "%s.hlsparts", destination);
  if (n < 0 || (size_t)n >= sizeof(directory))
    return -1;
  n = snprintf(lock_path, sizeof(lock_path), "%s/lock", directory);
  if (n < 0 || (size_t)n >= sizeof(lock_path))
    return -1;
  struct stat st;
  if (lstat(state_path, &st) == 0 &&
      (!S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1))
    return -1;
  if (mkdir(directory, 0700) != 0 && errno != EEXIST)
    return -1;
  if (lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != getuid() || (st.st_mode & 077))
    return -1;
  lock = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (!safe_file(lock) || flock(lock, LOCK_EX | LOCK_NB) != 0) {
    if (lock >= 0)
      close(lock);
    return -1;
  }
  output = open(destination, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (!d->reserved_file || !safe_file(output) || fstat(output, &st) != 0) {
    result = -3;
    goto finish;
  }
  if (st.st_size != 0) {
    struct stat state;
    if (lstat(state_path, &state) != 0 || !S_ISREG(state.st_mode) ||
        state.st_uid != getuid() || state.st_nlink != 1) {
      result = -3;
      goto finish;
    }
  }
  HlsPlaylist playlist = {0};
  char fingerprint[65] = "";
  bool parsed = false;
  if (provided) {
    if (!identity || strlen(identity) != 64)
      goto finish;
    playlist = *provided;
    playlist.segments =
        calloc(provided->segment_count, sizeof(*playlist.segments));
    playlist.maps = provided->map_count
                        ? calloc(provided->map_count, sizeof(*playlist.maps))
                        : NULL;
    if (!playlist.segments || (provided->map_count && !playlist.maps)) {
      hls_playlist_free(&playlist);
      goto finish;
    }
    memcpy(playlist.segments, provided->segments,
           provided->segment_count * sizeof(*playlist.segments));
    if (provided->map_count)
      memcpy(playlist.maps, provided->maps,
             provided->map_count * sizeof(*playlist.maps));
    strcpy(fingerprint, identity);
    parsed = true;
  } else {
    unsigned char *data = malloc(HLS_MAX_PLAYLIST_BYTES + 1);
    if (!data)
      goto finish;
    char url[HLS_URL_MAX];
    strcpy(url, d->url);
    for (int depth = 0; depth < 5; depth++) {
      HlsBody body = {
          .data = data, .limit = HLS_MAX_PLAYLIST_BYTES, .download = d};
      char effective[HLS_URL_MAX], error[128];
      if (!fetch_body(d, &context, url, &body, effective))
        break;
      HlsResult parse_result = hls_parse((char *)data, body.size, effective,
                                         &playlist, error, sizeof(error));
      if (parse_result != HLS_OK) {
        if (parse_result != HLS_NO_MEMORY) {
          queue_manager_set_site_error(d->id, error);
          result = -9;
        }
        break;
      }
      if (playlist.is_master) {
        if (depth == 4) {
          queue_manager_set_site_error(d->id,
                                       "HLS playlist nesting limit exceeded");
          result = -9;
        }
        strcpy(url, playlist.selected_url);
        hls_playlist_free(&playlist);
        continue;
      }
      /* Manifest body and final base URL identify both the selected layout and
       * relative-resource resolution. Key identity is checked per item on
       * resume.
       */
      EVP_MD_CTX *digest = EVP_MD_CTX_new();
      unsigned char hash[32];
      unsigned int length = 0;
      bool ok =
          digest && EVP_DigestInit_ex(digest, EVP_sha256(), NULL) == 1 &&
          EVP_DigestUpdate(digest, data, body.size) == 1 &&
          EVP_DigestUpdate(digest, effective, strlen(effective) + 1) == 1 &&
          EVP_DigestFinal_ex(digest, hash, &length) == 1 && length == 32;
      EVP_MD_CTX_free(digest);
      if (ok) {
        const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < 32; i++) {
          fingerprint[i * 2] = digits[hash[i] >> 4];
          fingerprint[i * 2 + 1] = digits[hash[i] & 15];
        }
        fingerprint[64] = '\0';
        parsed = true;
      }
      break;
    }
    free(data);
  }
  if (!parsed || !playlist.end_list) {
    LOG_WARN("HLS download %u requires a supported finite VOD playlist", d->id);
    if (parsed && !playlist.end_list) {
      queue_manager_set_site_error(d->id, "Live HLS playlist is unsupported");
      result = -9;
    }
    hls_playlist_free(&playlist);
    goto finish;
  }
  size_t count = playlist.map_count + playlist.segment_count;
  HlsSaved *saved = calloc(count, sizeof(*saved));
  if (!saved) {
    hls_playlist_free(&playlist);
    goto finish;
  }
  load_state(state_path, fingerprint, saved, count);
  if (!provided)
    atomic_store(&d->bytes_downloaded, 0);
  DownloadManagerConfig config;
  config_get(&config);
  uint64_t limit = r ? r->speed_limit_bps : 0;
  size_t parallel = (size_t)config.max_connections_per_download;
  if (parallel < 1)
    parallel = 1;
  if (parallel > MAX_WORKERS)
    parallel = MAX_WORKERS;
  bool all_ok = save_state(state_path, fingerprint, saved, count);
  for (size_t begin = 0; all_ok && begin < count && !stopped(d);
       begin += parallel) {
    HlsJob jobs[MAX_WORKERS] = {0};
    dm_thread_t threads[MAX_WORKERS];
    bool started[MAX_WORKERS] = {0};
    size_t batch = count - begin < parallel ? count - begin : parallel;
    for (size_t j = 0; j < batch; j++) {
      size_t index = begin + j;
      jobs[j].download = d;
      jobs[j].context = &context;
      jobs[j].config = config;
      jobs[j].saved = saved[index];
      jobs[j].speed_limit =
          limit ? (limit / parallel ? limit / parallel : 1) : 0;
      if (index < playlist.map_count) {
        jobs[j].url = playlist.maps[index].url;
        jobs[j].key = &playlist.maps[index].key;
      } else {
        HlsSegment *segment = &playlist.segments[index - playlist.map_count];
        jobs[j].url = segment->url;
        jobs[j].key = &segment->key;
      }
      if (item_path(directory, index, jobs[j].path, sizeof(jobs[j].path)))
        started[j] = dm_thread_create(&threads[j], segment_job, &jobs[j]) == 0;
    }
    for (size_t j = 0; j < batch; j++) {
      if (started[j])
        dm_thread_join(&threads[j], NULL);
      saved[begin + j] = jobs[j].saved;
      if (!started[j] || !jobs[j].success)
        all_ok = false;
    }
    if (!save_state(state_path, fingerprint, saved, count))
      all_ok = false;
  }
  char concat[HLS_PATH_MAX];
  n = snprintf(concat, sizeof(concat), "%s/concat", directory);
  if (all_ok && !stopped(d) && n >= 0 && (size_t)n < sizeof(concat)) {
    int assembled = create_file(concat);
    all_ok = assembled >= 0;
    size_t previous_map = HLS_NO_MAP;
    for (size_t i = 0; all_ok && i < playlist.segment_count; i++) {
      HlsSegment *segment = &playlist.segments[i];
      char path[HLS_PATH_MAX];
      if (segment->map_index != HLS_NO_MAP &&
          segment->map_index != previous_map) {
        all_ok = item_path(directory, segment->map_index, path, sizeof(path)) &&
                 append_file(assembled, path, d);
        previous_map = segment->map_index;
      }
      all_ok =
          all_ok &&
          item_path(directory, playlist.map_count + i, path, sizeof(path)) &&
          append_file(assembled, path, d);
    }
    if (assembled >= 0) {
      if (fsync(assembled) != 0)
        all_ok = false;
      close(assembled);
    }
    char hash[65];
    uint64_t bytes = 0;
    all_ok = all_ok && digest_file(concat, hash, &bytes);
    if (all_ok && !provided && r && r->expected_sha256[0] &&
        strcasecmp(hash, r->expected_sha256) != 0) {
      all_ok = false;
      result = -2;
    }
    struct stat current, reserved;
    all_ok = all_ok && lstat(destination, &current) == 0 &&
             fstat(output, &reserved) == 0 &&
             current.st_dev == reserved.st_dev &&
             current.st_ino == reserved.st_ino;
    if (all_ok && current.st_size != 0) {
      char published_hash[65];
      uint64_t published_size = 0;
      all_ok = digest_file(destination, published_hash, &published_size) &&
               published_size == bytes && strcmp(published_hash, hash) == 0;
      if (!all_ok)
        result = -3;
    } else if (all_ok) {
      /* All segments and the assembled output are durable before publication.
       * Renaming within the destination filesystem publishes complete bytes.
       * A restart can verify an already-published file against this assembly.
       */
      all_ok = !stopped(d) && rename(concat, destination) == 0;
    }
    if (all_ok) {
      if (output_bytes)
        *output_bytes = bytes;
      if (provided)
        result = 0;
      else {
        dm_mutex_t *mutex = queue_manager_get_mutex();
        dm_mutex_lock(mutex);
        d->total_size = bytes;
        d->progress = 1.0f;
        db_update_total_size(d->id, bytes);
        dm_mutex_unlock(mutex);
        atomic_store(&d->bytes_downloaded, bytes);
        result = remux_output(d, directory);
      }
    }
    unlink(concat);
  }
  if (result == 0 && !provided) {
    dm_mutex_t *mutex = queue_manager_get_mutex();
    dm_mutex_lock(mutex);
    if (db_update_media_output(d->id, destination, d->total_size) != 0)
      result = -1;
    dm_mutex_unlock(mutex);
  }
  if ((!provided && result == 0) || result == -2 ||
      atomic_load(&d->cancel_requested)) {
    for (size_t i = 0; i < count; i++) {
      char path[HLS_PATH_MAX];
      if (item_path(directory, i, path, sizeof(path)))
        unlink(path);
    }
    unlink(state_path);
  }
  free(saved);
  hls_playlist_free(&playlist);
finish:
  if (output >= 0)
    close(output);
  close(lock);
  if ((!provided && result == 0) || result == -2 ||
      atomic_load(&d->cancel_requested)) {
    hls_discard_state(original_destination);
  }
  if (result == -2)
    unlink(destination);
  return result;
}
int hls_run_download(Download *d) {
  return run_asset_playlist(d, d->dest_path, NULL, NULL, NULL);
}
int hls_run_asset_playlist(Download *d, const char *destination,
                           const HlsPlaylist *playlist, const char *fingerprint,
                           uint64_t *bytes) {
  if (!playlist || !playlist->segment_count ||
      playlist->segment_count > HLS_MAX_SEGMENTS ||
      playlist->map_count > HLS_MAX_MAPS || !playlist->segments ||
      (playlist->map_count && !playlist->maps))
    return -1;
  return run_asset_playlist(d, destination, playlist, fingerprint, bytes);
}
int hls_fetch_manifest(Download *d, unsigned char *buffer, size_t capacity,
                       size_t *length, char *base, char *fingerprint) {
  if (!buffer || !length || !base || !fingerprint ||
      capacity > HLS_MAX_PLAYLIST_BYTES)
    return -1;
  const RequestOptions *r = d->request;
  RequestContext context = {
      .cookie = r && r->cookie[0] ? r->cookie : NULL,
      .referrer = r && r->referrer[0] ? r->referrer : NULL,
      .user_agent = r && r->user_agent[0] ? r->user_agent : NULL,
      .extra_headers = r && r->extra_headers[0] ? r->extra_headers : NULL,
      .auth_user = r && r->auth_user[0] ? r->auth_user : NULL,
      .auth_password = r && r->auth_password[0] ? r->auth_password : NULL,
      .sensitive = r && r->browser_context};
  if (d->requires_browser_context && !context.sensitive)
    return -5;
  HlsBody body = {.data = buffer, .limit = capacity, .download = d};
  if (!fetch_body(d, &context, d->url, &body, base))
    return -1;
  EVP_MD_CTX *digest = EVP_MD_CTX_new();
  unsigned char hash[32];
  unsigned int hash_size = 0;
  bool ok = digest && EVP_DigestInit_ex(digest, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(digest, buffer, body.size) == 1 &&
            EVP_DigestUpdate(digest, base, strlen(base) + 1) == 1 &&
            EVP_DigestFinal_ex(digest, hash, &hash_size) == 1 &&
            hash_size == 32;
  EVP_MD_CTX_free(digest);
  if (!ok)
    return -1;
  const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    fingerprint[i * 2] = digits[hash[i] >> 4];
    fingerprint[i * 2 + 1] = digits[hash[i] & 15];
  }
  fingerprint[64] = 0;
  *length = body.size;
  return 0;
}

#else
/* TODO(platform): implement secure HLS staging and resume on Windows. */
void hls_discard_state(const char *destination) { (void)destination; }
int hls_run_download(struct Download *download) {
  (void)download;
  return -1;
}
int hls_run_asset_playlist(struct Download *d, const char *path,
                           const HlsPlaylist *playlist, const char *fingerprint,
                           uint64_t *bytes) {
  (void)d;
  (void)path;
  (void)playlist;
  (void)fingerprint;
  (void)bytes;
  return -1;
}
int hls_fetch_manifest(struct Download *d, unsigned char *buffer,
                       size_t capacity, size_t *length, char *base,
                       char *fingerprint) {
  (void)d;
  (void)buffer;
  (void)capacity;
  (void)length;
  (void)base;
  (void)fingerprint;
  return -1;
}
#endif
