#include "../src/platform/file_io.h"
#include <criterion/criterion.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define PREALLOC_FILE "/tmp/test_file_io_preallocate.tmp"
#define MISSING_FILE "/tmp/test_file_io_missing.tmp"
#define ZERO_FILE "/tmp/test_file_io_zero.tmp"

TestSuite(file_io);

Test(file_io, preallocate_and_pwrite) {
  unlink(PREALLOC_FILE);
  int ret = file_preallocate(PREALLOC_FILE, 1024);
  cr_assert_eq(ret, 0);

  FileHandle fd = file_open_rw(PREALLOC_FILE);
  cr_assert_neq(fd, INVALID_FILE_HANDLE);

  char data[] = "Hello world";
  ret = file_pwrite(fd, data, sizeof(data) - 1, 500);
  cr_assert_eq(ret, 0);

  uint64_t size = file_get_size(PREALLOC_FILE);
  cr_assert_eq(size, 1024);

  file_close(fd);
  unlink(PREALLOC_FILE);
}

Test(file_io, open_rw_fails_if_not_exists) {
  unlink(MISSING_FILE);
  FileHandle fd = file_open_rw(MISSING_FILE);
  cr_assert_eq(fd, INVALID_FILE_HANDLE);
}

Test(file_io, preallocate_zero_size) {
  unlink(ZERO_FILE);
  int ret = file_preallocate(ZERO_FILE, 0);
  cr_assert_eq(ret, 0);
  uint64_t size = file_get_size(ZERO_FILE);
  cr_assert_eq(size, 0);
  unlink(ZERO_FILE);
}

Test(file_io, ensure_directory_creates_missing_parents) {
  char root[128];
  char nested[160];
  snprintf(root, sizeof(root), "/tmp/cdm_file_io_%ld", (long)getpid());
  snprintf(nested, sizeof(nested), "%s/one/two", root);
  rmdir(nested);
  rmdir("/tmp/cdm_file_io_unused");
  rmdir(root);

  cr_assert_eq(file_ensure_directory(nested), 0);
  cr_assert_eq(access(nested, F_OK), 0);

  rmdir(nested);
  {
    char parent[144];
    snprintf(parent, sizeof(parent), "%s/one", root);
    rmdir(parent);
  }
  rmdir(root);
}
