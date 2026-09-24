#include "../src/core/queue_manager.h"
#include "../src/engine/engine_runner.h"
#include "../src/engine/worker_pool.h"
#include "../src/persistence/db.h"
#include "../src/platform/curl_client.h"
#include "../src/platform/file_io.h"
#include <criterion/criterion.h>
#include <stdatomic.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fallback_scenario;
static int fallback_call;
static bool filename_scenario;
static bool validator_scenario;
static const uint64_t fallback_size = 4ULL * 1024ULL * 1024ULL;

static void fill_file(const char *path, char value, uint64_t length) {
  int fd = open(path, O_WRONLY);
  cr_assert_geq(fd, 0);
  char block[4096];
  memset(block, value, sizeof(block));
  for (uint64_t offset = 0; offset < length; offset += sizeof(block))
    cr_assert_eq(pwrite(fd, block, sizeof(block), (off_t)offset),
                 (ssize_t)sizeof(block));
  close(fd);
}

/* Mocks for external dependencies */
int curl_client_head(const char *url, const RequestContext *ctx, FileInfo *out) {
  (void)url;
  (void)ctx;
  memset(out, 0, sizeof(*out));
  out->total_size = fallback_scenario ? fallback_size : 1000;
  out->supports_ranges = true;
  if (filename_scenario)
    strcpy(out->content_disposition,
           "attachment; filename=\"server-name.bin\"");
  if (validator_scenario) {
    strcpy(out->etag, "\"version-1\"");
    strcpy(out->last_modified, "Wed, 21 Oct 2015 07:28:00 GMT");
  }
  return 0;
}

WorkerPoolResult worker_pool_run(const char *url, const Range *ranges,
                                 int n_workers, const char *dest_path,
                                 _Atomic uint64_t *total_bytes_downloaded,
                                 _Atomic bool *cancel_flag,
                                 _Atomic bool *pause_flag,
                                 _Atomic uint64_t **chunk_progress_slots,
                                 uint64_t total_speed_limit_bps,
                                 const RequestContext *ctx_in,
                                 RebalancePool *rebalance) {
  (void)url;
  (void)ranges;
  (void)total_bytes_downloaded;
  (void)cancel_flag;
  (void)pause_flag;
  (void)chunk_progress_slots;
  (void)total_speed_limit_bps;
  (void)ctx_in;
  (void)rebalance;
  if (fallback_scenario) {
    fallback_call++;
    WorkerPoolResult attempt = {.all_succeeded = false};
    if (fallback_call == 1) {
      cr_assert_gt(n_workers, 1);
      atomic_store(chunk_progress_slots[0], 1024);
      atomic_store(total_bytes_downloaded, 1024);
      return attempt;
    }
    if (fallback_call == 2) {
      cr_assert_eq(n_workers, 1);
      cr_assert(ranges[0].whole_file);
      cr_assert_eq(ranges[0].end, fallback_size - 1);
      fill_file(dest_path, 'B', 4096);
      atomic_store(total_bytes_downloaded, 4096);
      return attempt;
    }
    cr_assert_eq(fallback_call, 3);
    cr_assert_gt(n_workers, 1);
    fill_file(dest_path, 'Z', fallback_size);
    atomic_store(total_bytes_downloaded, fallback_size);
    attempt.all_succeeded = true;
    attempt.total_bytes_downloaded = fallback_size;
    return attempt;
  }
  WorkerPoolResult res = {.all_succeeded = true,
                          .total_bytes_downloaded = 1000};
  for (int i = 0; i < n_workers; i++) {
    res.chunk_succeeded[i] = true;
  }
  if (!filename_scenario)
    file_preallocate(dest_path, 1000);
  return res;
}

RebalancePool *rebalance_pool_create(const Range *ranges, int n_ranges,
                                     _Atomic uint64_t **progress_slots,
                                     uint64_t min_steal_bytes,
                                     RebalanceSplitFn on_split,
                                     RebalanceMutationLockFn lock_mutation,
                                     RebalanceMutationLockFn unlock_mutation,
                                     void *userdata) {
  (void)ranges;
  (void)n_ranges;
  (void)progress_slots;
  (void)min_steal_bytes;
  (void)on_split;
  (void)lock_mutation;
  (void)unlock_mutation;
  (void)userdata;
  return NULL;
}

void rebalance_pool_destroy(RebalancePool *pool) { (void)pool; }

/* Setup / teardown */
static void setup_engine_test(void) {
  fallback_scenario = 0;
  fallback_call = 0;
  filename_scenario = false;
  validator_scenario = false;
  setenv("DOWNLOADMGR_ROOT", "/tmp", 1);
  db_init(":memory:"); // use in‑memory DB to avoid "out of memory" errors
}

static void teardown_engine_test(void) {
  db_close();
}

TestSuite(engine_runner, .init = setup_engine_test,
          .fini = teardown_engine_test);

Test(engine_runner, run_success) {
  Download d = {0};
  strcpy(d.url, "http://127.0.0.1/file");
  snprintf(d.dest_path, sizeof(d.dest_path), "/tmp/cdm-engine-success-%ld.bin",
           (long)getpid());
  unlink(d.dest_path);

  int rc = engine_run_download(&d);
  cr_assert_eq(rc, 0, "engine_run_download should return 0 on success");

  uint64_t size = file_get_size(d.dest_path);
  cr_assert_eq(size, 1000, "Output file should be 1000 bytes");
  unlink(d.dest_path);
}

