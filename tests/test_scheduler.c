#include "../src/core/queue_manager.h"
#include "../src/core/scheduler.h"
#include "../src/engine/engine_runner.h"
#include "../src/persistence/db.h"
#include "../src/platform/config.h"
#include <criterion/criterion.h>
#include <stdio.h>  // for sprintf
#include <stdlib.h> // for setenv

// --- Mock engine_run_download (overrides the real one) ---
int engine_run_download(struct Download *d) {
  return 0; // always success
}

static void setup_scheduler(void) {
  setenv("DOWNLOADMGR_ROOT", "/tmp", 1);
  db_init(":memory:");
}

static void teardown_scheduler(void) { db_close(); }

TestSuite(scheduler, .init = setup_scheduler, .fini = teardown_scheduler);

Test(scheduler, tick_starts_download) {
  uint32_t id = queue_manager_add("http://example.com", "/tmp/file");
  cr_assert_neq(id, 0);

  scheduler_tick();

  Download *d = queue_manager_find_by_id(id);
  cr_assert_not_null(d);
  // The stub returns 0, so the download thread finishes quickly.
  // Status becomes DONE.
  cr_assert(d->status == DOWNLOAD_ACTIVE || d->status == DOWNLOAD_DONE);
}

Test(scheduler, respects_max_active) {
  uint32_t ids[5];
  for (int i = 0; i < 5; i++) {
    char url[100];
    sprintf(url, "http://test%d", i);
    ids[i] = queue_manager_add(url, "/tmp/file");
    cr_assert_neq(ids[i], 0);
  }

  // Call tick a few times; only SCHEDULER_MAX_ACTIVE (3) should start.
  for (int i = 0; i < 5; i++) {
    scheduler_tick();
  }

  int started = 0;
  for (int i = 0; i < 5; i++) {
    Download *d = queue_manager_find_by_id(ids[i]);
    if (d->status == DOWNLOAD_ACTIVE || d->status == DOWNLOAD_DONE)
      started++;
  }
  cr_assert_leq(started, config_get_max_concurrent_downloads());
}
