// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "cli.h"

#include "../platform/ipc_socket.h"
#include "../utils/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/**
 * Parse --cookie, --referrer, --sha256, --limit, --header from
 * argv[i..argc-1] and fill *opts_out.
 *
 * @return true if any option was present (so opts_out is meaningful)
 */
static bool parse_add_options(int argc, char **argv, int first_opt_index,
                              IpcDownloadOptions *opts_out) {
  const char *cookie = NULL;
  const char *referrer = NULL;
  const char *sha256 = NULL;
  uint64_t speed_limit = 0;
  char headers_buf[4096] = {0};

  bool has_options = false;

  for (int i = first_opt_index; i + 1 < argc; i += 2) {
    if (strcmp(argv[i], "--cookie") == 0) {
      cookie = argv[i + 1];
      has_options = true;
    } else if (strcmp(argv[i], "--referrer") == 0) {
      referrer = argv[i + 1];
      has_options = true;
    } else if (strcmp(argv[i], "--sha256") == 0) {
      sha256 = argv[i + 1];
      has_options = true;
    } else if (strcmp(argv[i], "--limit") == 0) {
      speed_limit = (uint64_t)strtoull(argv[i + 1], NULL, 10);
      has_options = true;
    } else if (strcmp(argv[i], "--header") == 0) {
      if (headers_buf[0])
        strncat(headers_buf, "\n",
                sizeof(headers_buf) - strlen(headers_buf) - 1);
      strncat(headers_buf, argv[i + 1],
              sizeof(headers_buf) - strlen(headers_buf) - 1);
      has_options = true;
    } else {
      LOG_WARN("Unknown flag: %s", argv[i]);
    }
  }

  *opts_out = (IpcDownloadOptions){
      .cookie = cookie,
      .referrer = referrer,
      .extra_headers = headers_buf[0] ? headers_buf : NULL,
      .expected_sha256 = sha256,
      .speed_limit_bps = speed_limit,
  };

  return has_options;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int run_cli(int argc, char **argv) {
  /* ---------- usage ---------- */
  if (argc < 2) {
    printf("Usage: downloadmgr cli <command> [args...]\n");
    printf("Commands:\n");
    printf("  add <url> <dest> [--cookie V] [--referrer V] [--header \"K: V\"] "
           "[--sha256 HEX] [--limit BYTES_PER_SEC]\n");
    printf("         (--header may be repeated)\n");
    printf("  pause  <id>          Pause a download\n");
    printf("  resume <id>          Resume a download\n");
    printf("  cancel <id>          Cancel a download\n");
    return 1;
  }

  /* ---------- connect to daemon ---------- */
  int sock = ipc_client_connect();
  if (sock < 0) {
    LOG_ERROR("Cannot connect to daemon");
    fprintf(stderr,
            "Is the daemon running? Start it with: downloadmgr daemon\n");
    return 1;
  }

  const char *cmd = argv[1];
  int ret = 0;

  /* ---------- dispatch ---------- */
  if (strcmp(cmd, "add") == 0 && argc >= 4) {
    IpcDownloadOptions opts;
    bool has_opts = parse_add_options(argc, argv, 4, &opts);

    uint32_t id =
        ipc_send_add_download(sock, argv[2], argv[3], has_opts ? &opts : NULL);
    if (id == 0) {
      LOG_WARN(
          "Daemon rejected the download (invalid or unsafe destination path?)");
    }
    printf("Download added (ID: %u)\n", id);

  } else if (strcmp(cmd, "pause") == 0 && argc >= 3) {
    uint32_t id = (uint32_t)atoi(argv[2]);
    ipc_send_pause(sock, id);
    printf("Paused download %u\n", id);

  } else if (strcmp(cmd, "resume") == 0 && argc >= 3) {
    uint32_t id = (uint32_t)atoi(argv[2]);
    ipc_send_resume(sock, id);
    printf("Resumed download %u\n", id);

  } else if (strcmp(cmd, "cancel") == 0 && argc >= 3) {
    uint32_t id = (uint32_t)atoi(argv[2]);
    ipc_send_cancel(sock, id);
    printf("Cancelled download %u\n", id);

  } else {
    LOG_WARN("Unknown command: %s", cmd);
    ret = 1;
  }

  ipc_client_disconnect(sock);
  return ret;
}
