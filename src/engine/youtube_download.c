// SPDX-License-Identifier: MIT
#include "youtube_download.h"
#include "youtube_sabr.h"
#include "youtube_transfer.h"
#include "../core/queue_manager.h"
#include "../persistence/db.h"
#include "../platform/spawn.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../utils/path.h"
#include "../vendor/cJSON.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const cJSON *item(const cJSON *parent, const char *key) {
  return cJSON_GetObjectItemCaseSensitive(parent, key);
}

static const char *string(const cJSON *parent, const char *key) {
  const cJSON *field = item(parent, key);
  return cJSON_IsString(field) ? field->valuestring : NULL;
}

static bool decimal(const char *s, uint64_t *value) {
  if (!s || !*s) return false;
  uint64_t parsed = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' ||
        parsed > (UINT64_MAX - (unsigned)(*s - '0')) / 10) return false;
    parsed = parsed * 10 + (unsigned)(*s - '0');
  }
  *value = parsed;
  return true;
}

static int b64_digit(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}

static bool decode_config(const char *encoded, unsigned char *out,
                          size_t capacity, size_t *out_length) {
  if (!encoded) return false;
  size_t length = strlen(encoded), count = 0;
  if (!length || length > 44000) return false;
  unsigned bits = 0, carry = 0;
  bool padding = false;
  for (size_t i = 0; i < length; i++) {
    if (encoded[i] == '=') { padding = true; continue; }
    int digit = b64_digit(encoded[i]);
    if (padding || digit < 0) return false;
    carry = (carry << 6) | (unsigned)digit;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (count >= capacity) return false;
      out[count++] = (unsigned char)(carry >> bits);
      carry &= (1u << bits) - 1u;
    }
  }
  if (!count || bits >= 6 || carry) return false;
  *out_length = count;
  return true;
}

static bool parse_format(const cJSON *f, const char *prefix,
                         uint32_t *itag, uint64_t *last_modified,
                         char *xtags, size_t xtags_size,
                         uint64_t *size) {
  const cJSON *id = item(f, "itag");
  const char *mime = string(f, "mimeType");
  if (!cJSON_IsNumber(id) || id->valuedouble < 1 ||
      id->valuedouble > 100000 || (double)(uint32_t)id->valuedouble !=
      id->valuedouble || !mime || strncmp(mime, prefix, strlen(prefix)))
    return false;
  uint64_t lmt = 0, bytes = 0;
  if (!decimal(string(f, "lastModified"), &lmt)) return false;
  const char *length = string(f, "contentLength");
  if (length && !decimal(length, &bytes)) return false;
  const char *tags = string(f, "xtags");
  if (tags && strlen(tags) >= xtags_size) return false;
  if (tags) strcpy(xtags, tags);
  else xtags[0] = '\0';
  *itag = (uint32_t)id->valuedouble;
  *last_modified = lmt;
  *size = bytes;
  return true;
}

