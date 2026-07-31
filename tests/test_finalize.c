#include "../src/engine/finalize.h"
#include "../src/platform/file_io.h"
#include <criterion/criterion.h>
#include <unistd.h>

#define TEST_FILE "/tmp/test_finalize.tmp"

static void setup_file(void) {
  unlink(TEST_FILE);
  file_preallocate(TEST_FILE, 100);
}

static void teardown_file(void) { unlink(TEST_FILE); }

TestSuite(finalize, .init = setup_file, .fini = teardown_file);

Test(finalize, correct_size) {
  int ret = engine_finalize(TEST_FILE, 100, "abc");
  cr_assert_eq(ret, 0);
}

Test(finalize, wrong_size) {
  int ret = engine_finalize(TEST_FILE, 200, "abc");
  cr_assert_neq(ret, 0);
}

Test(finalize, unknown_size) {
  int ret = engine_finalize(TEST_FILE, 0, "abc");
  cr_assert_eq(ret, 0);
}
