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
static _Atomic int post_action_runs;

int spawn_post_action(const char *action, const char *argument) {
  cr_assert_str_eq(action, "command");
  cr_assert_str_eq(argument, "/bin/true");
  atomic_fetch_add(&post_action_runs, 1);
  return 0;
}

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
  atomic_store(&post_action_runs, 0);
  db_init(":memory:");
}

static void teardown_scheduler(void) {
  scheduler_shutdown();
  db_close();
}

TestSuite(scheduler, .init = setup_scheduler, .fini = teardown_scheduler);

Test(scheduler, command_post_action_requires_all_done_and_five_seconds) {
  char config_path[] = "/tmp/cdm-actions-XXXXXX";
  int fd = mkstemp(config_path);
  cr_assert_geq(fd, 0);
  FILE *config_file = fdopen(fd, "w");
  cr_assert_not_null(config_file);
  cr_assert_gt(fputs("[post_actions]\nallow_command = true\n", config_file),
               0);
  cr_assert_eq(fclose(config_file), 0);
  config_init(config_path);
  unlink(config_path);

  Queue queue = {0};
  strcpy(queue.name, "Actions");
  strcpy(queue.post_action, "command");
  strcpy(queue.post_action_arg, "/bin/true");
  uint32_t queue_id = 0;
  cr_assert_eq(queue_create(&queue, &queue_id), 0);
  RequestOptions options = {.queue_id = queue_id};
  uint32_t id = queue_manager_add("http://127.0.0.1/action",
                                  "/tmp/action-item", &options);
  cr_assert_neq(id, 0);
  cr_assert_eq(db_insert_download(id, "http://127.0.0.1/action",
                                  "/tmp/action-item", &options), 0);
  scheduler_post_actions_tick_at(100);
  cr_assert_eq(atomic_load(&post_action_runs), 0);
  cr_assert_eq(db_update_status(id, "DONE"), 0);
  scheduler_post_actions_tick_at(101);
  scheduler_post_actions_tick_at(105);
  cr_assert_eq(atomic_load(&post_action_runs), 0);
  scheduler_post_actions_tick_at(106);
  scheduler_post_actions_tick_at(200);
  cr_assert_eq(atomic_load(&post_action_runs), 1);
  queue_manager_remove(id);
}

static time_t fixed_utc(int day, int hour, int minute) {
  setenv("TZ", "UTC", 1);
  tzset();
  struct tm value = {.tm_year = 126, .tm_mon = 0, .tm_mday = day,
                     .tm_hour = hour, .tm_min = minute};
  return mktime(&value);
}

Test(scheduler, schedule_states_include_boundaries_and_overnight) {
  Queue queue = {0};
  cr_assert_eq(scheduler_queue_state(&queue, fixed_utc(1, 12, 0)),
               SCHEDULE_ALWAYS);
  strcpy(queue.schedule_start, "09:00");
  strcpy(queue.schedule_stop, "17:00");
  cr_assert_eq(scheduler_queue_state(&queue, fixed_utc(1, 8, 59)),
               SCHEDULED_IDLE);
  cr_assert_eq(scheduler_queue_state(&queue, fixed_utc(1, 9, 0)),
               SCHEDULED_ACTIVE);
  cr_assert_eq(scheduler_queue_state(&queue, fixed_utc(1, 16, 59)),
               SCHEDULED_ACTIVE);
  cr_assert_eq(scheduler_queue_state(&queue, fixed_utc(1, 17, 0)),
               SCHEDULED_IDLE);
  strcpy(queue.schedule_start, "22:00");
  strcpy(queue.schedule_stop, "06:00");
  cr_assert_eq(scheduler_queue_state(&queue, fixed_utc(1, 23, 0)),
               SCHEDULED_ACTIVE);
  cr_assert_eq(scheduler_queue_state(&queue, fixed_utc(2, 5, 59)),
               SCHEDULED_ACTIVE);
  cr_assert_eq(scheduler_queue_state(&queue, fixed_utc(2, 6, 0)),
               SCHEDULED_IDLE);
}

