// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "scheduler.h"

#include "../core/queue_manager.h"
#include "../engine/engine_runner.h"
#include "../persistence/db.h"
#include "../platform/ipc_socket.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../utils/notify.h"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#define SCHEDULER_MAX_WORKERS 64

typedef struct {
  dm_thread_t thread;
  Download *download;
  uint32_t download_id;
  _Atomic bool done;
  bool in_use;
} SchedulerWorker;

static SchedulerWorker g_workers[SCHEDULER_MAX_WORKERS];

/* Helpers */

/**
 * Return the filename component of `path` (everything after the last
 * '/' or '\').
 */
static const char *basename_of(const char *path) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *bslash = strrchr(path, '\\');
  if (bslash && (!slash || bslash > slash))
    slash = bslash;
#endif
  return slash ? slash + 1 : path;
}

/**
 * Calculate the backoff delay (seconds) for retry attempt `attempt`
 * (1‑based).  Uses base delay and cap from the configuration.
 */
static int retry_backoff_seconds(int attempt) {
  int base = config_get_retry_base_delay_sec();
  int max_delay = config_get_retry_max_delay_sec();
  int shift = attempt - 1;
  if (shift > 30)
    shift = 30; // prevent overflow on huge attempt counts
  int delay = base << shift;
  return (delay > max_delay || delay < 0) ? max_delay : delay;
}

/**
 * The actual download worker thread.
 *
 * Runs the engine for the given download and handles the result
 * (completion, cancel, pause, retry, permanent error).
 */
static int download_thread_fn(void *arg) {
  Download *dl = (Download *)arg;

  LOG_INFO("Starting download %u (%s)", dl->id, dl->url);
  int rc = engine_run_download(dl);

  bool canceled = atomic_load(&dl->cancel_requested);
  bool paused = atomic_load(&dl->pause_requested);

  if (rc == 0) {
    dl->chunk_count = 0;
    dl->retry_count = 0;
    dl->next_retry_at = 0;
    db_delete_chunks(dl->id);
    queue_manager_update_status(dl->id, DOWNLOAD_DONE);
    db_update_status(dl->id, "DONE");
    ipc_broadcast_status(dl->id, "Done", 1.0f);
    LOG_INFO("Download %u completed", dl->id);
    dm_notify_send("Download Complete", basename_of(dl->dest_path),
                   DM_NOTIFY_INFO);
    return 0;
  }

  if (canceled) {
    LOG_INFO("Download %u canceled", dl->id);
    queue_manager_clear_resume_state(dl->id);
    db_delete_chunks(dl->id);
    db_update_total_size(dl->id, 0);
    if (dl->dest_path[0] != '\0')
      unlink(dl->dest_path);
    db_update_status(dl->id, "CANCELED");
    queue_manager_update_status(dl->id, DOWNLOAD_CANCELED);
    ipc_broadcast_status(dl->id, "Canceled", 0.0f);
    return rc;
  }

  if (paused) {
    LOG_INFO("Download %u paused", dl->id);
    queue_manager_update_status(dl->id, DOWNLOAD_PAUSED);
    db_update_status(dl->id, "PAUSED");
    ipc_broadcast_status(dl->id, "Paused", dl->progress);
    return rc;
  }

  /* Verification failure (checksum / size mismatch) – not retryable. */
  if (rc == -2) {
    dl->retry_count = 0;
    dl->next_retry_at = 0;
    queue_manager_update_status(dl->id, DOWNLOAD_ERROR);
    db_update_status(dl->id, "ERROR");
    ipc_broadcast_status(dl->id, "Verification failed", dl->progress);
    LOG_WARN("Download %u failed verification – not retrying, file removed",
             dl->id);
    dm_notify_send("Download Failed", basename_of(dl->dest_path),
                   DM_NOTIFY_ERROR);
    return rc;
  }

  if (rc == -3 || rc == -4) {
    dl->retry_count = 0;
    dl->next_retry_at = 0;
    queue_manager_update_status(dl->id, DOWNLOAD_ERROR);
    db_update_status(dl->id, "ERROR");
    ipc_broadcast_status(dl->id, "Error", dl->progress);
    LOG_WARN("Download %u failed — %s, not retrying: %s", dl->id,
             rc == -4 ? "resume file is missing" : "destination already exists",
             dl->dest_path);
    dm_notify_send("Download Failed", basename_of(dl->dest_path),
                   DM_NOTIFY_ERROR);
    return rc;
  }

  /* Retryable failure – backoff and re‑queue if attempts remain. */
  if (dl->retry_count < config_get_retry_max_attempts()) {
    dl->retry_count++;
    int delay = retry_backoff_seconds(dl->retry_count);
    dl->next_retry_at = time(NULL) + delay;
    queue_manager_update_status(dl->id, DOWNLOAD_QUEUED);
    db_update_status(dl->id, "QUEUED");
    ipc_broadcast_status(dl->id, "Retrying", dl->progress);
    LOG_WARN("Download %u failed, retrying in %ds (attempt %d/%d)", dl->id,
             delay, dl->retry_count, config_get_retry_max_attempts());
    return rc;
  }

  /* All retries exhausted – mark as resumable error. */
  dl->retry_count = 0;
  dl->next_retry_at = 0;
  queue_manager_update_status(dl->id, DOWNLOAD_ERROR);
  db_update_status(dl->id, "ERROR");
  ipc_broadcast_status(dl->id, "Error", dl->progress);
  LOG_WARN("Download %u failed (resumable)", dl->id);
  dm_notify_send("Download Failed", basename_of(dl->dest_path),
                 DM_NOTIFY_ERROR);
  return rc;
}

