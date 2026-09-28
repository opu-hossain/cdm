// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "queue_manager.h"
#include "../engine/hls.h"
#include "../engine/dash.h"
#include "scheduler.h"
#include "../persistence/db.h"
#include "../platform/thread.h"
#include "../platform/curl_client.h"
#include "../utils/log.h"
#include "../utils/path.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

/* Internal state */
static Download *g_head = NULL;
static uint32_t g_next_id = 1;
static dm_mutex_t g_mutex;
static once_flag g_mutex_once = ONCE_FLAG_INIT;

bool category_for_filename(const char *name, Category *out) {
  if (!out)
    return false;
  Category *categories = NULL;
  size_t count = 0;
  if (db_category_list(&categories, &count) != 0)
    return false;
  bool found_default = false;
  size_t best_length = 0;
  for (size_t i = 0; i < count; ++i) {
    if (categories[i].id == 1) {
      *out = categories[i];
      found_default = true;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    size_t list_length = strlen(categories[i].extensions);
    if (categories[i].id != 1 && list_length > best_length &&
        path_extension_in_list(name, categories[i].extensions)) {
      *out = categories[i];
      best_length = list_length;
    }
  }
  free(categories);
  return found_default;
}

static bool request_options_present(const RequestOptions *opts) {
  return opts &&
         (opts->cookie[0] != '\0' || opts->referrer[0] != '\0' ||
          opts->extra_headers[0] != '\0' || opts->expected_sha256[0] != '\0' ||
          opts->speed_limit_bps != 0 || opts->auth_user[0] != '\0' ||
          opts->auth_password[0] != '\0');
}

static void clear_browser_request(Download *download) {
  if (!download->request || !download->request->browser_context)
    return;
  volatile unsigned char *bytes = (volatile unsigned char *)download->request;
  for (size_t i = 0; i < sizeof(*download->request); i++) bytes[i] = 0;
  free(download->request);
  download->request = NULL;
}

static void free_download(Download *download) {
  if (download->media_kind == DOWNLOAD_MEDIA_HLS)
    hls_discard_state(download->dest_path);
  if (download->media_kind == DOWNLOAD_MEDIA_DASH) dash_discard_state(download->dest_path);
  clear_browser_request(download);
  free(download->request);
  free(download);
}

/* Static helpers */

/**
 * Return the filesystem root under which all downloads must reside.
 *
 * Defaults to $HOME; override with DOWNLOADMGR_ROOT.
 */
static const char *get_allowed_root(void) {
  const char *configured = getenv("DOWNLOADMGR_ROOT");
  const char *home = getenv("HOME");
  return configured ? configured : (home ? home : "/tmp");
}

/**
 * Validate that the directory part of `path` resolves (after symlinks and
 * ".." collapsing) to somewhere inside get_allowed_root(), and that the
 * filename component contains no path separators.
 *
 * @return true if the destination is safe to write to
 */
static bool is_safe_dest_path(const char *path) {
  if (!path || path[0] == '\0' || strlen(path) >= 900) {
    LOG_WARN("is_safe_dest_path: rejected (null/empty/too long)");
    return false;
  }

  char dir_buf[1024];
  strncpy(dir_buf, path, sizeof(dir_buf) - 1);
  dir_buf[sizeof(dir_buf) - 1] = '\0';
  char *dir = dirname(dir_buf);

  char temp_dir[1024];
  strncpy(temp_dir, dir, sizeof(temp_dir) - 1);
  temp_dir[sizeof(temp_dir) - 1] = '\0';

  char resolved_dir[PATH_MAX];
  while (realpath(temp_dir, resolved_dir) == NULL) {
    if (errno != ENOENT) {
      LOG_WARN("is_safe_dest_path: rejected — realpath('%s') failed: %s",
               temp_dir, strerror(errno));
      return false;
    }
    char previous[sizeof(temp_dir)];
    strcpy(previous, temp_dir);
    char *parent = dirname(temp_dir);
    if (!parent || strcmp(parent, previous) == 0 || strcmp(parent, ".") == 0 ||
        strcmp(parent, "/") == 0) {
      LOG_WARN("is_safe_dest_path: rejected — no valid existing parent "
               "directory for '%s'",
               dir);
      return false;
    }
    char parent_copy[1024];
    strncpy(parent_copy, parent, sizeof(parent_copy) - 1);
    parent_copy[sizeof(parent_copy) - 1] = '\0';
    strncpy(temp_dir, parent_copy, sizeof(temp_dir) - 1);
    temp_dir[sizeof(temp_dir) - 1] = '\0';
  }

  const char *configured_root = get_allowed_root();
  char resolved_root[PATH_MAX];
  if (realpath(configured_root, resolved_root) == NULL) {
    LOG_WARN("is_safe_dest_path: rejected — allowed root '%s' is invalid",
             configured_root);
    return false;
  }

  size_t root_len = strlen(resolved_root);
  if (strncmp(resolved_dir, resolved_root, root_len) != 0) {
    LOG_WARN("is_safe_dest_path: rejected — '%s' outside allowed root '%s'",
             resolved_dir, resolved_root);
    return false;
  }
  if (resolved_dir[root_len] != '\0' && resolved_dir[root_len] != '/')
    return false;

  char base_buf[1024];
  strncpy(base_buf, path, sizeof(base_buf) - 1);
  base_buf[sizeof(base_buf) - 1] = '\0';
  char *base = basename(base_buf);
  if (strchr(base, '/') != NULL || strcmp(base, "..") == 0 ||
      strcmp(base, ".") == 0)
    return false;

  return true;
}

/**
 * One‑time initialisation of the global mutex.
 */
static void initialize_mutex(void) { dm_mutex_init(&g_mutex); }

static void ensure_mutex(void) { call_once(&g_mutex_once, initialize_mutex); }

/* Lifecycle */

static uint32_t add_download(const char *url, const char *dest_path,
                             const RequestOptions *opts, bool auto_filename) {
  ensure_mutex();

  if (!is_safe_dest_path(dest_path))
    return 0;

  Download *d = calloc(1, sizeof(Download));
  if (!d)
    return 0;

  strncpy(d->url, url, sizeof(d->url) - 1);
  d->url[sizeof(d->url) - 1] = '\0';
  strncpy(d->dest_path, dest_path, sizeof(d->dest_path) - 1);
  d->dest_path[sizeof(d->dest_path) - 1] = '\0';
  atomic_store(&d->auto_filename, auto_filename);
  d->media_kind = opts ? opts->media_kind : DOWNLOAD_MEDIA_NONE;
  d->site_grab = opts && opts->site_grab;
  d->requires_browser_context = opts && opts->browser_context;

  if (request_options_present(opts) || d->requires_browser_context) {
    d->request = malloc(sizeof(*d->request));
    if (!d->request) {
      free(d);
      return 0;
    }
    *d->request = *opts;
  }

  d->status = DOWNLOAD_QUEUED;
  d->priority = 0;
  d->queue_id = opts && opts->queue_id ? opts->queue_id : 1;
  d->created_at = time(NULL);
  d->transfer_metrics.eta_seconds = UINT64_MAX;

  dm_mutex_lock(&g_mutex);
  d->id = g_next_id++;
  d->next = g_head;
  g_head = d;
  dm_mutex_unlock(&g_mutex);

  return d->id;
}

uint32_t queue_manager_add(const char *url, const char *dest_path,
                           const RequestOptions *opts) {
  return add_download(url, dest_path, opts, false);
}

uint32_t queue_manager_add_auto(const char *url, const char *dest_path,
                                const RequestOptions *opts) {
  return add_download(url, dest_path, opts, true);
}

void queue_manager_add_existing(Download *d) {
  ensure_mutex();

  dm_mutex_lock(&g_mutex);
  d->transfer_metrics = (DownloadTransferMetrics){.eta_seconds = UINT64_MAX};
  if (d->id >= g_next_id)
    g_next_id = d->id + 1;
  d->next = g_head;
  g_head = d;
  dm_mutex_unlock(&g_mutex);
}

void queue_manager_seed_next_id(uint32_t min_next_id) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  if (min_next_id > g_next_id)
    g_next_id = min_next_id;
  dm_mutex_unlock(&g_mutex);
}

