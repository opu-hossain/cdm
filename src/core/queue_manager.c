// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "queue_manager.h"
#include "../platform/thread.h"

#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Internal state                                                    */
/* ------------------------------------------------------------------ */
static Download *g_head = NULL;
static uint32_t g_next_id = 1;
static dm_mutex_t g_mutex;
static bool g_mutex_ready = false;

/* ------------------------------------------------------------------ */
/*  Static helpers                                                    */
/* ------------------------------------------------------------------ */

/**
 * Return the filesystem root under which all downloads must reside.
 *
 * Defaults to $HOME; override with DOWNLOADMGR_ROOT.
 */
static const char *get_allowed_root(void) {
  static char root[1024] = {0};
  if (root[0] == '\0') {
    const char *configured = getenv("DOWNLOADMGR_ROOT");
    const char *home = getenv("HOME");
    const char *base = configured ? configured : (home ? home : "/tmp");
    strncpy(root, base, sizeof(root) - 1);
  }
  return root;
}

/**
 * Validate that the directory part of `path` resolves (after symlinks and
 * ".." collapsing) to somewhere inside get_allowed_root(), and that the
 * filename component contains no path separators.
 *
 * @return true if the destination is safe to write to
 */
static bool is_safe_dest_path(const char *path) {
  if (!path || path[0] == '\0' || strlen(path) >= 900)
    return false;

  char dir_buf[1024];
  strncpy(dir_buf, path, sizeof(dir_buf) - 1);
  dir_buf[sizeof(dir_buf) - 1] = '\0';
  char *dir = dirname(dir_buf);

  char resolved_dir[PATH_MAX];
  if (realpath(dir, resolved_dir) == NULL)
    return false;

  const char *root = get_allowed_root();
  size_t root_len = strlen(root);
  if (strncmp(resolved_dir, root, root_len) != 0)
    return false;
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
static void ensure_mutex(void) {
  if (!g_mutex_ready) {
    dm_mutex_init(&g_mutex);
    g_mutex_ready = true;
  }
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

uint32_t queue_manager_add(const char *url, const char *dest_path,
                           const RequestOptions *opts) {
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

  if (opts)
    d->request = *opts;

  d->status = DOWNLOAD_QUEUED;
  d->priority = 0;

  dm_mutex_lock(&g_mutex);
  d->id = g_next_id++;
  d->next = g_head;
  g_head = d;
  dm_mutex_unlock(&g_mutex);

  return d->id;
}

void queue_manager_add_existing(Download *d) {
  ensure_mutex();

  dm_mutex_lock(&g_mutex);
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
      *prev_ptr = cur->next;
      if (cur->dest_path[0] != '\0')
        unlink(cur->dest_path);
      free(cur);
      break;
    }
    prev_ptr = &cur->next;
  }
  dm_mutex_unlock(&g_mutex);
}

/* ------------------------------------------------------------------ */
/*  Query                                                             */
/* ------------------------------------------------------------------ */

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

  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->status != DOWNLOAD_QUEUED)
      continue;
    if (cur->next_retry_at != 0 && cur->next_retry_at > now)
      continue; // backoff period still active

    if (best == NULL || cur->priority > best->priority)
      best = cur;
  }
  return best;
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
      n++;
    }
  }
  dm_mutex_unlock(&g_mutex);
  return n;
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

/* ------------------------------------------------------------------ */
/*  Status / control                                                  */
/* ------------------------------------------------------------------ */

void queue_manager_update_status(uint32_t id, DownloadStatus new_status) {
  ensure_mutex();

  dm_mutex_lock(&g_mutex);
  for (Download *cur = g_head; cur != NULL; cur = cur->next) {
    if (cur->id == id) {
      cur->status = new_status;
      break;
    }
  }
  dm_mutex_unlock(&g_mutex);
}

bool queue_manager_cancel(uint32_t id) {
  ensure_mutex();
  bool was_active = false;

  dm_mutex_lock(&g_mutex);
  Download **prev_ptr = &g_head;
  while (*prev_ptr != NULL) {
    Download *cur = *prev_ptr;
    if (cur->id == id) {
      if (cur->status == DOWNLOAD_ACTIVE) {
        atomic_store(&cur->cancel_requested, true);
        was_active = true;
      } else {
        *prev_ptr = cur->next;
        if (cur->dest_path[0] != '\0')
          unlink(cur->dest_path);
        free(cur);
      }
      break;
    }
    prev_ptr = &cur->next;
  }
  dm_mutex_unlock(&g_mutex);
  return was_active;
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
      if (cur->status == DOWNLOAD_PAUSED || cur->status == DOWNLOAD_ERROR) {
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

/* ------------------------------------------------------------------ */
/*  Internal (use with care)                                          */
/* ------------------------------------------------------------------ */

void *queue_manager_get_mutex(void) {
  ensure_mutex();
  return &g_mutex;
}
