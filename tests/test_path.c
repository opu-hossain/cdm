#include "../src/utils/path.h"
#include <criterion/criterion.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char test_dir[128];

static void setup(void) {
  snprintf(test_dir, sizeof(test_dir), "/tmp/cdm_path_%llu",
           (unsigned long long)getpid());
  mkdir(test_dir, 0755);
}

static void teardown(void) {
  char path[256];
  snprintf(path, sizeof(path), "%s/report.txt", test_dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/report (1).txt", test_dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/report (2).txt", test_dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/download (1)", test_dir);
  unlink(path);
  rmdir(test_dir);
}

TestSuite(path, .init = setup, .fini = teardown);

Test(path, keeps_unused_path) {
  char input[256];
  char output[256];
  snprintf(input, sizeof(input), "%s/report.txt", test_dir);
  cr_assert(path_make_unique(input, output, sizeof(output)));
  cr_assert_str_eq(output, input);
}

Test(path, adds_suffix_before_extension) {
  char input[256];
  char output[256];
  snprintf(input, sizeof(input), "%s/report.txt", test_dir);
  FILE *file = fopen(input, "w");
  cr_assert_not_null(file);
  fclose(file);

  cr_assert(path_make_unique(input, output, sizeof(output)));
  char expected[256];
  snprintf(expected, sizeof(expected), "%s/report (1).txt", test_dir);
  cr_assert_str_eq(output, expected);
}

Test(path, increments_existing_suffixes) {
  char input[256];
  char occupied[256];
  snprintf(input, sizeof(input), "%s/report.txt", test_dir);
  snprintf(occupied, sizeof(occupied), "%s/report (1).txt", test_dir);
  FILE *file = fopen(input, "w");
  cr_assert_not_null(file);
  fclose(file);
  file = fopen(occupied, "w");
  cr_assert_not_null(file);
  fclose(file);

  char output[256];
  cr_assert(path_make_unique(input, output, sizeof(output)));
  char expected[256];
  snprintf(expected, sizeof(expected), "%s/report (2).txt", test_dir);
  cr_assert_str_eq(output, expected);
}

Test(path, adds_suffix_to_extensionless_file) {
  char input[256];
  char output[256];
  snprintf(input, sizeof(input), "%s/download", test_dir);
  FILE *file = fopen(input, "w");
  cr_assert_not_null(file);
  fclose(file);

  cr_assert(path_make_unique(input, output, sizeof(output)));
  char expected[256];
  snprintf(expected, sizeof(expected), "%s/download (1)", test_dir);
  cr_assert_str_eq(output, expected);
}