Test(scheduler, schedule_pauses_active_and_resumes_only_its_own_download) {
  Queue queue = {.max_concurrent = 2};
  strcpy(queue.name, "Workday");
  strcpy(queue.schedule_start, "09:00");
  strcpy(queue.schedule_stop, "17:00");
  uint32_t queue_id = 0;
  cr_assert_eq(queue_create(&queue, &queue_id), 0);
  RequestOptions options = {.queue_id = queue_id};
  uint32_t active_id = queue_manager_add("http://127.0.0.1/scheduled",
                                         "/tmp/scheduled-item", &options);
  uint32_t manual_id = queue_manager_add("http://127.0.0.1/manual",
                                         "/tmp/manual-item", &options);
  cr_assert_neq(active_id, 0);
  cr_assert_neq(manual_id, 0);
  cr_assert_eq(db_insert_download(active_id, "http://127.0.0.1/scheduled",
                                  "/tmp/scheduled-item", &options), 0);
  cr_assert_eq(db_insert_download(manual_id, "http://127.0.0.1/manual",
                                  "/tmp/manual-item", &options), 0);
  queue_manager_update_status(active_id, DOWNLOAD_ACTIVE);
  cr_assert_eq(db_update_status(active_id, "ACTIVE"), 0);
  queue_manager_pause(manual_id);
  scheduler_schedule_tick_at(fixed_utc(1, 17, 0));
  Download *active = queue_manager_find_by_id(active_id);
  cr_assert_not_null(active);
  cr_assert(atomic_load(&active->pause_requested));
  cr_assert(active->schedule_paused);
  queue_manager_update_status(active_id, DOWNLOAD_PAUSED);
  cr_assert_eq(db_update_status(active_id, "PAUSED"), 0);
  scheduler_schedule_tick_at(fixed_utc(2, 9, 0));
  DownloadStatus status = DOWNLOAD_ERROR;
  cr_assert(queue_manager_get_status(active_id, &status));
  cr_assert_eq(status, DOWNLOAD_QUEUED);
  cr_assert(queue_manager_get_status(manual_id, &status));
  cr_assert_eq(status, DOWNLOAD_PAUSED);
  cr_assert_not(active->schedule_paused);
  queue_manager_remove(active_id);
  queue_manager_remove(manual_id);
}

Test(scheduler, scheduled_pause_survives_restart_and_resumes_at_start) {
  char path[] = "/tmp/cdm-schedule-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  close(fd);
  db_close();
  cr_assert_eq(db_init(path), 0);
  Queue queue = {0};
  strcpy(queue.name, "Restart schedule");
  strcpy(queue.schedule_start, "09:00");
  strcpy(queue.schedule_stop, "17:00");
  uint32_t queue_id = 0;
  cr_assert_eq(queue_create(&queue, &queue_id), 0);
  RequestOptions options = {.queue_id = queue_id};
  uint32_t id = queue_manager_add("http://127.0.0.1/restart",
                                   "/tmp/schedule-restart", &options);
  cr_assert_neq(id, 0);
  cr_assert_eq(db_insert_download(id, "http://127.0.0.1/restart",
                                  "/tmp/schedule-restart", &options), 0);
  queue_manager_update_status(id, DOWNLOAD_ACTIVE);
  cr_assert_eq(db_update_status(id, "ACTIVE"), 0);
  scheduler_schedule_tick_at(fixed_utc(1, 17, 0));
  queue_manager_update_status(id, DOWNLOAD_PAUSED);
  cr_assert_eq(db_update_status(id, "PAUSED"), 0);
  queue_manager_remove(id);
  db_close();
  cr_assert_eq(db_init(path), 0);
  cr_assert_eq(db_restore_queue(), 0);
  Download *restored = queue_manager_find_by_id(id);
  cr_assert_not_null(restored);
  cr_assert(restored->schedule_paused);
  cr_assert_eq(restored->status, DOWNLOAD_PAUSED);
  scheduler_schedule_tick_at(fixed_utc(2, 9, 0));
  cr_assert_eq(restored->status, DOWNLOAD_QUEUED);
  cr_assert_not(restored->schedule_paused);
  queue_manager_remove(id);
  db_close();
  unlink(path);
}

Test(scheduler, schedule_boundary_pauses_running_worker) {
  Queue queue = {0};
  strcpy(queue.name, "Worker schedule");
  uint32_t queue_id = 0;
  cr_assert_eq(queue_create(&queue, &queue_id), 0);
  RequestOptions options = {.queue_id = queue_id};
  uint32_t id = queue_manager_add("http://127.0.0.1/worker",
                                   "/tmp/schedule-worker", &options);
  cr_assert_neq(id, 0);
  cr_assert_eq(db_insert_download(id, "http://127.0.0.1/worker",
                                  "/tmp/schedule-worker", &options), 0);
  atomic_store(&hold_workers, true);
  scheduler_tick();
  for (int i = 0; i < 1000 && atomic_load(&active_workers) == 0; ++i)
    dm_thread_sleep_ms(1);
  cr_assert_eq(atomic_load(&active_workers), 1);
  queue.id = queue_id;
  strcpy(queue.schedule_start, "09:00");
  strcpy(queue.schedule_stop, "17:00");
  cr_assert_eq(queue_update(&queue), 0);
  scheduler_schedule_tick_at(fixed_utc(1, 17, 0));
  DownloadStatus status = DOWNLOAD_ACTIVE;
  for (int i = 0; i < 1000; ++i) {
    if (queue_manager_get_status(id, &status) && status == DOWNLOAD_PAUSED)
      break;
    dm_thread_sleep_ms(1);
  }
  cr_assert_eq(status, DOWNLOAD_PAUSED);
  scheduler_schedule_tick_at(fixed_utc(2, 9, 0));
  cr_assert(queue_manager_get_status(id, &status));
  cr_assert_eq(status, DOWNLOAD_QUEUED);
  scheduler_shutdown();
  queue_manager_remove(id);
  atomic_store(&hold_workers, false);
}

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
