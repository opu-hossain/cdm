#include "../src/utils/i18n.h"
#include "../src/utils/log.h"
#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Test(i18n, quoted_dotted_keys_load_once_and_fall_back) {
  char path[] = "/tmp/cdm-i18n-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  FILE *file = fdopen(fd, "w");
  cr_assert_not_null(file);
  fputs("\"settings.title\" = \"Settings\"\n"
        "\"download.done\" = \"Finished\"\n"
        "\"gui.settings.title\" = \"Preferencias\"\n"
        "\"notify.download_complete\" = \"Descarga completa\"\n"
        "\"cli.usage.main\" = \"Translated usage\"\n", file);
  cr_assert_eq(fclose(file), 0);
  cr_assert_str_eq(tr("settings.title"), "settings.title");
  cr_assert(tr_load_catalog(path));
  cr_assert_str_eq(tr("settings.title"), "Settings");
  cr_assert_str_eq(tr("download.done"), "Finished");
  cr_assert_str_eq(tr("gui.settings.title"), "Preferencias");
  cr_assert_str_eq(tr("notify.download_complete"), "Descarga completa");
  cr_assert_str_eq(tr("cli.usage.main"), "Translated usage");
  char log_path[] = "/tmp/cdm-i18n-log-XXXXXX";
  int log_fd = mkstemp(log_path);
  cr_assert_geq(log_fd, 0);
  close(log_fd);
  cr_assert(log_init(log_path, LOG_DEBUG));
  cr_assert_str_eq(tr("missing.key"), "missing.key");
  cr_assert_str_eq(tr("missing.key"), "missing.key");
  log_close();
  file = fopen(log_path, "r");
  cr_assert_not_null(file);
  char line[512];
  int occurrences = 0;
  while (fgets(line, sizeof(line), file))
    if (strstr(line, "missing translation key: missing.key"))
      occurrences++;
  fclose(file);
  cr_assert_eq(occurrences, 1);
  cr_assert_not(tr_load_catalog(path));
  unlink(log_path);
  unlink(path);
}

Test(i18n, builtin_cli_english_is_available_without_a_catalog) {
  cr_assert_str_eq(tr("cli.usage.main"),
                   "Usage: cdm cli <command> [args...]");
}

Test(i18n, builtin_gui_english_is_available_without_a_catalog) {
  cr_assert_str_eq(tr("gui.settings.title"), "Settings");
  cr_assert_str_eq(tr("gui.browser.review"), "Review download");
}

Test(i18n, builtin_native_host_error_is_available_without_a_catalog) {
  cr_assert_str_eq(tr("host.error.invalid_json"), "invalid JSON");
}

Test(i18n, builtin_notification_titles_are_available_without_a_catalog) {
  cr_assert_str_eq(tr("notify.download_complete"), "Download Complete");
  cr_assert_str_eq(tr("notify.download_failed"), "Download Failed");
}

Test(i18n, rejects_non_string_values_without_publishing) {
  char path[] = "/tmp/cdm-i18n-XXXXXX";
  int fd = mkstemp(path);
  cr_assert_geq(fd, 0);
  FILE *file = fdopen(fd, "w");
  cr_assert_not_null(file);
  fputs("\"settings.title\" = 42\n", file);
  cr_assert_eq(fclose(file), 0);
  cr_assert_not(tr_load_catalog(path));
  cr_assert_str_eq(tr("settings.title"), "settings.title");
  unlink(path);
}
