// SPDX-License-Identifier: MIT
#include "site_grab.h"
#include "../vendor/cJSON.h"
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SITE_PROBE_JSON_LIMIT (256u * 1024u)

static bool probe_text(const char *text, size_t length) {
  for (size_t i = 0; i < length;) {
    unsigned char c = (unsigned char)text[i++];
    if (c < 0x80) {
      if (c < 0x20 || c == 0x7f) return false;
      continue;
    }
    unsigned int value, remaining, minimum;
    if (c >= 0xc2 && c <= 0xdf) {
      value = c & 31u; remaining = 1; minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      value = c & 15u; remaining = 2; minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      value = c & 7u; remaining = 3; minimum = 0x10000;
    } else return false;
    if (remaining > length - i) return false;
    while (remaining--) {
      c = (unsigned char)text[i++];
      if ((c & 0xc0) != 0x80) return false;
      value = (value << 6) | (c & 63u);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}

static bool probe_token(const char *text, size_t limit) {
  size_t length = strlen(text);
  if (!length || length >= limit) return false;
  for (size_t i = 0; i < length; i++) {
    char c = text[i];
    bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9');
    if (!alphanumeric && (i == 0 || (c != '_' && c != '-' && c != '.')))
      return false;
  }
  return true;
}
bool site_grab_format_id_valid(const char *id) {
  if (!id || !probe_token(id, sizeof(((SiteGrabFormat *)0)->id))) return false;
  static const char *reserved[] = {
    "all", "mergeall", "best", "b", "worst", "w",
    "bestvideo", "bv", "bestaudio", "ba",
    "worstvideo", "wv", "worstaudio", "wa"
  };
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
    if (strcasecmp(id, reserved[i]) == 0) return false;
  return true;
}

static bool probe_integer(const cJSON *item, uint64_t maximum, uint64_t *out) {
  *out = 0;
  if (!item || cJSON_IsNull(item)) return true;
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
      item->valuedouble < 0 || item->valuedouble > (double)maximum ||
      floor(item->valuedouble) != item->valuedouble) return false;
  *out = (uint64_t)item->valuedouble;
  return true;
}

static uint32_t probe_rank(const SiteGrabFormat *format) {
  return format->has_video ? 1u + format->height * 2u +
    (format->has_audio ? 1u : 0u) : 0u;
}

bool site_grab_parse_probe_json(const char *json, size_t length, SiteGrabProbe *out) {
  if (!json || !out || !length || length > SITE_PROBE_JSON_LIMIT || json[length])
    return false;
  memset(out, 0, sizeof(*out));
  cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1, NULL, true);
  if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
  const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
  const cJSON *formats = cJSON_GetObjectItemCaseSensitive(root, "formats");
  if (!cJSON_IsString(title) || !title->valuestring || !cJSON_IsArray(formats)) {
    cJSON_Delete(root); return false;
  }
  size_t title_length = strlen(title->valuestring);
  if (!title_length || !probe_text(title->valuestring, title_length)) {
    cJSON_Delete(root); return false;
  }
  size_t kept = title_length < sizeof(out->title) ? title_length : sizeof(out->title) - 1;
  if (kept < title_length)
    while (kept && ((unsigned char)title->valuestring[kept] & 0xc0u) == 0x80u) kept--;
  memcpy(out->title, title->valuestring, kept);
  out->title[kept] = '\0';
  int count = cJSON_GetArraySize(formats);
  for (int i = 0; i < count; i++) {
    const cJSON *item = cJSON_GetArrayItem(formats, i);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "format_id");
    const cJSON *ext = cJSON_GetObjectItemCaseSensitive(item, "ext");
    const cJSON *video = cJSON_GetObjectItemCaseSensitive(item, "vcodec");
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(item, "acodec");
    if (!cJSON_IsString(id) || !id->valuestring ||
        !site_grab_format_id_valid(id->valuestring) ||
        !cJSON_IsString(ext) || !ext->valuestring ||
        !probe_token(ext->valuestring, sizeof(out->formats[0].ext)) ||
        !cJSON_IsString(video) || !video->valuestring ||
        !cJSON_IsString(audio) || !audio->valuestring) continue;
    bool has_video = strcmp(video->valuestring, "none") != 0;
    bool has_audio = strcmp(audio->valuestring, "none") != 0;
    if (!has_video && !has_audio) continue;
    uint64_t width, height, exact, estimate;
    if (!probe_integer(cJSON_GetObjectItemCaseSensitive(item, "width"), 16384, &width) ||
        !probe_integer(cJSON_GetObjectItemCaseSensitive(item, "height"), 16384, &height) ||
        !probe_integer(cJSON_GetObjectItemCaseSensitive(item, "filesize"),
                       9007199254740991ULL, &exact) ||
        !probe_integer(cJSON_GetObjectItemCaseSensitive(item, "filesize_approx"),
                       9007199254740991ULL, &estimate)) continue;
    SiteGrabFormat parsed = {0};
    SiteGrabFormat *format = &parsed;
    strcpy(format->id, id->valuestring);
    strcpy(format->ext, ext->valuestring);
    format->width = (uint32_t)width;
    format->height = (uint32_t)height;
    format->size_bytes = exact ? exact : estimate;
    format->size_estimated = !exact && estimate != 0;
    format->has_video = has_video;
    format->has_audio = has_audio;
    size_t slot = out->format_count;
    if (slot == SITE_GRAB_PROBE_MAX_FORMATS) {
      slot = 0;
      for (size_t j = 1; j < SITE_GRAB_PROBE_MAX_FORMATS; j++)
        if (probe_rank(&out->formats[j]) < probe_rank(&out->formats[slot])) slot = j;
      if (probe_rank(format) <= probe_rank(&out->formats[slot])) continue;
    } else {
      out->format_count++;
    }
    out->formats[slot] = parsed;
  }
  cJSON_Delete(root);
  return out->format_count != 0;
}

