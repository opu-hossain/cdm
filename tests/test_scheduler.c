#include "../src/core/queue_manager.h"
#include "../src/core/scheduler.h"
#include "../src/engine/engine_runner.h"
#include "../src/persistence/db.h"
#include "../src/platform/thread.h"
#include "../src/utils/config.h"
#include <criterion/criterion.h>
#include <stdio.h>  // for sprintf
#include <stdlib.h> // for setenv
#include <stdatomic.h>
#include <unistd.h>

/* Mock engine_run_download (overrides the real one) */
static _Atomic int active_workers;
static _Atomic bool hold_workers;
static _Atomic bool fail_workers;
static _Atomic int engine_runs;

int engine_run_download(struct Download *d) {
  atomic_fetch_add(&engine_runs, 1);
  atomic_fetch_add(&active_workers, 1);
  while (atomic_load(&hold_workers) &&
         !atomic_load(&d->pause_requested) &&
         !atomic_load(&d->cancel_requested))
    dm_thread_sleep_ms(1);
  atomic_fetch_sub(&active_workers, 1);
  if (atomic_load(&d->pause_requested) ||
      atomic_load(&d->cancel_requested) || atomic_load(&fail_workers))
    return -1;
  return 0;
}

static void setup_scheduler(void) {
  setenv("DOWNLOADMGR_ROOT", "/tmp", 1);
  atomic_store(&active_workers, 0);
  atomic_store(&hold_workers, false);
  atomic_store(&fail_workers, false);
  atomic_store(&engine_runs, 0);
  db_init(":memory:");
}

static void teardown_scheduler(void) { db_close(); }

TestSuite(scheduler, .init = setup_scheduler, .fini = teardown_scheduler);

Test(scheduler, smooths_transfer_speed_and_estimates_eta) {
  DownloadTransferMetrics sample = {.eta_seconds = UINT64_MAX};
  sample = scheduler_advance_transfer_metrics(sample, 0, 10000, 1000);
  cr_assert_eq(sample.speed_bps, 0);
  cr_assert_eq(sample.eta_seconds, UINT64_MAX);

  sample = scheduler_advance_transfer_metrics(sample, 1000, 10000, 2000);
  cr_assert_eq(sample.speed_bps, 1000);
  cr_assert_eq(sample.eta_seconds, 9);

  sample = scheduler_advance_transfer_metrics(sample, 3000, 10000, 3000);
  cr_assert_eq(sample.speed_bps, 1300); // 0.3 * 2000 + 0.7 * 1000
  cr_assert_eq(sample.eta_seconds, 6);

  sample = scheduler_advance_transfer_metrics(sample, 3000, 0, 4000);
  cr_assert_eq(sample.speed_bps, 910);
  cr_assert_eq(sample.eta_seconds, UINT64_MAX);

  sample = scheduler_advance_transfer_metrics(sample, 100, 10000, 5000);
  cr_assert_eq(sample.speed_bps, 0); // a restarted transfer resets samples
  cr_assert_eq(sample.eta_seconds, UINT64_MAX);
}

Test(scheduler, automatic_retries_stop_at_terminal_error) {
  uint32_t id = queue_manager_add("http://example.com/fails",
                                  "/tmp/scheduler-retry-limit", NULL);
  cr_assert_neq(id, 0);
  atomic_store(&fail_workers, true);
  int max_retries = config_get_retry_max_attempts();

  for (int attempt = 0; attempt <= max_retries; attempt++) {
    scheduler_tick();
    DownloadStatus status = DOWNLOAD_ACTIVE;
    for (int wait = 0; wait < 1000; wait++) {
      if (atomic_load(&engine_runs) > attempt &&
          queue_manager_get_status(id, &status) &&
          status != DOWNLOAD_ACTIVE)
        break;
      dm_thread_sleep_ms(1);
    }
    cr_assert_eq(atomic_load(&engine_runs), attempt + 1);
    Download *d = queue_manager_find_by_id(id);
    cr_assert_not_null(d);
    if (attempt < max_retries) {
      cr_assert_eq(status, DOWNLOAD_QUEUED);
      cr_assert_eq(d->retry_count, attempt + 1);
      d->next_retry_at = time(NULL) - 1; /* Skip wall-clock backoff in test. */
      dm_thread_sleep_ms(2);
    } else {
      cr_assert_eq(status, DOWNLOAD_ERROR);
    }
  }

  for (int tick = 0; tick < 10; tick++)
    scheduler_tick();
  cr_assert_eq(atomic_load(&engine_runs), max_retries + 1);
  DownloadStatus final_status = DOWNLOAD_QUEUED;
  cr_assert(queue_manager_get_status(id, &final_status));
  cr_assert_eq(final_status, DOWNLOAD_ERROR);
  queue_manager_remove(id);
}

