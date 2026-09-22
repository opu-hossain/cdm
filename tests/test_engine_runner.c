#include "../src/core/queue_manager.h"
#include "../src/engine/engine_runner.h"
#include "../src/engine/worker_pool.h"
#include "../src/persistence/db.h"
#include "../src/platform/curl_client.h"
#include "../src/platform/file_io.h"
#include <criterion/criterion.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Mocks for external dependencies */
int curl_client_head(const char *url, const RequestContext *ctx, FileInfo *out) {
  (void)url;
  (void)ctx;
  out->total_size = 1000;
  out->supports_ranges = true;
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
  WorkerPoolResult res = {.all_succeeded = true,
                          .total_bytes_downloaded = 1000};
  for (int i = 0; i < n_workers; i++) {
    res.chunk_succeeded[i] = true;
  }
  file_preallocate(dest_path, 1000);
  return res;
}

RebalancePool *rebalance_pool_create(const Range *ranges, int n_ranges,
                                     _Atomic uint64_t **progress_slots,
                                     uint64_t min_steal_bytes,
                                     RebalanceSplitFn on_split, void *userdata) {
  (void)ranges;
  (void)n_ranges;
  (void)progress_slots;
  (void)min_steal_bytes;
  (void)on_split;
  (void)userdata;
  return NULL;
}

void rebalance_pool_destroy(RebalancePool *pool) { (void)pool; }

/* Setup / teardown */
static void setup_engine_test(void) {
  setenv("DOWNLOADMGR_ROOT", "/tmp", 1);
  db_init(":memory:"); // use in‑memory DB to avoid "out of memory" errors
}

static void teardown_engine_test(void) {
  db_close();
  unlink("/tmp/test_engine_out");
}

TestSuite(engine_runner, .init = setup_engine_test,
          .fini = teardown_engine_test);

Test(engine_runner, run_success) {
  Download d = {0};
  strcpy(d.url, "http://example.com/file");
  strcpy(d.dest_path, "/tmp/test_engine_out");

  int rc = engine_run_download(&d);
  cr_assert_eq(rc, 0, "engine_run_download should return 0 on success");

  uint64_t size = file_get_size("/tmp/test_engine_out");
  cr_assert_eq(size, 1000, "Output file should be 1000 bytes");
}

Test(engine_runner, invalid_url) {
  Download d = {0};
  strcpy(d.url, "ftp://invalid");
  strcpy(d.dest_path, "/tmp/test_engine_out");

  int rc = engine_run_download(&d);
  cr_assert_eq(rc, -1, "Invalid URL should return -1");
}