bool youtube_parse_player(const char *json, const char *video_id,
                          uint32_t selected_itag, YoutubeSelection *out) {
  if (!json || !video_id || !out || !selected_itag ||
      strlen(video_id) > 32) return false;
  cJSON *root = cJSON_ParseWithLength(json, strlen(json));
  if (!root) return false;
  YoutubeSelection selected = {0};
  const cJSON *details = item(root, "videoDetails");
  const cJSON *status = item(root, "playabilityStatus");
  const cJSON *stream = item(root, "streamingData");
  const cJSON *formats = item(stream, "adaptiveFormats");
  const char *id = string(details, "videoId");
  const char *url = string(stream, "serverAbrStreamingUrl");
  const cJSON *live = item(details, "isLive");
  const cJSON *live_content = item(details, "isLiveContent");
  const cJSON *media = item(item(root, "playerConfig"), "mediaCommonConfig");
  const cJSON *ustream = item(media, "mediaUstreamerRequestConfig");
  const char *config = string(ustream, "videoPlaybackUstreamerConfig");
  bool valid = id && strcmp(id, video_id) == 0 &&
      strcmp(string(status, "status") ? string(status, "status") : "", "OK") == 0 &&
      !cJSON_IsTrue(live) && !cJSON_IsTrue(live_content) &&
      url && strlen(url) < sizeof(selected.stream_url) &&
      strncmp(url, "https://", 8) == 0 && cJSON_IsArray(formats) &&
      decode_config(config, selected.config, sizeof(selected.config),
                    &selected.config_length);
  if (valid) {
    strcpy(selected.video_id, id);
    strcpy(selected.stream_url, url);
    bool have_video = false, have_audio = false;
    uint64_t video_size = 0, audio_size = 0;
    const cJSON *format;
    cJSON_ArrayForEach(format, formats) {
      uint32_t itag = 0;
      uint64_t lmt = 0, size = 0;
      char tags[513];
      if (!have_video && parse_format(format, "video/mp4", &itag, &lmt,
                                       tags, sizeof(tags), &size) &&
          itag == selected_itag) {
        const cJSON *height = item(format, "height");
        if (cJSON_IsNumber(height) && height->valuedouble >= 1 &&
            height->valuedouble <= 4320 &&
            (double)(uint32_t)height->valuedouble == height->valuedouble) {
          selected.video_itag = itag;
          selected.height = (uint32_t)height->valuedouble;
          selected.video_last_modified = lmt;
          strcpy(selected.video_xtags, tags);
          video_size = size;
          have_video = true;
        }
      }
      if (!have_audio && parse_format(format, "audio/mp4", &itag, &lmt,
                                       tags, sizeof(tags), &size) &&
          itag == 140) {
        selected.audio_itag = itag;
        selected.audio_last_modified = lmt;
        strcpy(selected.audio_xtags, tags);
        audio_size = size;
        have_audio = true;
      }
    }
    valid = have_video && have_audio &&
        video_size <= UINT64_MAX - audio_size;
    if (valid) selected.expected_size = video_size + audio_size;
  }
  cJSON_Delete(root);
  if (valid) *out = selected;
  return valid;
}

typedef struct {
  char *bytes;
  size_t length, limit;
  YoutubeResponseFile spool;
  Download *download;
  uint64_t base_bytes;
} ResponseBuffer;

static void response_release(ResponseBuffer *response) {
  if (response->spool.file) youtube_response_close(&response->spool);
  else free(response->bytes);
  response->bytes = NULL;
  response->length = 0;
}

static int response_progress(void *userdata, curl_off_t total,
    curl_off_t now, curl_off_t upload_total, curl_off_t upload_now) {
  (void)total; (void)now; (void)upload_total; (void)upload_now;
  Download *d = userdata;
  if (d) {
    dm_mutex_t *mutex = queue_manager_get_mutex();
    dm_mutex_lock(mutex);
    if (d->total_size) {
      double fraction = (double)atomic_load(&d->bytes_downloaded) / d->total_size;
      d->progress = fraction > 0.99 ? 0.99f : (float)fraction;
    }
    dm_mutex_unlock(mutex);
  }
  return d && (atomic_load(&d->cancel_requested) ||
               atomic_load(&d->pause_requested));
}

static size_t response_write(char *data, size_t unit, size_t count,
                             void *userdata) {
  ResponseBuffer *buffer = userdata;
  if (unit && count > SIZE_MAX / unit) return 0;
  size_t amount = unit * count;
  if (buffer->spool.file) {
    if (!youtube_response_append(&buffer->spool, data, amount)) return 0;
    buffer->length = buffer->spool.length;
    if (buffer->download)
      atomic_store(&buffer->download->bytes_downloaded,
                   buffer->base_bytes + buffer->spool.media_bytes);
    return amount;
  }
  if (amount > buffer->limit - buffer->length) return 0;
  char *next = realloc(buffer->bytes, buffer->length + amount + 1);
  if (!next) return 0;
  buffer->bytes = next;
  memcpy(next + buffer->length, data, amount);
  buffer->length += amount;
  next[buffer->length] = '\0';
  return amount;
}