static int scheduler_worker_fn(void *arg) {
  SchedulerWorker *worker = (SchedulerWorker *)arg;
  int result = download_thread_fn(worker->download);
  atomic_store(&worker->done, true);
  return result;
}

static void reap_finished_workers(void) {
  for (int i = 0; i < SCHEDULER_MAX_WORKERS; i++) {
    SchedulerWorker *worker = &g_workers[i];
    if (!worker->in_use || !atomic_load(&worker->done))
      continue;
    dm_thread_join(&worker->thread, NULL);
    worker->download = NULL;
    worker->download_id = 0;
    worker->in_use = false;
  }
}

/* Public API */

void scheduler_tick(void) {
  reap_finished_workers();

  dm_mutex_t *mutex = (dm_mutex_t *)queue_manager_get_mutex();

  dm_mutex_lock(mutex);

  int active_count = queue_manager_count_by_status_locked(DOWNLOAD_ACTIVE);
  int max_active = config_get_max_concurrent_downloads();
  Download *next_dl = NULL;

  if (active_count < max_active) {
    next_dl = queue_manager_find_next_queued();
    if (next_dl != NULL) {
      next_dl->status = DOWNLOAD_ACTIVE;
      next_dl->next_retry_at = 0; // not needed once running
    }
  }

  dm_mutex_unlock(mutex);

  if (next_dl != NULL) {
    SchedulerWorker *worker = NULL;
    for (int i = 0; i < SCHEDULER_MAX_WORKERS; i++) {
      if (!g_workers[i].in_use) {
        worker = &g_workers[i];
        break;
      }
    }

    if (worker != NULL) {
      worker->download = next_dl;
      worker->download_id = next_dl->id;
      atomic_store(&worker->done, false);
      worker->in_use = true;
    }

    if (worker != NULL &&
        dm_thread_create(&worker->thread, scheduler_worker_fn, worker) == 0) {
      return;
    }

    if (worker != NULL) {
      worker->download = NULL;
      worker->download_id = 0;
      worker->in_use = false;
    }

    {
      /* rollback on thread creation failure */
      dm_mutex_lock(mutex);
      next_dl->status = DOWNLOAD_QUEUED;
      dm_mutex_unlock(mutex);
    }
  }
}

void scheduler_shutdown(void) {
  for (int i = 0; i < SCHEDULER_MAX_WORKERS; i++) {
    SchedulerWorker *worker = &g_workers[i];
    if (worker->in_use)
      queue_manager_pause(worker->download_id);
  }

  for (int i = 0; i < SCHEDULER_MAX_WORKERS; i++) {
    SchedulerWorker *worker = &g_workers[i];
    if (!worker->in_use)
      continue;
    dm_thread_join(&worker->thread, NULL);
    worker->download = NULL;
    worker->download_id = 0;
    worker->in_use = false;
  }
}

void scheduler_report_progress(void) {
  DownloadProgressSnapshot snaps[64];
  int n = queue_manager_snapshot_active_progress(snaps, 64);
  for (int i = 0; i < n; i++) {
    float progress = 0.0f;
    if (snaps[i].total_size > 0) {
      progress = (float)((double)snaps[i].bytes_downloaded /
                         (double)snaps[i].total_size);
    }
    ipc_broadcast_status(snaps[i].id, "Downloading", progress);
  }

  /* Persist per‑chunk progress so a crash loses at most one tick. */
  ChunkProgressSnapshot chunk_snaps[64];
  int cn = queue_manager_snapshot_chunk_progress(chunk_snaps, 64);
  for (int i = 0; i < cn; i++) {
    db_update_chunk_progress(chunk_snaps[i].download_id,
                             chunk_snaps[i].range_start,
                             chunk_snaps[i].bytes_done);
  }
}