bool site_grab_parse_probe_output(const char *text, size_t length, SiteGrabProbe *out) {
  if (!text || !out || !length || length > SITE_PROBE_JSON_LIMIT || text[length])
    return false;
  const char *split = memchr(text, '\n', length);
  if (!split) return false;
  size_t title_length = (size_t)(split - text);
  if (title_length && text[title_length - 1] == '\r') title_length--;
  const char *formats = split + 1;
  size_t formats_length = length - (size_t)(formats - text);
  while (formats_length && (formats[formats_length - 1] == '\n' ||
                            formats[formats_length - 1] == '\r')) formats_length--;
  if (!title_length || !formats_length || memchr(formats, '\n', formats_length))
    return false;
  static const char prefix[] = "{\"title\":";
  static const char middle[] = ",\"formats\":";
  size_t total = sizeof(prefix) - 1 + title_length + sizeof(middle) - 1 +
    formats_length + 1;
  if (total > SITE_PROBE_JSON_LIMIT) return false;
  char *wrapper = malloc(total + 1);
  if (!wrapper) return false;
  size_t used = 0;
  memcpy(wrapper + used, prefix, sizeof(prefix) - 1); used += sizeof(prefix) - 1;
  memcpy(wrapper + used, text, title_length); used += title_length;
  memcpy(wrapper + used, middle, sizeof(middle) - 1); used += sizeof(middle) - 1;
  memcpy(wrapper + used, formats, formats_length); used += formats_length;
  wrapper[used++] = '}';
  wrapper[used] = '\0';
  bool ok = site_grab_parse_probe_json(wrapper, used, out);
  volatile unsigned char *private_bytes = (volatile unsigned char *)wrapper;
  for (size_t i = 0; i < used; i++) private_bytes[i] = 0;
  free(wrapper);
  return ok;
}

