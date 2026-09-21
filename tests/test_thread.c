#include "../src/platform/thread.h"
#include <criterion/criterion.h>
#include <stdatomic.h>

static int thread_func(void *arg) {
  atomic_int *counter = (atomic_int *)arg;
  atomic_fetch_add(counter, 1);
  return 123;
}

Test(thread, create_and_join) {
  dm_thread_t t;
  atomic_int counter = 0;
  int rc = dm_thread_create(&t, thread_func, &counter);
  cr_assert_eq(rc, 0);
  int result;
  rc = dm_thread_join(&t, &result);
  cr_assert_eq(rc, 0);
  cr_assert_eq(result, 123);
  cr_assert_eq(atomic_load(&counter), 1);
}

Test(thread, mutex) {
  dm_mutex_t m;
  int rc = dm_mutex_init(&m);
  cr_assert_eq(rc, 0);
  rc = dm_mutex_lock(&m);
  cr_assert_eq(rc, 0);
  rc = dm_mutex_unlock(&m);
  cr_assert_eq(rc, 0);
  dm_mutex_destroy(&m);
}

Test(thread, sleep) {
  // Just ensure it doesn't crash
  dm_thread_sleep_ms(10);
  cr_assert(1);
}