void queue_manager_remove(uint32_t id) {
  ensure_mutex();

  dm_mutex_lock(&g_mutex);
  Download **prev_ptr = &g_head;
  while (*prev_ptr != NULL) {
    Download *cur = *prev_ptr;
    if (cur->id == id) {
      if (cur->status == DOWNLOAD_ACTIVE) {
        atomic_store(&cur->cancel_requested, true);
        dm_mutex_unlock(&g_mutex);
        return;
      }
      *prev_ptr = cur->next;
      if (cur->dest_path[0] != '\0')
        unlink(cur->dest_path);
      free_download(cur);
      break;
    }
    prev_ptr = &cur->next;
  }
  dm_mutex_unlock(&g_mutex);
}

bool queue_manager_forget_locked(uint32_t id) {
  Download **prev_ptr = &g_head;
  while (*prev_ptr != NULL) {
    Download *current = *prev_ptr;
    if (current->id == id) {
      if (current->status == DOWNLOAD_ACTIVE)
        return false;
      *prev_ptr = current->next;
      free_download(current);
      return true;
    }
    prev_ptr = &current->next;
  }
  return false;
}

bool queue_manager_can_forget_locked(uint32_t id) {
  for (Download *current = g_head; current; current = current->next) {
    if (current->id == id)
      return current->status != DOWNLOAD_ACTIVE;
  }
  return true;
}

