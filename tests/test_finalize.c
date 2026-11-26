#include "../src/engine/finalize.h"
#include "../src/platform/file_io.h"
#include <criterion/criterion.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

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

Test(finalize, quarantine_preserves_existing_name) {
  char dir[] = "/tmp/cdm-finalize-quarantine-XXXXXX";
  cr_assert_not_null(mkdtemp(dir));
  char path[256], moved[256], expected[256];
  int n = snprintf(path, sizeof(path), "%s/sample.bin", dir);
  cr_assert_geq(n, 0); cr_assert_lt((size_t)n, sizeof(path));
  FILE *fp = fopen(path, "wb");
  cr_assert_not_null(fp); cr_assert_geq(fputs("first", fp), 0);
  cr_assert_eq(fclose(fp), 0);
  cr_assert_eq(engine_quarantine_output(path, moved, sizeof(moved)), 0);
  cr_assert_neq(access(moved, F_OK), -1);
  cr_assert_eq(access(path, F_OK), -1);
  fp = fopen(path, "wb");
  cr_assert_not_null(fp); cr_assert_geq(fputs("second", fp), 0);
  cr_assert_eq(fclose(fp), 0);
  cr_assert_eq(engine_quarantine_output(path, expected, sizeof(expected)), 0);
  cr_assert_str_neq(moved, expected);
  cr_assert_neq(access(expected, F_OK), -1);
  unlink(moved); unlink(expected);
  n = snprintf(path, sizeof(path), "%s/.quarantine", dir);
  cr_assert_geq(n, 0); cr_assert_lt((size_t)n, sizeof(path));
  rmdir(path); rmdir(dir);
}