Test(engine_runner, renames_automatic_destination_after_probe) {
  char dir[128];
  snprintf(dir, sizeof(dir), "/tmp/cdm-auto-name-%ld", (long)getpid());
  cr_assert_eq(mkdir(dir, 0700), 0);
  Download d = {.id = 77, .auto_filename = true, .reserved_file = true};
  strcpy(d.url, "http://127.0.0.1/download.php?id=5");
  snprintf(d.dest_path, sizeof(d.dest_path), "%s/download.php", dir);
  cr_assert_eq(file_preallocate(d.dest_path, 0), 0);
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, NULL), 0);
  filename_scenario = true;

  cr_assert_eq(engine_run_download(&d), 0);
  char expected[256];
  snprintf(expected, sizeof(expected), "%s/server-name.bin", dir);
  cr_assert_str_eq(d.dest_path, expected);
  cr_assert(!atomic_load(&d.auto_filename));
  cr_assert_eq(file_get_size(expected), 1000);
  char old[256];
  snprintf(old, sizeof(old), "%s/download.php", dir);
  cr_assert_neq(access(old, F_OK), 0);
  DbDownloadRow rows[1] = {0};
  cr_assert_eq(db_list_all_downloads(rows, 1), 1);
  cr_assert_str_eq(rows[0].dest_path, expected);
  unlink(expected);
  rmdir(dir);
}

Test(engine_runner, keeps_explicit_destination_after_probe) {
  char dir[128];
  snprintf(dir, sizeof(dir), "/tmp/cdm-explicit-name-%ld", (long)getpid());
  cr_assert_eq(mkdir(dir, 0700), 0);
  Download d = {.id = 78, .reserved_file = true};
  strcpy(d.url, "http://127.0.0.1/download.php?id=5");
  snprintf(d.dest_path, sizeof(d.dest_path), "%s/chosen.bin", dir);
  cr_assert_eq(file_preallocate(d.dest_path, 0), 0);
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, NULL), 0);
  filename_scenario = true;

  cr_assert_eq(engine_run_download(&d), 0);
  char chosen[256];
  snprintf(chosen, sizeof(chosen), "%s/chosen.bin", dir);
  cr_assert_str_eq(d.dest_path, chosen);
  cr_assert_eq(file_get_size(chosen), 1000);
  unlink(chosen);
  rmdir(dir);
}

Test(engine_runner, stores_probe_validators) {
  Download d = {.id = 79};
  strcpy(d.url, "http://127.0.0.1/file.bin");
  snprintf(d.dest_path, sizeof(d.dest_path), "/tmp/cdm-validator-%ld.bin",
           (long)getpid());
  unlink(d.dest_path);
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, NULL), 0);
  validator_scenario = true;

  cr_assert_eq(engine_run_download(&d), 0);
  cr_assert_str_eq(d.etag, "\"version-1\"");
  cr_assert_str_eq(d.last_modified, "Wed, 21 Oct 2015 07:28:00 GMT");
  unlink(d.dest_path);
}

Test(engine_runner, fills_claimed_file) {
  char path[128];
  snprintf(path, sizeof(path), "/tmp/test_engine_claimed_%ld", (long)getpid());
  unlink(path);
  cr_assert_eq(file_preallocate(path, 0), 0);
  cr_assert_eq(file_preallocate_reserved(path, 1000), 0);
  cr_assert_eq(file_get_size(path), 1000);
  unlink(path);
}

Test(engine_runner, invalid_url) {
  Download d = {0};
  strcpy(d.url, "ftp://invalid");
  strcpy(d.dest_path, "/tmp/cdm-invalid-url-never-created");

  int rc = engine_run_download(&d);
  cr_assert_eq(rc, -1, "Invalid URL should return -1");
}

Test(engine_runner, missing_resume_file_is_terminal_and_clears_ranges) {
  Download d = {0};
  d.id = 123;
  strcpy(d.url, "http://example.com/missing");
  strcpy(d.dest_path, "/tmp/cdm-missing-resume-file");
  unlink(d.dest_path);
  d.chunk_count = 1;
  d.total_size = 1000;
  d.chunks[0] = (DownloadChunk){0, 999, 400};
  atomic_store(&d.bytes_downloaded, 400);

  cr_assert_eq(engine_run_download(&d), -4);
  cr_assert_eq(d.chunk_count, 0);
  cr_assert_eq(d.total_size, 0);
  cr_assert_eq(atomic_load(&d.bytes_downloaded), 0);
}

Test(engine_runner, failed_single_stream_fallback_restarts_with_fresh_plan) {
  fallback_scenario = 1;
  Download d = {0};
  d.id = 88;
  strcpy(d.url, "http://127.0.0.1/fallback.bin");
  snprintf(d.dest_path, sizeof(d.dest_path), "/tmp/cdm-engine-fallback-%ld.bin",
           (long)getpid());
  unlink(d.dest_path);
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, NULL), 0);

  cr_assert_eq(engine_run_download(&d), -1);
  cr_assert_eq(fallback_call, 2);
  cr_assert_eq(d.chunk_count, 0);
  DbChunkRow chunks[QM_MAX_CHUNKS];
  cr_assert_eq(db_load_chunks(d.id, chunks, QM_MAX_CHUNKS), 0);
  cr_assert_neq(access(d.dest_path, F_OK), 0);

  cr_assert_eq(engine_run_download(&d), 0);
  cr_assert_eq(fallback_call, 3);
  cr_assert_eq(file_get_size(d.dest_path), fallback_size);
  int fd = open(d.dest_path, O_RDONLY);
  cr_assert_geq(fd, 0);
  char block[4096], expected[4096];
  memset(expected, 'Z', sizeof(expected));
  for (uint64_t offset = 0; offset < fallback_size; offset += sizeof(block)) {
    cr_assert_eq(pread(fd, block, sizeof(block), (off_t)offset),
                 (ssize_t)sizeof(block));
    cr_assert_eq(memcmp(block, expected, sizeof(block)), 0);
  }
  close(fd);
  unlink(d.dest_path);
}
