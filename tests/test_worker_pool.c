#include "../src/engine/worker_pool.h"
#include "../src/platform/file_io.h"
#include <criterion/criterion.h>
#include <stdatomic.h>
#include <unistd.h>

Test(worker_pool, single_worker_failure) {
  const char *test_file = "/tmp/test_worker_pool_fail";
  unlink(test_file);
  file_preallocate(test_file, 100);

  Range ranges[1] = {
      {.start = 0, .end = 99, .resume_offset = 0, .whole_file = false}};
  _Atomic uint64_t total_bytes = 0;
  _Atomic bool cancel = false;
  _Atomic bool pause = false;
  _Atomic uint64_t *slots[1] = {NULL};

  // Use a non‑routable address to force failure.
  WorkerPoolResult res =
      worker_pool_run("http://127.0.0.1:1/", ranges, 1, test_file, &total_bytes,
                      &cancel, &pause, slots, 1024, NULL, NULL);
  cr_assert(!res.all_succeeded);
  unlink(test_file);
}
