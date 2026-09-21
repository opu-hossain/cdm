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

// --- Mock engine_run_download (overrides the real one) ---
static _Atomic int active_workers;
static _Atomic bool hold_workers;

int engine_run_download(struct Download *d) {
  (void)d;
  atomic_fetch_add(&active_workers, 1);
  while (atomic_load(&hold_workers) &&
         !atomic_load(&d->pause_requested))
    dm_thread_sleep_ms(1);
  atomic_fetch_sub(&active_workers, 1);
  if (atomic_load(&d->pause_requested))
    return -1;
  return 0;
}

static void setup_scheduler(void) {
  setenv("DOWNLOADMGR_ROOT", "/tmp", 1);
  atomic_store(&active_workers, 0);
  atomic_store(&hold_workers, false);
  db_init(":memory:");
}

static void teardown_scheduler(void) { db_close(); }

TestSuite(scheduler, .init = setup_scheduler, .fini = teardown_scheduler);

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
    sprintf(url, "http://test%d", i);
    ids[i] = queue_manager_add(url, "/tmp/file", NULL);
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