/* Query */

Download *queue_manager_find_by_id(uint32_t id) {
  ensure_mutex();
  Download *found = NULL;

  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id == id) {
      found = cur;
      break;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return found;
}

Download *queue_manager_find_next_queued(void) {
  /* Caller must hold g_mutex. */
  Download *best = NULL;
  time_t now = time(NULL);
  Queue *queues = NULL;
  size_t queue_count = 0;
  if (queue_list(&queues, &queue_count) != 0)
    return NULL;
  int best_queue_priority = 0;

  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->status != DOWNLOAD_QUEUED)
      continue;
    if (cur->next_retry_at != 0 && cur->next_retry_at > now)
      continue; // backoff period still active
    uint32_t queue_id = cur->queue_id ? cur->queue_id : 1;
    int queue_priority = 0;
    int max_concurrent = 0;
    for (size_t i = 0; i < queue_count; i++) {
      if (queues[i].id == queue_id) {
        if (scheduler_queue_state(&queues[i], now) == SCHEDULED_IDLE) {
          queue_id = 0;
          break;
        }
        queue_priority = queues[i].priority;
        max_concurrent = queues[i].max_concurrent;
        break;
      }
    }
    if (queue_id == 0)
      continue;
    if (max_concurrent > 0) {
      int active_in_queue = 0;
      for (Download *other = g_head; other; other = other->next) {
        uint32_t other_queue = other->queue_id ? other->queue_id : 1;
        if (other_queue == queue_id && other->status == DOWNLOAD_ACTIVE)
          active_in_queue++;
      }
      if (active_in_queue >= max_concurrent)
        continue;
    }
    if (best == NULL || queue_priority > best_queue_priority ||
        (queue_priority == best_queue_priority &&
         (cur->created_at < best->created_at ||
          (cur->created_at == best->created_at &&
           (cur->priority > best->priority ||
            (cur->priority == best->priority && cur->id < best->id)))))) {
      best = cur;
      best_queue_priority = queue_priority;
    }
  }
  free(queues);
  return best;
}

int queue_manager_apply_schedule(uint32_t queue_id, bool active,
                                 uint32_t *resumed_ids, int capacity) {
  if (!queue_id || capacity < 0 || (capacity && !resumed_ids))
    return -1;
  ensure_mutex();
  int resumed = 0;
  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur; cur = cur->next) {
    if ((cur->queue_id ? cur->queue_id : 1) != queue_id)
      continue;
    if (!active && cur->status == DOWNLOAD_ACTIVE &&
        !cur->schedule_paused && !atomic_load(&cur->pause_requested)) {
      if (db_set_schedule_paused(cur->id, true) == 0) {
        cur->schedule_paused = true;
        atomic_store(&cur->pause_requested, true);
      } else
        LOG_WARN("Could not persist scheduled pause for download %u", cur->id);
    } else if (active && cur->schedule_paused) {
      if (cur->status == DOWNLOAD_PAUSED) {
        if (cur->requires_browser_context && !cur->request)
          continue;
        if (resumed >= capacity)
          continue;
        if (db_resume_scheduled_download(cur->id) != 0) {
          LOG_WARN("Could not resume scheduled download %u", cur->id);
          continue;
        }
        cur->schedule_paused = false;
        atomic_store(&cur->pause_requested, false);
        cur->retry_count = 0;
        cur->next_retry_at = 0;
        cur->status = DOWNLOAD_QUEUED;
        resumed_ids[resumed++] = cur->id;
      } else if (cur->status != DOWNLOAD_ACTIVE &&
                 db_set_schedule_paused(cur->id, false) == 0) {
        cur->schedule_paused = false;
      }
    }
  }
  dm_mutex_unlock(&g_mutex);
  return resumed;
}