static bool host_is(const char *host, const char *domain) {
  size_t h = strlen(host), d = strlen(domain);
  return !strcasecmp(host, domain) ||
         (h > d && host[h - d - 1] == '.' && !strcasecmp(host + h - d, domain));
}
bool site_grab_host_allowed(const char *host) {
  if (!host)
    return false;
  return host_is(host, "youtube.com") || host_is(host, "youtu.be") ||
         host_is(host, "vimeo.com") || host_is(host, "dailymotion.com");
}
static bool public_host_allowed(const char *host) {
  if (!host) return false;
  size_t length = strlen(host);
  if (!length || length > 253 || host[length - 1] == '.') return false;
  const char *last_dot = strrchr(host, '.');
  if (!last_dot || !last_dot[1]) return false;
  bool tld_has_letter = false;
  size_t label = 0;
  for (const char *p = host; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '.') {
      if (!label || label > 63 || p[-1] == '-') return false;
      label = 0;
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-') ||
        (!label && c == '-')) return false;
    if (p > last_dot && isalpha(c)) tld_has_letter = true;
    label++;
  }
  if (!label || label > 63 || host[length - 1] == '-' || !tld_has_letter)
    return false;
  static const char *blocked[] = {
    "localhost", "local", "internal", "lan", "home", "onion", "test", "arpa"
  };
  for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++)
    if (!strcasecmp(last_dot + 1, blocked[i])) return false;
  return true;
}

bool site_grab_public_url_allowed(const char *url) {
  if (!url || strlen(url) > 2047) return false;
  CURLU *parsed = curl_url();
  char *scheme = NULL, *host = NULL, *user = NULL, *pass = NULL;
  bool ok = parsed &&
            curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
            curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
            curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
            !strcasecmp(scheme, "https") && public_host_allowed(host);
  if (ok && (curl_url_get(parsed, CURLUPART_USER, &user, 0) == CURLUE_OK ||
             curl_url_get(parsed, CURLUPART_PASSWORD, &pass, 0) == CURLUE_OK))
    ok = false;
  curl_free(scheme); curl_free(host); curl_free(user); curl_free(pass);
  curl_url_cleanup(parsed);
  return ok;
}
bool site_grab_url_allowed(const char *url) {
  if (!url)
    return false;
  CURLU *parsed = curl_url();
  char *scheme = NULL, *host = NULL, *user = NULL, *pass = NULL;
  bool ok = parsed &&
            curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
            curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
            curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
            !strcasecmp(scheme, "https") && site_grab_host_allowed(host);
  if (ok && (curl_url_get(parsed, CURLUPART_USER, &user, 0) == CURLUE_OK ||
             curl_url_get(parsed, CURLUPART_PASSWORD, &pass, 0) == CURLUE_OK))
    ok = false;
  curl_free(scheme);
  curl_free(host);
  curl_free(user);
  curl_free(pass);
  curl_url_cleanup(parsed);
  return ok;
}
static bool parse_uint(const char *text, uint64_t *value, bool unknown) {
  if (unknown && (!strcmp(text, "NA") || !strcmp(text, "None") || !*text)) {
    *value = 0;
    return true;
  }
  if (!*text)
    return false;
  uint64_t v = 0;
  for (; *text; text++) {
    if (!isdigit((unsigned char)*text) ||
        v > (UINT64_MAX - (unsigned)(*text - '0')) / 10)
      return false;
    v = v * 10 + (unsigned)(*text - '0');
  }
  *value = v;
  return true;
}
bool site_grab_parse_progress(const char *line, SiteGrabProgress *out) {
  if (!line || !out || strncmp(line, "CDM|", 4) || strlen(line) > 255)
    return false;
  char copy[256];
  strcpy(copy, line);
  char *parts[5], *cursor = copy + 4;
  for (int i = 0; i < 5; i++) {
    parts[i] = cursor;
    if (i < 4) {
      char *sep = strchr(cursor, '|');
      if (!sep)
        return false;
      *sep = 0;
      cursor = sep + 1;
    } else if (strchr(cursor, '|'))
      return false;
    while (isspace((unsigned char)*parts[i]))
      parts[i]++;
    char *end = parts[i] + strlen(parts[i]);
    while (end > parts[i] && isspace((unsigned char)end[-1]))
      *--end = 0;
  }
  SiteGrabProgress parsed = {.eta_seconds = UINT64_MAX, .percent = -1};
  uint64_t eta = 0;
  if (!parse_uint(parts[0], &parsed.downloaded_bytes, false) ||
      !parse_uint(parts[1], &parsed.total_bytes, true))
    return false;
  if (strcmp(parts[2], "NA") && strcmp(parts[2], "None") && *parts[2]) {
    char *end = NULL;
    errno = 0;
    long double speed = strtold(parts[2], &end);
    if (errno || end == parts[2] || *end || !isfinite(speed) || speed < 0 ||
        speed >= (long double)UINT64_MAX)
      return false;
    parsed.speed_bps = (uint64_t)speed;
  }
  if (strcmp(parts[3], "NA") && strcmp(parts[3], "None") && *parts[3]) {
    if (!parse_uint(parts[3], &eta, false))
      return false;
    parsed.eta_seconds = eta;
  }
  if (strcmp(parts[4], "NA") && strcmp(parts[4], "None") && *parts[4]) {
    char *end = NULL;
    errno = 0;
    parsed.percent = strtod(parts[4], &end);
    if (errno || end == parts[4] || (*end != '%' && *end) ||
        (*end == '%' && end[1]) || !isfinite(parsed.percent) ||
        parsed.percent < 0 || parsed.percent > 100)
      return false;
  }
  if (parsed.total_bytes && parsed.downloaded_bytes > parsed.total_bytes)
    return false;
  *out = parsed;
  return true;
}

