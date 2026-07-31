#include "../src/platform/file_io.h"
#include <criterion/criterion.h>
#include <string.h>
#include <unistd.h>

#define TEST_FILE "/tmp/test_file_io.tmp"

static void setup_file(void) { unlink(TEST_FILE); }

static void teardown_file(void) { unlink(TEST_FILE); }

TestSuite(file_io, .init = setup_file, .fini = teardown_file);

Test(file_io, preallocate_and_pwrite) {
  int ret = file_preallocate(TEST_FILE, 1024);
  cr_assert_eq(ret, 0);

  FileHandle fd = file_open_rw(TEST_FILE);
  cr_assert_neq(fd, INVALID_FILE_HANDLE);

  char data[] = "Hello world";
  ret = file_pwrite(fd, data, sizeof(data) - 1, 500);
  cr_assert_eq(ret, 0);

  uint64_t size = file_get_size(TEST_FILE);
  cr_assert_eq(size, 1024);

  file_close(fd);
}

Test(file_io, open_rw_fails_if_not_exists) {
  unlink(TEST_FILE);
  FileHandle fd = file_open_rw(TEST_FILE);
  cr_assert_eq(fd, INVALID_FILE_HANDLE);
}

Test(file_io, preallocate_zero_size) {
  int ret = file_preallocate(TEST_FILE, 0);
  cr_assert_eq(ret, 0);
  uint64_t size = file_get_size(TEST_FILE);
  cr_assert_eq(size, 0);
}