int queue_manager_count_by_status(DownloadStatus s) {
  ensure_mutex();
  int count = 0;

  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->status == s)
      count++;
  }
  dm_mutex_unlock(&g_mutex);
  return count;
}

int queue_manager_count_by_status_locked(DownloadStatus s) {
  int count = 0;
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->status == s)
      count++;
  }
  return count;
}

bool queue_manager_get_status(uint32_t id, DownloadStatus *out_status) {
  if (!out_status)
    return false;
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id == id) {
      *out_status = cur->status;
      dm_mutex_unlock(&g_mutex);
      return true;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return false;
}

bool queue_manager_get_media_kind(uint32_t id, DownloadMediaKind *out) {
  if (!out)
    return false;
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (Download *d = g_head; d; d = d->next) {
    if (d->id == id) {
      *out = d->media_kind;
      dm_mutex_unlock(&g_mutex);
      return true;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return false;
}
bool queue_manager_get_site_grab(uint32_t id, bool *out) {
  if (!out)
    return false;
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  bool found = false;
  for (Download *d = g_head; d; d = d->next)
    if (d->id == id) {
      *out = d->site_grab;
      found = true;
      break;
    }
  dm_mutex_unlock(&g_mutex);
  return found;
}
bool queue_manager_get_error(uint32_t id, char *out, size_t capacity) {
  if (!out || !capacity)
    return false;
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  bool found = false;
  for (Download *d = g_head; d; d = d->next)
    if (d->id == id) {
      size_t length = strlen(d->last_error);
      if (length >= capacity)
        length = capacity - 1;
      memcpy(out, d->last_error, length);
      out[length] = 0;
      found = true;
      break;
    }
  dm_mutex_unlock(&g_mutex);
  return found;
}
void queue_manager_set_site_error(uint32_t id, const char *error) {
  if (!error || strlen(error) >= 256)
    return;
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (Download *d = g_head; d; d = d->next)
    if (d->id == id) {
      if (db_update_site_error(id, error) == 0)
        strcpy(d->last_error, error);
      break;
    }
  dm_mutex_unlock(&g_mutex);
}

bool queue_manager_get_runtime_snapshot(uint32_t id,
                                        DownloadRuntimeSnapshot *out) {
  if (!out)
    return false;
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id == id) {
      out->media_kind = cur->media_kind;
      out->status = cur->status;
      out->total_size = cur->total_size;
      out->bytes_downloaded = atomic_load(&cur->bytes_downloaded);
      out->transfer_metrics = cur->transfer_metrics;
      memcpy(out->dest_path, cur->dest_path, sizeof(out->dest_path));
      out->chunk_count = cur->chunk_count;
      if (out->chunk_count > QM_MAX_CHUNKS)
        out->chunk_count = QM_MAX_CHUNKS;
      memcpy(out->chunks, cur->chunks,
             (size_t)out->chunk_count * sizeof(out->chunks[0]));
      dm_mutex_unlock(&g_mutex);
      return true;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return false;
}

int queue_manager_snapshot_active_progress(DownloadProgressSnapshot *out,
                                           int max) {
  ensure_mutex();
  int n = 0;
  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL && n < max; cur = cur->next) {
    if (cur->status == DOWNLOAD_ACTIVE) {
      out[n].id = cur->id;
      out[n].bytes_downloaded = atomic_load(&cur->bytes_downloaded);
      out[n].total_size = cur->total_size;
      out[n].transfer_metrics = cur->transfer_metrics;
      n++;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return n;
}

void queue_manager_set_transfer_metrics(uint32_t id,
                                        DownloadTransferMetrics metrics) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id == id && cur->status == DOWNLOAD_ACTIVE) {
      cur->transfer_metrics = metrics;
      break;
    }
  }
  dm_mutex_unlock(&g_mutex);
}

