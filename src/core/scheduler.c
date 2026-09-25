// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "scheduler.h"

#include "../core/queue_manager.h"
#include "../engine/engine_runner.h"
#include "../persistence/db.h"
#include "../platform/ipc_socket.h"
#include "../platform/spawn.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../utils/notify.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
static _Atomic time_t g_next_schedule_check;

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
    db_update_status(dl->id, "PAUSED");
    queue_manager_update_status(dl->id, DOWNLOAD_PAUSED);
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

static int parse_schedule_time(const char value[6]) {
  if (strnlen(value, 6) != 5 || value[2] != ':' ||
      value[0] < '0' || value[0] > '9' ||
      value[1] < '0' || value[1] > '9' ||
      value[3] < '0' || value[3] > '9' ||
      value[4] < '0' || value[4] > '9')
    return -1;
  int hour = (value[0] - '0') * 10 + value[1] - '0';
  int minute = (value[3] - '0') * 10 + value[4] - '0';
  return hour < 24 && minute < 60 ? hour * 60 + minute : -1;
}

SchedulerQueueState scheduler_queue_state(const Queue *queue, time_t now) {
  if (!queue)
    return SCHEDULED_IDLE;
  if (!queue->schedule_start[0] && !queue->schedule_stop[0])
    return SCHEDULE_ALWAYS;
  int start = parse_schedule_time(queue->schedule_start);
  int stop = parse_schedule_time(queue->schedule_stop);
  if (start < 0 || stop < 0 || start == stop)
    return SCHEDULED_IDLE;
  struct tm local;
  /* TODO(platform): use localtime_s when building this scheduler on Windows. */
  if (!localtime_r(&now, &local))
    return SCHEDULED_IDLE;
  int minute = local.tm_hour * 60 + local.tm_min;
  bool active = start < stop ? minute >= start && minute < stop
                             : minute >= start || minute < stop;
  return active ? SCHEDULED_ACTIVE : SCHEDULED_IDLE;
}

void scheduler_schedule_tick_at(time_t now) {
  Queue *queues = NULL;
  size_t count = 0;
  if (queue_list(&queues, &count) != 0) {
    LOG_WARN("Could not evaluate queue schedules");
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    SchedulerQueueState state = scheduler_queue_state(&queues[i], now);
    uint32_t resumed_ids[SCHEDULER_MAX_WORKERS];
    int resumed = queue_manager_apply_schedule(queues[i].id,
                         state != SCHEDULED_IDLE, resumed_ids,
                         SCHEDULER_MAX_WORKERS);
    for (int j = 0; j < resumed; ++j)
      ipc_broadcast_status(resumed_ids[j], "QUEUED", 0.0f);
  }
  free(queues);
}

void scheduler_post_actions_tick_at(time_t now) {
  if (now == (time_t)-1)
    return;
  Queue *queues = NULL;
  size_t count = 0;
  if (queue_list(&queues, &count) != 0) {
    LOG_WARN("Could not evaluate queue post-actions");
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    Queue *queue = &queues[i];
    if (!config_post_action_enabled(queue->post_action))
      continue;
    if (strcmp(queue->post_action, "command") == 0 &&
        !queue->post_action_arg[0])
      continue;
    int due = db_queue_post_action_due(queue->id, (int64_t)now);
    if (due < 0)
      LOG_WARN("Could not evaluate post-action for queue %u", queue->id);
    else if (due > 0 &&
             spawn_post_action(queue->post_action,
                               queue->post_action_arg) != 0)
      LOG_WARN("Could not start post-action for queue %u", queue->id);
  }
  free(queues);
}

void scheduler_tick(void) {
  time_t now = time(NULL);
  scheduler_post_actions_tick_at(now);
  time_t next = atomic_load(&g_next_schedule_check);
  if (now != (time_t)-1 && (next == 0 || now >= next || now < next - 60)) {
    scheduler_schedule_tick_at(now);
    atomic_store(&g_next_schedule_check, now + 60);
  }
  reap_finished_workers();

  dm_mutex_t *mutex = (dm_mutex_t *)queue_manager_get_mutex();

  dm_mutex_lock(mutex);

  int active_count = queue_manager_count_by_status_locked(DOWNLOAD_ACTIVE);
  int max_active = config_get_max_concurrent_downloads();
  Download *next_dl = NULL;

  if (active_count < max_active) {
    next_dl = queue_manager_find_next_queued();
    if (next_dl != NULL) {
      next_dl->transfer_metrics =
          (DownloadTransferMetrics){.eta_seconds = UINT64_MAX};
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
  atomic_store(&g_next_schedule_check, 0);
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

DownloadTransferMetrics scheduler_advance_transfer_metrics(
    DownloadTransferMetrics previous, uint64_t bytes_received,
    uint64_t total_bytes, uint64_t now_ms) {
  DownloadTransferMetrics next = previous;
  next.sampled_bytes = bytes_received;
  next.sampled_at_ms = now_ms;
  next.eta_seconds = UINT64_MAX;

  if (previous.sampled_at_ms == 0 || now_ms <= previous.sampled_at_ms ||
      bytes_received < previous.sampled_bytes) {
    next.speed_bps = 0;
    return next;
  }

  uint64_t delta_bytes = bytes_received - previous.sampled_bytes;
  uint64_t elapsed_ms = now_ms - previous.sampled_at_ms;
  long double instant =
      ((long double)delta_bytes * 1000.0L) / (long double)elapsed_ms;
  uint64_t instantaneous =
      instant >= (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)instant;
  if (previous.speed_bps == 0)
    next.speed_bps = instantaneous;
  else
    next.speed_bps = (instantaneous / 10) * 3 +
                     (previous.speed_bps / 10) * 7 +
                     ((instantaneous % 10) * 3 +
                      (previous.speed_bps % 10) * 7) / 10;

  if (total_bytes > 0 && next.speed_bps > 0) {
    uint64_t remaining =
        bytes_received >= total_bytes ? 0 : total_bytes - bytes_received;
    next.eta_seconds = remaining / next.speed_bps +
                       (remaining % next.speed_bps != 0);
  }
  return next;
}

void scheduler_report_progress(void) {
  struct timespec now;
  bool have_time = clock_gettime(CLOCK_MONOTONIC, &now) == 0;
  uint64_t now_ms = have_time
                        ? (uint64_t)now.tv_sec * 1000 +
                              (uint64_t)now.tv_nsec / 1000000
                        : 0;
  DownloadProgressSnapshot snaps[64];
  int n = queue_manager_snapshot_active_progress(snaps, 64);
  for (int i = 0; i < n; i++) {
    if (have_time) {
      DownloadTransferMetrics metrics = scheduler_advance_transfer_metrics(
          snaps[i].transfer_metrics, snaps[i].bytes_downloaded,
          snaps[i].total_size, now_ms);
      queue_manager_set_transfer_metrics(snaps[i].id, metrics);
    }
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