#include "../core/queue_manager.h"
#include "../persistence/db.h"
#include "../platform/spawn.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../utils/path.h"
#include "finalize.h"

int site_grab_probe_with_consent(const char *url, bool public_site,
                                const _Atomic bool *cancel, SiteGrabProbe *out) {
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (!url || strlen(url) > 2047 ||
      !(public_site ? site_grab_public_url_allowed(url)
                    : site_grab_url_allowed(url))) return -1;
  DownloadManagerConfig config;
  config_get(&config);
  if (!config.use_yt_dlp || !spawn_site_tool_available()) return -1;
  char *json = malloc(SITE_PROBE_JSON_LIMIT + 1);
  if (!json) return -1;
  size_t length = 0;
  char *argv[] = {"yt-dlp", "--ignore-config", "--no-playlist", "--no-cache-dir",
    "--simulate", "--no-warnings", "--no-progress",
    "--print", "%(title)j", "--print",
    "%(formats.:.{format_id,ext,width,height,filesize,filesize_approx,vcodec,acodec})j",
    "--", (char *)url, NULL};
  int timeout = config.transfer_timeout_sec < 30 ? config.transfer_timeout_sec : 30;
  int rc = spawn_site_tool_capture(argv, cancel, timeout, json,
                                   SITE_PROBE_JSON_LIMIT + 1, &length);
  if (rc == 0 && !site_grab_parse_probe_output(json, length, out)) rc = -1;
  volatile unsigned char *private_bytes = (volatile unsigned char *)json;
  for (size_t i = 0; i < length; i++) private_bytes[i] = 0;
  free(json);
  return rc;
}
int site_grab_probe(const char *url, const _Atomic bool *cancel, SiteGrabProbe *out) {
  return site_grab_probe_with_consent(url, false, cancel, out);
}
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static bool site_owned(int fd) {
  struct stat st;
  return fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
         st.st_uid == getuid() && st.st_nlink == 1;
}
static bool site_dir(const char *dir) {
  struct stat st;
  return lstat(dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
         !(st.st_mode & 077);
}
static bool site_path(const char *base, const char *part, char *out,
                      size_t size) {
  int n = snprintf(out, size, "%s%s", base, part);
  return n >= 0 && (size_t)n < size;
}
static bool stage_marker(const char *directory, bool created) {
  char marker[1300];
  if (!site_path(directory, "/.cdm-site-v1", marker, sizeof(marker)))
    return false;
  int fd = open(marker,
                (created ? O_CREAT | O_EXCL | O_WRONLY : O_RDONLY) |
                    O_NOFOLLOW | O_CLOEXEC,
                0600);
  if (!site_owned(fd)) {
    if (fd >= 0)
      close(fd);
    return false;
  }
  char expected = '1', actual = 0;
  bool ok = created ? write(fd, &expected, 1) == 1 && fsync(fd) == 0
                    : read(fd, &actual, 1) == 1 && actual == expected;
  close(fd);
  return ok;
}
static bool clean_asset_files(const char *directory) {
  DIR *dir = opendir(directory);
  if (!dir)
    return false;
  struct dirent *entry;
  bool ok = true;
  char path[1300];
  while ((entry = readdir(dir))) {
    if (strncmp(entry->d_name, "asset.", 6))
      continue;
    if (!site_path(directory, "/", path, sizeof(path)) ||
        strlen(path) + strlen(entry->d_name) >= sizeof(path)) {
      ok = false;
      break;
    }
    strcat(path, entry->d_name);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    bool owned = site_owned(fd);
    if (fd >= 0)
      close(fd);
    if (!owned || unlink(path) != 0) {
      ok = false;
      break;
    }
  }
  closedir(dir);
  return ok;
}
static bool stopped(struct Download *d) {
  return atomic_load(&d->cancel_requested) || atomic_load(&d->pause_requested);
}
static void redact_child_error(const char *input, char out[192]) {
  const char *text = input;
  if (strstr(input, "://") || strchr(input, '/') ||
      strcasestr(input, "cookie") || strcasestr(input, "token") ||
      strcasestr(input, "password"))
    text = "[REDACTED]";
  size_t n = 0;
  for (; text[n] && n < 190; n++)
    out[n] = isprint((unsigned char)text[n]) ? text[n] : '?';
  out[n] = 0;
}
typedef struct {
  struct Download *download;
  char last_error[192];
} SiteRun;
static void site_line(const char *line, bool stderr_line, void *userdata) {
  SiteRun *run = userdata;
  if (stderr_line) {
    redact_child_error(line, run->last_error);
    return;
  }
  SiteGrabProgress p;
  if (!site_grab_parse_progress(line, &p))
    return;
  Download *d = run->download;
  uint64_t old = atomic_load(&d->bytes_downloaded);
  if (p.downloaded_bytes > old)
    atomic_store(&d->bytes_downloaded, p.downloaded_bytes);
  dm_mutex_t *mutex = queue_manager_get_mutex();
  dm_mutex_lock(mutex);
  if (p.total_bytes > d->total_size)
    d->total_size = p.total_bytes;
  if (p.percent >= 0)
    d->progress = (float)(p.percent / 100.0);
  d->transfer_metrics.speed_bps = p.speed_bps;
  d->transfer_metrics.eta_seconds = p.eta_seconds;
  dm_mutex_unlock(mutex);
}
static bool publish_asset(const char *source, const char *requested,
                          char *published) {
  for (int i = 0; i < 1000000; i++) {
    if (!path_make_unique(requested, published, 1024))
      return false;
    if (link(source, published) == 0)
      return true;
    if (errno != EEXIST)
      return false;
  }
  return false;
}
static bool sync_site_parent(const char *path) {
  char parent[1024];
  if (strlen(path) >= sizeof(parent))
    return false;
  strcpy(parent, path);
  char *slash = strrchr(parent, '/');
  if (!slash)
    strcpy(parent, ".");
  else if (slash == parent)
    slash[1] = 0;
  else
    *slash = 0;
  int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  bool ok = fd >= 0 && fsync(fd) == 0;
  if (fd >= 0)
    close(fd);
  return ok;
}
int site_grab_run_download(struct Download *d) {
  DownloadManagerConfig config;
  config_get(&config);
  if (!d->site_grab || !config.use_yt_dlp || !spawn_site_tool_available() ||
      !(d->site_grab_public ? site_grab_public_url_allowed(d->url)
                            : site_grab_url_allowed(d->url)) ||
      d->requires_browser_context ||
      (d->request && d->request->browser_context))
    return -1;
  char original[1024], directory[1200], lock_path[1300], template[1300];
  strcpy(original, d->dest_path);
  if (!site_path(original, ".siteparts", directory, sizeof(directory)) ||
      !site_path(directory, "/lock", lock_path, sizeof(lock_path)) ||
      !site_path(directory, "/asset.%(ext)s", template, sizeof(template)))
    return -1;
  int reserved = open(original, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat claim;
  if (!d->reserved_file || !site_owned(reserved) ||
      fstat(reserved, &claim) != 0 || claim.st_size != 0) {
    if (reserved >= 0)
      close(reserved);
    return -3;
  }
  bool created = mkdir(directory, 0700) == 0;
  if ((!created && errno != EEXIST) || !site_dir(directory) ||
      !stage_marker(directory, created)) {
    close(reserved);
    return -1;
  }
  int lock = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (!site_owned(lock) || flock(lock, LOCK_EX | LOCK_NB) != 0) {
    if (lock >= 0)
      close(lock);
    close(reserved);
    return -1;
  }
  int result = -1;
  if (!clean_asset_files(directory) || stopped(d))
    goto finish;
  queue_manager_set_site_error(d->id, "");
  uint64_t limit = d->request ? d->request->speed_limit_bps : 0;
  if (config.max_speed_bytes_per_sec &&
      (!limit || config.max_speed_bytes_per_sec < limit))
    limit = config.max_speed_bytes_per_sec;
  char rate[32];
  if (limit) {
    int n = snprintf(rate, sizeof(rate), "%llu", (unsigned long long)limit);
    if (n < 0 || (size_t)n >= sizeof(rate))
      goto finish;
  }
  char *argv[22];
  char selected_format[128];
  const char *format = config.yt_dlp_format;
  if (d->site_format_id[0]) {
    if (!site_grab_format_id_valid(d->site_format_id))
      goto finish;
    int n = d->site_format_has_audio
        ? snprintf(selected_format, sizeof(selected_format), "%s", d->site_format_id)
        : snprintf(selected_format, sizeof(selected_format), "%s+bestaudio/%s",
                   d->site_format_id, d->site_format_id);
    if (n < 0 || (size_t)n >= sizeof(selected_format))
      goto finish;
    format = selected_format;
  }
  size_t argc = 0;
  argv[argc++] = "yt-dlp";
  argv[argc++] = "--ignore-config";
  argv[argc++] = "--no-playlist";
  argv[argc++] = "--no-cache-dir";
  argv[argc++] = "--no-simulate";
  argv[argc++] = "--newline";
  argv[argc++] = "--progress-template";
  argv[argc++] =
      "download:CDM|%(progress.downloaded_bytes)s|%(progress.total_"
      "bytes_estimate)s|%(progress.speed)s|%(progress.eta)s|%("
      "progress._percent_str)s";
  argv[argc++] = "--print";
  argv[argc++] = "after_move:CDMFILE:%(filepath)s";
  if (limit) {
    argv[argc++] = "-r";
    argv[argc++] = rate;
  }
  argv[argc++] = "-f";
  argv[argc++] = (char *)format;
  argv[argc++] = "-o";
  argv[argc++] = template;
  argv[argc++] = "--";
  argv[argc++] = d->url;
  argv[argc] = NULL;
  SiteRun run = {.download = d};
  int status = spawn_site_tool(argv, &d->cancel_requested, &d->pause_requested,
                               config.transfer_timeout_sec, site_line, &run);
  if (status != 0) {
    char message[256];
    int n = snprintf(message, sizeof(message), "yt-dlp exit %d: %s", status,
                     run.last_error[0] ? run.last_error : "no diagnostic");
    if (n >= 0 && (size_t)n < sizeof(message))
      queue_manager_set_site_error(d->id, message);
    if (!stopped(d))
      LOG_WARN("Site download %u: yt-dlp exited %d", d->id, status);
    goto finish;
  }
  DIR *folder = opendir(directory);
  char asset[1300] = "", extension[16] = "";
  unsigned files = 0;
  if (!folder)
    goto finish;
  struct dirent *entry;
  while ((entry = readdir(folder)))
    if (!strncmp(entry->d_name, "asset.", 6)) {
      const char *ext = entry->d_name + 6;
      size_t len = strlen(ext);
      bool valid = len > 0 && len < sizeof(extension);
      for (size_t i = 0; valid && i < len; i++)
        valid = isalnum((unsigned char)ext[i]) != 0;
      if (!valid || !site_path(directory, "/", asset, sizeof(asset)) ||
          strlen(asset) + strlen(entry->d_name) >= sizeof(asset)) {
        files = 2;
        break;
      }
      strcat(asset, entry->d_name);
      strcpy(extension, ext);
      files++;
    }
  closedir(folder);
  if (files != 1 || stopped(d))
    goto finish;
  int artifact = open(asset, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat produced;
  bool valid = site_owned(artifact) && fstat(artifact, &produced) == 0 &&
               produced.st_size > 0 && fsync(artifact) == 0;
  if (artifact >= 0)
    close(artifact);
  if (!valid || produced.st_size > INT64_MAX)
    goto finish;
  if (d->request && d->request->expected_sha256[0] &&
      engine_finalize(asset, 0, d->request->expected_sha256) != 0) {
    result = -2;
    goto finish;
  }
  char requested[1024], published[1024];
  size_t stem = strlen(original);
  const char *slash = strrchr(original, '/'), *dot = strrchr(original, '.');
  if (dot && (!slash || dot > slash))
    stem = (size_t)(dot - original);
  if (stem + strlen(extension) + 2 > sizeof(requested))
    goto finish;
  memcpy(requested, original, stem);
  requested[stem] = '.';
  strcpy(requested + stem + 1, extension);
  if (!publish_asset(asset, requested, published))
    goto finish;
  if (!sync_site_parent(published)) {
    unlink(published);
    goto finish;
  }
  struct stat current;
  bool unchanged = lstat(original, &current) == 0 &&
                   current.st_dev == claim.st_dev &&
                   current.st_ino == claim.st_ino && current.st_size == 0;
  dm_mutex_t *mutex = queue_manager_get_mutex();
  dm_mutex_lock(mutex);
  result =
      unchanged && !stopped(d)
          ? db_update_media_output(d->id, published, (uint64_t)produced.st_size)
          : -1;
  if (result == 0) {
    strcpy(d->dest_path, published);
    d->total_size = (uint64_t)produced.st_size;
    d->progress = 1.0f;
    atomic_store(&d->auto_filename, false);
    atomic_store(&d->bytes_downloaded, d->total_size);
  }
  dm_mutex_unlock(mutex);
  if (result == 0) {
    if (unlink(original) != 0)
      LOG_WARN("Site download %u: old reservation retained", d->id);
    if (unlink(asset) != 0)
      LOG_WARN("Site download %u: private asset retained", d->id);
  } else
    unlink(published);
finish:
  if (result == 0 || result == -2 || atomic_load(&d->cancel_requested))
    clean_asset_files(directory);
  close(reserved);
  flock(lock, LOCK_UN);
  close(lock);
  if (result == 0 || result == -2 || atomic_load(&d->cancel_requested)) {
    char marker[1300];
    if (site_path(directory, "/.cdm-site-v1", marker, sizeof(marker)))
      unlink(marker);
    unlink(lock_path);
    rmdir(directory);
  }
  if (result == -2)
    unlink(original);
  return result;
}
#else
/* TODO(platform): site process staging, polling and publication need Windows.
 */
int site_grab_run_download(struct Download *d) {
  (void)d;
  return -1;
}
#endif