int queue_manager_snapshot_chunk_progress(ChunkProgressSnapshot *out, int max) {
  ensure_mutex();
  int n = 0;
  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL && n < max; cur = cur->next) {
    if (cur->status != DOWNLOAD_ACTIVE)
      continue;
    for (int j = 0; j < cur->chunk_count && n < max; j++) {
      out[n].download_id = cur->id;
      out[n].range_start = cur->chunks[j].range_start;
      out[n].bytes_done = atomic_load(&cur->chunk_live_bytes[j]);
      n++;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return n;
}

/* Status / control */

void queue_manager_update_status(uint32_t id, DownloadStatus new_status) {
  ensure_mutex();

  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id == id) {
      if (new_status == DOWNLOAD_ACTIVE && cur->status != DOWNLOAD_ACTIVE)
        cur->transfer_metrics =
            (DownloadTransferMetrics){.eta_seconds = UINT64_MAX};
      cur->status = new_status;
      if (new_status == DOWNLOAD_DONE || new_status == DOWNLOAD_ERROR ||
          new_status == DOWNLOAD_CANCELED)
        clear_browser_request(cur);
      break;
    }
  }
  dm_mutex_unlock(&g_mutex);
}

bool queue_manager_cancel(uint32_t id) {
  ensure_mutex();
  bool was_active = false;

  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id == id) {
      if (cur->status == DOWNLOAD_ACTIVE) {
        atomic_store(&cur->cancel_requested, true);
        was_active = true;
      } else {
        cur->status = DOWNLOAD_CANCELED;
        clear_browser_request(cur);
      }
      break;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return was_active;
}

void queue_manager_clear_resume_state(uint32_t id) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id != id)
      continue;
    if (cur->media_kind == DOWNLOAD_MEDIA_HLS) hls_discard_state(cur->dest_path);
    if (cur->media_kind == DOWNLOAD_MEDIA_DASH) dash_discard_state(cur->dest_path);
    memset(cur->chunks, 0, sizeof(cur->chunks));
    cur->chunk_count = 0;
    cur->total_size = 0;
    cur->progress = 0.0f;
    cur->retry_count = 0;
    cur->next_retry_at = 0;
    atomic_store(&cur->bytes_downloaded, 0);
    for (int i = 0; i < QM_MAX_CHUNKS; i++)
      atomic_store(&cur->chunk_live_bytes[i], 0);
    break;
  }
  dm_mutex_unlock(&g_mutex);
}

bool queue_manager_pause(uint32_t id) {
  ensure_mutex();
  bool was_active = false;

  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id == id) {
      if (cur->status == DOWNLOAD_ACTIVE) {
        atomic_store(&cur->pause_requested, true);
        was_active = true;
      } else if (cur->status == DOWNLOAD_QUEUED) {
        cur->status = DOWNLOAD_PAUSED;
      }
      break;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return was_active;
}

bool queue_manager_resume(uint32_t id) {
  ensure_mutex();
  bool resumed = false;

  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id == id) {
      if (cur->requires_browser_context && !cur->request)
        break;
      if (cur->status == DOWNLOAD_PAUSED || cur->status == DOWNLOAD_ERROR) {
        if (cur->schedule_paused) {
          int persisted = cur->status == DOWNLOAD_PAUSED
                              ? db_resume_scheduled_download(cur->id)
                              : db_set_schedule_paused(cur->id, false);
          if (persisted != 0)
            break;
        }
        cur->schedule_paused = false;
        atomic_store(&cur->pause_requested, false);
        atomic_store(&cur->cancel_requested, false);
        cur->retry_count = 0;
        cur->next_retry_at = 0;
        cur->status = DOWNLOAD_QUEUED;
        resumed = true;
      }
      break;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return resumed;
}

int queue_manager_refresh_browser_context(uint32_t id,
                                           const RequestOptions *options) {
  if (!options || !options->browser_context)
    return false;
  ensure_mutex();
  int refreshed = 0;
  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur; cur = cur->next) {
    if (cur->id != id) continue;
    if (cur->requires_browser_context && !cur->request &&
        (cur->status == DOWNLOAD_PAUSED || cur->status == DOWNLOAD_ERROR)) {
      RequestOptions *copy = malloc(sizeof(*copy));
      if (!copy) { refreshed = -1; break; }
      int saved = cur->schedule_paused && cur->status == DOWNLOAD_PAUSED
                      ? db_resume_scheduled_download(cur->id)
                      : db_update_status(cur->id, "QUEUED");
      if (saved != 0) { free(copy); refreshed = -1; break; }
      *copy = *options;
      cur->request = copy;
      cur->schedule_paused = false;
      atomic_store(&cur->pause_requested, false);
      atomic_store(&cur->cancel_requested, false);
      cur->retry_count = 0;
      cur->next_retry_at = 0;
      cur->status = DOWNLOAD_QUEUED;
      refreshed = 1;
    }
    break;
  }
  dm_mutex_unlock(&g_mutex);
  return refreshed;
}

