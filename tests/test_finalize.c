#include "../src/engine/finalize.h"
#include "../src/platform/file_io.h"
#include <criterion/criterion.h>
#include <unistd.h>

#define CORRECT_SIZE_FILE "/tmp/test_finalize_correct.tmp"
#define WRONG_SIZE_FILE "/tmp/test_finalize_wrong.tmp"
#define UNKNOWN_SIZE_FILE "/tmp/test_finalize_unknown.tmp"

TestSuite(finalize);

Test(finalize, correct_size) {
  unlink(CORRECT_SIZE_FILE);
  file_preallocate(CORRECT_SIZE_FILE, 100);
  int ret = engine_finalize(CORRECT_SIZE_FILE, 100, NULL);
  cr_assert_eq(ret, 0);
  unlink(CORRECT_SIZE_FILE);
}

Test(finalize, wrong_size) {
  unlink(WRONG_SIZE_FILE);
  file_preallocate(WRONG_SIZE_FILE, 100);
  int ret = engine_finalize(WRONG_SIZE_FILE, 200, NULL);
  cr_assert_neq(ret, 0);
  unlink(WRONG_SIZE_FILE);
}

Test(finalize, unknown_size) {
  unlink(UNKNOWN_SIZE_FILE);
  file_preallocate(UNKNOWN_SIZE_FILE, 100);
  int ret = engine_finalize(UNKNOWN_SIZE_FILE, 0, NULL);
  cr_assert_eq(ret, 0);
  unlink(UNKNOWN_SIZE_FILE);
}