static bool https_get_or_post(const char *url, const void *body,
                              size_t body_length, const char *content_type,
                              const char *user_agent, const char *referrer,
                              size_t limit, Download *download,
                              ResponseBuffer *out) {
  CURL *curl = curl_easy_init();
  if (!curl) return false;
  struct curl_slist *headers = NULL;
  if (content_type) {
    headers = curl_slist_append(headers, content_type);
    if (!headers) { curl_easy_cleanup(curl); return false; }
    if (strstr(content_type, "application/x-protobuf")) {
      struct curl_slist *next = curl_slist_append(headers,
          "Accept: application/vnd.yt-ump");
      if (!next) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return false;
      }
      headers = next;
    }
  }
  ResponseBuffer received = {.limit = limit, .download = download,
      .base_bytes = download ? atomic_load(&download->bytes_downloaded) : 0};
  if (download && !youtube_response_open(&received.spool, download->dest_path)) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return false;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, download ? 0L : 30L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, response_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, download);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &received);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  if (user_agent && *user_agent)
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
  if (referrer && *referrer)
    curl_easy_setopt(curl, CURLOPT_REFERER, referrer);
  if (body) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_length);
  }
  CURLcode code = curl_easy_perform(curl);
  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (received.spool.file && code == CURLE_OK && http_status == 200 &&
      youtube_response_map(&received.spool))
    received.bytes = (char *)received.spool.data;
  if (code != CURLE_OK || http_status != 200 || !received.bytes) {
    LOG_WARN("YouTube request failed: curl=%d HTTP=%ld bytes=%zu",
             (int)code, http_status, received.length);
    if (download) atomic_store(&download->bytes_downloaded, received.base_bytes);
    response_release(&received);
    return false;
  }
  *out = received;
  return true;
}

static bool innertube_key(const char *html, char *out, size_t capacity) {
  static const char marker[] = "\"INNERTUBE_API_KEY\":\"";
  const char *start = strstr(html, marker);
  if (!start) return false;
  start += sizeof(marker) - 1;
  size_t length = 0;
  while ((start[length] >= 'A' && start[length] <= 'Z') ||
         (start[length] >= 'a' && start[length] <= 'z') ||
         (start[length] >= '0' && start[length] <= '9') ||
         start[length] == '_' || start[length] == '-') length++;
  if (length < 10 || length >= capacity || start[length] != '"') return false;
  memcpy(out, start, length);
  out[length] = '\0';
  return true;
}

static bool googlevideo_url(const char *url) {
  if (!url || strncmp(url, "https://", 8) != 0) return false;
  const char *host = url + 8;
  const char *path = strchr(host, '/');
  if (!path || path - host < 16 || path - host > 254) return false;
  if (strncmp(path, "/videoplayback", 14) != 0 ||
      (path[14] != '\0' && path[14] != '?')) return false;
  static const char suffix[] = ".googlevideo.com";
  size_t host_length = (size_t)(path - host);
  return host_length > sizeof(suffix) - 1 &&
      memcmp(host + host_length - (sizeof(suffix) - 1), suffix,
             sizeof(suffix) - 1) == 0 &&
      !memchr(host, '@', host_length) && !memchr(host, ':', host_length);
}

static bool video_id_from_watch(const char *url, char video_id[33]) {
  if (strncmp(url, "https://www.youtube.com/watch?", 30) != 0) return false;
  const char *query = strchr(url, '?');
  if (!query) return false;
  for (const char *field = query + 1; *field; ) {
    if (field[0] == 'v' && field[1] == '=') {
      field += 2;
      size_t length = 0;
      while (field[length] && field[length] != '&') {
        char c = field[length];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        length++;
      }
      if (length != 11) return false;
      memcpy(video_id, field, length);
      video_id[length] = '\0';
      return true;
    }
    const char *next = strchr(field, '&');
    if (!next) break;
    field = next + 1;
  }
  return false;
}