void queue_manager_clear_browser_contexts(void) {
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur; cur = cur->next)
    clear_browser_request(cur);
  dm_mutex_unlock(&g_mutex);
}

/* Internal (use with care) */

void queue_manager_reassign_queue_locked(uint32_t old_id) {
  if (old_id <= 1)
    return;
  for (Download *cur = g_head; cur; cur = cur->next)
    if (cur->queue_id == old_id)
      cur->queue_id = 1;
}

void *queue_manager_get_mutex(void) {
  ensure_mutex();
  return &g_mutex;
}

int queue_manager_refresh_url(uint32_t id) {
  ensure_mutex();
  char url[2048], etag[256], modified[128];
  RequestOptions options = {0};
  DownloadStatus status;
  uint64_t size, bytes;
  int chunks;
  volatile unsigned char *secret = (volatile unsigned char *)&options;
  dm_mutex_lock(&g_mutex);
  Download *d = g_head;
  while (d && d->id != id) d = d->next;
  if (!d) { dm_mutex_unlock(&g_mutex); return IPC_RESULT_NOT_FOUND; }
  if (d->status == DOWNLOAD_ACTIVE ||
      (d->requires_browser_context && !d->request)) {
    dm_mutex_unlock(&g_mutex);
    return IPC_RESULT_REJECTED;
  }
  memcpy(url, d->url, sizeof(url));
  memcpy(etag, d->etag, sizeof(etag));
  memcpy(modified, d->last_modified, sizeof(modified));
  if (d->request) options = *d->request;
  status = d->status;
  size = d->total_size;
  bytes = atomic_load(&d->bytes_downloaded);
  chunks = d->chunk_count;
  dm_mutex_unlock(&g_mutex);

  RequestContext context = {
      .cookie = options.cookie, .referrer = options.referrer,
      .extra_headers = options.extra_headers, .auth_user = options.auth_user,
      .auth_password = options.auth_password,
      .user_agent = options.user_agent[0] ? options.user_agent : NULL,
      .sensitive = options.browser_context};
  FileInfo info = {0};
  int result = IPC_RESULT_ERROR;
  if (curl_client_head(url, &context, &info) != 0 ||
      (strncmp(info.effective_url, "http://", 7) != 0 &&
       strncmp(info.effective_url, "https://", 8) != 0))
    goto clear;

  dm_mutex_lock(&g_mutex);
  d = g_head;
  while (d && d->id != id) d = d->next;
  if (!d) result = IPC_RESULT_NOT_FOUND;
  else if (d->status != status || d->status == DOWNLOAD_ACTIVE ||
           d->total_size != size || d->chunk_count != chunks ||
           atomic_load(&d->bytes_downloaded) != bytes ||
           strcmp(d->url, url) || strcmp(d->etag, etag) ||
           strcmp(d->last_modified, modified) ||
           (d->request && memcmp(d->request, &options, sizeof(options))) ||
           (!d->request && options.browser_context))
    result = IPC_RESULT_REJECTED;
  /* Never replace the validators that protect existing resume chunks. A
   * changed resource must be re-downloaded, not spliced into partial bytes. */
  else if (chunks > 0 && (size != info.total_size ||
           strcmp(etag, info.etag) || strcmp(modified, info.last_modified)))
    result = IPC_RESULT_REJECTED;
  else if (db_refresh_download(id, info.effective_url, info.total_size,
                               info.etag, info.last_modified) == 0) {
    memcpy(d->url, info.effective_url, sizeof(d->url));
    memcpy(d->etag, info.etag, sizeof(d->etag));
    memcpy(d->last_modified, info.last_modified, sizeof(d->last_modified));
    d->total_size = info.total_size;
    result = IPC_RESULT_OK;
  }
  dm_mutex_unlock(&g_mutex);
clear:
  /* The snapshot may contain browser cookies or authentication credentials. */
  for (size_t i = 0; i < sizeof(options); i++) secret[i] = 0;
  return result;
}
