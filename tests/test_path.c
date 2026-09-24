#include "../src/utils/path.h"
#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char test_dir[128];

static void setup(void) {
  strcpy(test_dir, "/tmp/cdm_path_XXXXXX");
  cr_assert_not_null(mkdtemp(test_dir));
}

static void teardown(void) {
  char path[256];
  const char *names[] = {"report.txt", "report(1).txt", "report(2).txt",
                         "report(3).txt", "archive.tar.gz", "README",
                         "missing.apk", "missing(1).apk"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s", test_dir, names[i]);
    unlink(path);
  }
  rmdir(test_dir);
}

static void create_file(const char *path) {
  FILE *file = fopen(path, "w");
  cr_assert_not_null(file);
  fclose(file);
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
  create_file(input);

  cr_assert(path_make_unique(input, output, sizeof(output)));
  char expected[256];
  snprintf(expected, sizeof(expected), "%s/report(1).txt", test_dir);
  cr_assert_str_eq(output, expected);
}

Test(path, increments_existing_suffixes) {
  char input[256];
  char occupied[256];
  snprintf(input, sizeof(input), "%s/report.txt", test_dir);
  snprintf(occupied, sizeof(occupied), "%s/report(1).txt", test_dir);
  create_file(input);
  create_file(occupied);

  char output[256];
  cr_assert(path_make_unique(input, output, sizeof(output)));
  char expected[256];
  snprintf(expected, sizeof(expected), "%s/report(2).txt", test_dir);
  cr_assert_str_eq(output, expected);

  create_file(output);
  cr_assert(path_make_unique(input, output, sizeof(output)));
  snprintf(expected, sizeof(expected), "%s/report(3).txt", test_dir);
  cr_assert_str_eq(output, expected);
}

Test(path, adds_suffix_to_extensionless_file) {
  char input[256];
  char output[256];
  snprintf(input, sizeof(input), "%s/README", test_dir);
  create_file(input);

  cr_assert(path_make_unique(input, output, sizeof(output)));
  char expected[256];
  snprintf(expected, sizeof(expected), "%s/README(1)", test_dir);
  cr_assert_str_eq(output, expected);
}

Test(path, keeps_compound_archive_extension) {
  char input[256];
  char output[256];
  char expected[256];
  snprintf(input, sizeof(input), "%s/archive.tar.gz", test_dir);
  create_file(input);
  cr_assert(path_make_unique(input, output, sizeof(output)));
  snprintf(expected, sizeof(expected), "%s/archive(1).tar.gz", test_dir);
  cr_assert_str_eq(output, expected);
}

Test(path, deleted_file_frees_its_name) {
  char input[256];
  char output[256];
  snprintf(input, sizeof(input), "%s/report.txt", test_dir);
  create_file(input);
  cr_assert_eq(unlink(input), 0);
  cr_assert(path_make_unique(input, output, sizeof(output)));
  cr_assert_str_eq(output, input);
}

Test(path, dangling_symlink_occupies_its_name) {
  char input[256];
  char output[256];
  char expected[256];
  snprintf(input, sizeof(input), "%s/missing.apk", test_dir);
  cr_assert_eq(symlink("nonexistent-target", input), 0);
  cr_assert(path_make_unique(input, output, sizeof(output)));
  snprintf(expected, sizeof(expected), "%s/missing(1).apk", test_dir);
  cr_assert_str_eq(output, expected);
}