bool youtube_sequence_url(const char *base, unsigned sequence,
                          char *out, size_t capacity) {
  if (!base || !out || !capacity) return false;
  const char *query = strchr(base, '?');
  if (query) for (const char *field = query + 1; *field; ) {
    if (strncmp(field, "rn=", 3) == 0) {
      const char *tail = strchr(field, '&');
      if (!tail) tail = base + strlen(base);
      size_t prefix = (size_t)(field + 3 - base);
      if (prefix > INT_MAX) return false;
      int n = snprintf(out, capacity, "%.*s%u%s", (int)prefix, base,
                       sequence, tail);
      return n >= 0 && (size_t)n < capacity;
    }
    const char *next = strchr(field, '&');
    if (!next) break;
    field = next + 1;
  }
  int n = snprintf(out, capacity, "%s%crn=%u", base,
                   query ? '&' : '?', sequence);
  return n >= 0 && (size_t)n < capacity;
}

static bool fetch_selection(const char *video_id, uint32_t itag,
                            YoutubeSelection *out) {
  char watch[128], api[256], request[512], key[129];
  int n = snprintf(watch, sizeof(watch),
                   "https://www.youtube.com/watch?v=%s", video_id);
  if (n < 0 || (size_t)n >= sizeof(watch)) return false;
  ResponseBuffer html = {0};
  if (!https_get_or_post(watch, NULL, 0, NULL, NULL, NULL,
                         4 * 1024 * 1024, NULL, &html))
    return false;
  bool found = innertube_key(html.bytes, key, sizeof(key));
  free(html.bytes);
  if (!found) return false;
  n = snprintf(api, sizeof(api),
      "https://www.youtube.com/youtubei/v1/player?key=%s", key);
  if (n < 0 || (size_t)n >= sizeof(api)) return false;
  n = snprintf(request, sizeof(request),
      "{\"videoId\":\"%s\",\"context\":{\"client\":{"
      "\"clientName\":\"ANDROID\",\"clientVersion\":\"21.26.364\","
      "\"androidSdkVersion\":33,\"osName\":\"Android\","
      "\"osVersion\":\"13\"}}}", video_id);
  if (n < 0 || (size_t)n >= sizeof(request)) return false;
  ResponseBuffer player = {0};
  if (!https_get_or_post(api, request, (size_t)n,
                         "Content-Type: application/json", NULL, NULL,
                         4 * 1024 * 1024, NULL,
                         &player)) return false;
  bool valid = youtube_parse_player(player.bytes, video_id, itag, out) &&
               googlevideo_url(out->stream_url);
  free(player.bytes);
  return valid;
}

static bool browser_selection(const RequestOptions *request,
                              const char *video_id, uint32_t itag,
                              uint64_t expected_size,
                              YoutubeSelection *out) {
  if (!request || !request->browser_context ||
      !request->youtube_sabr_url[0] ||
      !request->youtube_request_b64[0] ||
      request->youtube_height < 1 || request->youtube_height > 4320 ||
      !googlevideo_url(request->youtube_sabr_url)) return false;
  unsigned char raw[16000];
  size_t raw_length = 0;
  if (!decode_config(request->youtube_request_b64, raw, sizeof(raw),
                     &raw_length) ||
      !sabr_parse_captured_request(raw, raw_length, itag,
                                   &out->browser_capture)) {
    memset(raw, 0, sizeof(raw));
    return false;
  }
  memset(raw, 0, sizeof(raw));
  if (!out->browser_capture.video_format_length) {
    YoutubeSelection player = {0};
    if (!fetch_selection(video_id, itag, &player) ||
        player.height != request->youtube_height) return false;
    out->video_last_modified = player.video_last_modified;
    memcpy(out->video_xtags, player.video_xtags,
           sizeof(out->video_xtags));
  }
  strcpy(out->video_id, video_id);
  strcpy(out->stream_url, request->youtube_sabr_url);
  out->video_itag = itag;
  out->audio_itag = out->browser_capture.audio_itag;
  out->height = request->youtube_height;
  out->config_length = out->browser_capture.config_length;
  memcpy(out->config, out->browser_capture.config, out->config_length);
  out->expected_size = expected_size;
  out->from_browser = true;
  return true;
}

static bool stopped(const Download *d) {
  return atomic_load(&d->cancel_requested) || atomic_load(&d->pause_requested);
}

