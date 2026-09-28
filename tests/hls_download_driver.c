// SPDX-License-Identifier: MIT
#include "../src/core/queue_manager.h"
#include "../src/engine/engine_runner.h"
#include "../src/persistence/db.h"
#include "../src/platform/file_io.h"
#include "../src/utils/config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 5 && argc != 6)
    return 2;
  config_init(argv[4]);
  if (db_init(argv[3]) != 0)
    return 3;
  if (db_restore_queue() != 0)
    return 6;
  Download *d = queue_manager_find_by_id(1);
  if (!d) {
    RequestOptions options = {0};
    options.site_grab = argc == 6 && strcmp(argv[5], "site") == 0;
    options.media_kind = options.site_grab ? DOWNLOAD_MEDIA_NONE
                         : argc == 6 && strncmp(argv[5], "dash", 4) == 0
                             ? DOWNLOAD_MEDIA_DASH
                             : DOWNLOAD_MEDIA_HLS;
    if (argc == 6 && (strcmp(argv[5], "context") == 0 ||
                      strcmp(argv[5], "dash-context") == 0)) {
      options.browser_context = true;
      strcpy(options.cookie, "synthetic=1");
    }
    if (argc == 6 && strcmp(argv[5], "bad-sha") == 0)
      memset(options.expected_sha256, '0', 64);
    uint32_t id = queue_manager_add(argv[1], argv[2], &options);
    if (!id || file_preallocate(argv[2], 0) != 0 ||
        db_insert_reserved_download(id, argv[1], argv[2], &options) != 0)
      return 4;
    d = queue_manager_find_by_id(id);
    d->reserved_file = true;
  }
  if (!d || (!d->site_grab && d->media_kind != DOWNLOAD_MEDIA_HLS &&
             d->media_kind != DOWNLOAD_MEDIA_DASH))
    return 5;
  if (argc == 6 &&
      (strcmp(argv[5], "cancel") == 0 || strcmp(argv[5], "dash-cancel") == 0))
    atomic_store(&d->cancel_requested, true);
  if (argc == 6 &&
      (strcmp(argv[5], "pause") == 0 || strcmp(argv[5], "dash-pause") == 0))
    atomic_store(&d->pause_requested, true);
  int rc = engine_run_download(d);
  /* Exercise restart context recovery from an intentionally paused record. */
  if (rc == 0 && argc == 6 &&
      (strcmp(argv[5], "context") == 0 || strcmp(argv[5], "dash-context") == 0))
    db_update_status(d->id, "PAUSED");
  printf("%d %llu %llu\n", rc, (unsigned long long)d->total_size,
         (unsigned long long)atomic_load(&d->bytes_downloaded));
  db_close();
  return 0;
}