Test(scheduler, tick_starts_download) {
  uint32_t id = queue_manager_add("http://example.com", "/tmp/file", NULL);
  cr_assert_neq(id, 0);

  atomic_store(&hold_workers, false);
  scheduler_tick();

  Download *d = queue_manager_find_by_id(id);
  cr_assert_not_null(d);
  // The stub returns 0, so the download thread finishes quickly.
  // Status becomes DONE.
  cr_assert(d->status == DOWNLOAD_ACTIVE || d->status == DOWNLOAD_DONE);
}

Test(scheduler, respects_max_active) {
  atomic_store(&hold_workers, true);
  uint32_t ids[5];
  for (int i = 0; i < 5; i++) {
    char url[100];
    char dest[128];
    sprintf(url, "http://test%d", i);
    snprintf(dest, sizeof(dest), "/tmp/scheduler-file-%d", i);
    ids[i] = queue_manager_add(url, dest, NULL);
    cr_assert_neq(ids[i], 0);
  }

  // Call tick a few times; only SCHEDULER_MAX_ACTIVE (3) should start.
  for (int i = 0; i < 5; i++) {
    scheduler_tick();
  }

  for (int i = 0; i < 1000 && atomic_load(&active_workers) <
                                  config_get_max_concurrent_downloads();
       i++)
    dm_thread_sleep_ms(1);

  int started = 0;
  for (int i = 0; i < 5; i++) {
    Download *d = queue_manager_find_by_id(ids[i]);
    if (d->status == DOWNLOAD_ACTIVE || d->status == DOWNLOAD_DONE)
      started++;
  }
  cr_assert_leq(started, config_get_max_concurrent_downloads());

  atomic_store(&hold_workers, false);
  for (int i = 0; i < 1000 && atomic_load(&active_workers) != 0; i++)
    dm_thread_sleep_ms(1);
  cr_assert_eq(atomic_load(&active_workers), 0);
}

Test(scheduler, shutdown_preserves_active_download_for_resume) {
  atomic_store(&hold_workers, true);
  uint32_t id =
      queue_manager_add("http://example.com/shutdown", "/tmp/shutdown", NULL);
  cr_assert_neq(id, 0);

  scheduler_tick();
  for (int i = 0; i < 1000 && atomic_load(&active_workers) == 0; i++)
    dm_thread_sleep_ms(1);
  cr_assert_eq(atomic_load(&active_workers), 1);

  scheduler_shutdown();

  DownloadStatus status = DOWNLOAD_ERROR;
  cr_assert(queue_manager_get_status(id, &status));
  cr_assert_eq(status, DOWNLOAD_PAUSED);
  cr_assert_eq(atomic_load(&active_workers), 0);

  queue_manager_remove(id);
  atomic_store(&hold_workers, false);
}

Test(scheduler, restore_requeues_active_downloads_with_chunk_resume_state) {
  const char *path = "/tmp/cdm_scheduler_restart.db";
  unlink(path);

  db_close();
  cr_assert_eq(db_init(path), 0);

  uint32_t id = 42;
  RequestOptions opts = {0};
  cr_assert_eq(db_insert_download(id, "http://example.com/resume",
                                 "/tmp/resume.bin", &opts),
               0);
  cr_assert_eq(db_update_status(id, "ACTIVE"), 0);
  cr_assert_eq(db_insert_chunk(id, 0, 63), 0);
  cr_assert_eq(db_update_chunk_progress(id, 0, 31), 0);

  db_close();
  cr_assert_eq(db_init(path), 0);
  cr_assert_eq(db_restore_queue(), 0);
  queue_manager_seed_next_id(db_get_max_id() + 1);

  Download *d = queue_manager_find_next_queued();
  cr_assert_not_null(d);
  cr_assert_eq(d->id, id);
  cr_assert_eq(d->status, DOWNLOAD_QUEUED);
  cr_assert_eq(d->chunk_count, 1);
  cr_assert_eq(d->chunks[0].bytes_done, 31);

  queue_manager_remove(id);
  db_close();
  unlink(path);
}