static bool stage_path(const char *base, const char *suffix, char *out,
                       size_t capacity) {
  int n = snprintf(out, capacity, "%s%s", base, suffix);
  return n >= 0 && (size_t)n < capacity;
}

static bool owned_regular(int fd) {
  struct stat st;
  return fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
         st.st_uid == getuid() && st.st_nlink == 1;
}

static bool durable_parent(const char *path) {
  char parent[1024];
  size_t length = strlen(path);
  if (length >= sizeof(parent)) return false;
  memcpy(parent, path, length + 1);
  char *slash = strrchr(parent, '/');
  if (!slash) strcpy(parent, ".");
  else if (slash == parent) slash[1] = '\0';
  else *slash = '\0';
  int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  bool okay = fd >= 0 && fsync(fd) == 0;
  if (fd >= 0) close(fd);
  return okay;
}

static bool publish_unique(const char *source, const char *destination,
                           char out[1024]) {
  for (int i = 0; i < 1000000; i++) {
    if (!path_make_unique(destination, out, 1024)) return false;
    if (link(source, out) == 0) return true;
    if (errno != EEXIST) return false;
  }
  return false;
}

static void update_transfer_progress(Download *d, uint64_t bytes,
                                     uint64_t total) {
  atomic_store(&d->bytes_downloaded, bytes);
  dm_mutex_t *mutex = queue_manager_get_mutex();
  dm_mutex_lock(mutex);
  if (total && d->total_size != total) {
    if (db_update_total_size(d->id, total) == 0) d->total_size = total;
  }
  if (total) {
    double fraction = (double)bytes / (double)total;
    d->progress = fraction > 0.99 ? 0.99f : (float)fraction;
  }
  dm_mutex_unlock(mutex);
}

static int transfer_selected(Download *d, YoutubeSelection *selection,
                             YoutubeTrackState *video,
                             YoutubeTrackState *audio) {
  SabrFormatId video_id = {.itag = selection->video_itag,
      .last_modified = selection->video_last_modified,
      .xtags = selection->video_xtags};
  SabrFormatId audio_id = {.itag = selection->audio_itag,
      .last_modified = selection->audio_last_modified,
      .xtags = selection->audio_xtags};
  if (selection->from_browser) {
    if (selection->browser_capture.video_format_length) {
      video_id.raw = selection->browser_capture.video_format;
      video_id.raw_length = selection->browser_capture.video_format_length;
    }
    audio_id.raw = selection->browser_capture.audio_format;
    audio_id.raw_length = selection->browser_capture.audio_format_length;
  }
  unsigned char request[40000];
  unsigned stalled = 0;
  bool authorized = false;
  for (unsigned sequence = 0; sequence < 4096 && !stopped(d); sequence++) {
    if (video->has_end && audio->has_end &&
        video->has_segment && audio->has_segment &&
        video->last_segment >= video->end_segment &&
        audio->last_segment >= audio->end_segment) return 0;
    SabrBufferedRange ranges[2];
    size_t range_count = 0;
    if (video->has_segment)
      ranges[range_count++] = (SabrBufferedRange){
          .format = video_id, .end_ms = video->end_ms,
          .end_segment = video->last_segment,
          .start_segment = video->first_segment};
    if (audio->has_segment)
      ranges[range_count++] = (SabrBufferedRange){
          .format = audio_id, .end_ms = audio->end_ms,
          .end_segment = audio->last_segment,
          .start_segment = audio->first_segment};
    uint64_t playback_ms = video->end_ms < audio->end_ms
                               ? video->end_ms : audio->end_ms;
    size_t request_length = 0;
    if (!sabr_encode_request_with_context(&video_id, &audio_id,
            selection->height, playback_ms, selection->config,
            selection->config_length, ranges, range_count,
            selection->from_browser ? selection->browser_capture.context : NULL,
            selection->from_browser ? selection->browser_capture.context_length : 0,
            request, sizeof(request), &request_length)) return -9;
    char url[2100];
    if (!youtube_sequence_url(selection->stream_url, sequence,
                              url, sizeof(url))) return -9;
    ResponseBuffer response = {0};
    bool fetched = https_get_or_post(url, request, request_length,
        "Content-Type: application/x-protobuf",
        d->request ? d->request->user_agent : NULL,
        d->request ? d->request->referrer : NULL,
        0, d,
        &response);
    uint64_t before_video = video->bytes;
    uint64_t before_audio = audio->bytes;
    int protection = fetched ? sabr_protection_status(
        (const unsigned char *)response.bytes, response.length) : -1;
    if (fetched && (protection == 2 || protection == 3 ||
                    (protection == 0 && !authorized))) {
      queue_manager_set_site_error(d->id,
          "YouTube rejected browser playback authorization");
      response_release(&response);
      return -9;
    }
    if (protection == 1) authorized = true;
    bool context_valid = !selection->from_browser ||
        (fetched && sabr_update_playback_context(
            &selection->browser_capture,
            (const unsigned char *)response.bytes, response.length));
    const char *parse_error = "Unsupported YouTube playback context";
    bool accepted = fetched && context_valid && youtube_process_ump_ex(
        (const unsigned char *)response.bytes, response.length,
        selection->video_id, video, audio, &parse_error);
    if (fetched && !accepted)
      LOG_WARN("YouTube UMP response rejected for download %u (%zu bytes): %s",
               d->id, response.length, parse_error);
    response_release(&response);
    atomic_store(&d->bytes_downloaded, video->bytes + audio->bytes);
    if (fetched && !accepted) {
      queue_manager_set_site_error(d->id, parse_error);
      return -9;
    }
    if (!accepted || (video->bytes == before_video &&
                      audio->bytes == before_audio)) {
      if (accepted)
        LOG_WARN("YouTube stream stalled for download %u at request %u",
                 d->id, sequence);
      if (++stalled >= 3) {
        if (!fetched) return -1;
        queue_manager_set_site_error(d->id,
            "YouTube stopped sending media; play the video and offer it again");
        return -9;
      }
      dm_thread_sleep_ms(500u << (stalled - 1));
      continue;
    }
    stalled = 0;
    update_transfer_progress(d, video->bytes + audio->bytes,
                             selection->expected_size);
  }
  return stopped(d) ? -1 : -9;
}

int youtube_run_download(Download *d) {
  if (!d || !d->site_format_id[0]) return -9;
  uint64_t requested = 0;
  char video_id[33];
  if (!decimal(d->site_format_id, &requested) || !requested ||
      requested > UINT32_MAX || !video_id_from_watch(d->url, video_id)) {
    queue_manager_set_site_error(d->id, "Invalid YouTube selection");
    return -9;
  }
  YoutubeSelection *selection = calloc(1, sizeof(*selection));
  if (!selection) return -1;
  int result = -1;
  bool selected = browser_selection(d->request, video_id,
      (uint32_t)requested, d->total_size, selection);
  if (!selected && d->request && !d->request->browser_context)
    selected = fetch_selection(video_id, (uint32_t)requested, selection);
  if (!selected) {
    queue_manager_set_site_error(d->id,
        "Fresh browser playback is required for this YouTube format");
    result = -9;
    goto done;
  }
  if (stopped(d)) goto done;
  char original[1024], video_path[1100] = "", audio_path[1100] = "",
      merged_path[1100] = "", published[1024] = "", companion[1024] = "";
  memcpy(original, d->dest_path, sizeof(original));
  int reserved = open(original, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat claim;
  if (!d->reserved_file || !owned_regular(reserved) ||
      fstat(reserved, &claim) != 0 || claim.st_size != 0) {
    if (reserved >= 0) close(reserved);
    result = -3;
    goto done;
  }
  if (!stage_path(original, ".cdm-yt-video-XXXXXX", video_path,
                  sizeof(video_path)) ||
      !stage_path(original, ".cdm-yt-audio-XXXXXX", audio_path,
                  sizeof(audio_path)) ||
      !stage_path(original, ".cdm-yt-merged-XXXXXX", merged_path,
                  sizeof(merged_path))) {
    close(reserved);
    goto done;
  }
  int video_fd = mkstemp(video_path);
  int audio_fd = mkstemp(audio_path);
  if (video_fd < 0 || audio_fd < 0) {
    if (video_fd >= 0) close(video_fd);
    if (audio_fd >= 0) close(audio_fd);
    if (video_fd >= 0) unlink(video_path);
    if (audio_fd >= 0) unlink(audio_path);
    close(reserved);
    goto done;
  }
  YoutubeTrackState video = {.fd = video_fd, .itag = selection->video_itag};
  YoutubeTrackState audio = {.fd = audio_fd, .itag = selection->audio_itag};
  update_transfer_progress(d, 0, selection->expected_size);
  result = transfer_selected(d, selection, &video, &audio);
  if (result == 0) atomic_store(&d->finishing, true);
  if (result == 0 && (fsync(video_fd) != 0 || fsync(audio_fd) != 0)) result = -1;
  close(video_fd);
  close(audio_fd);
  if (result != 0 || stopped(d)) goto clean_stage;
  const char *primary = video_path;
  bool merged = false;
  if (spawn_ffmpeg_available()) {
    int output_fd = mkstemp(merged_path);
    if (output_fd >= 0) {
      close(output_fd);
      DownloadManagerConfig config;
      config_get(&config);
      int rc = selection->audio_itag == 140
          ? spawn_ffmpeg_merge(video_path, audio_path, merged_path,
              &d->cancel_requested, &d->pause_requested,
              config.transfer_timeout_sec)
          : spawn_ffmpeg_merge_opus(video_path, audio_path, merged_path,
          &d->cancel_requested, &d->pause_requested,
          config.transfer_timeout_sec);
      int fd = open(merged_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      struct stat st;
      merged = rc == 0 && owned_regular(fd) && fstat(fd, &st) == 0 &&
               st.st_size > 0 && fsync(fd) == 0;
      if (fd >= 0) close(fd);
    }
    if (merged) primary = merged_path;
  }
  if (stopped(d)) { result = -1; goto clean_stage; }
  if (!merged)
    LOG_WARN("YouTube download %u: ffmpeg unavailable or failed; retaining tracks",
             d->id);
  if (!publish_unique(primary, original, published)) {
    result = -1;
    goto clean_stage;
  }
  if (!merged) {
    char desired[1024];
    const char *suffix = selection->audio_itag == 140
        ? ".audio.m4a" : ".audio.webm";
    if (!stage_path(published, suffix, desired, sizeof(desired)) ||
        !publish_unique(audio_path, desired, companion)) {
      unlink(published);
      result = -1;
      goto clean_stage;
    }
  }
  if (!durable_parent(published)) {
    unlink(published);
    if (companion[0]) unlink(companion);
    result = -1;
    goto clean_stage;
  }
  struct stat current, output;
  result = -1;
  bool unchanged = lstat(original, &current) == 0 &&
      current.st_dev == claim.st_dev && current.st_ino == claim.st_ino &&
      current.st_size == 0;
  if (stat(published, &output) != 0 || output.st_size <= 0) unchanged = false;
  dm_mutex_t *mutex = queue_manager_get_mutex();
  dm_mutex_lock(mutex);
  if (unchanged && !stopped(d) &&
      db_complete_media_outputs(d->id, published, companion,
                                (uint64_t)output.st_size) == 0) {
    strcpy(d->dest_path, published);
    d->total_size = (uint64_t)output.st_size;
    d->progress = 1.0f;
    atomic_store(&d->auto_filename, false);
    atomic_store(&d->bytes_downloaded, d->total_size);
    result = 0;
  }
  dm_mutex_unlock(mutex);
  if (result == 0) unlink(original);
  else {
    unlink(published);
    if (companion[0]) unlink(companion);
  }
clean_stage:
  unlink(video_path);
  unlink(audio_path);
  unlink(merged_path);
  close(reserved);
done:
  ;
  volatile unsigned char *secret = (volatile unsigned char *)selection;
  for (size_t i = 0; i < sizeof(*selection); i++) secret[i] = 0;
  free(selection);
  return result;
}
