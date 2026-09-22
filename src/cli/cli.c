// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "cli.h"

#include "../platform/ipc_socket.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "platform/ipc_protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/**
 *
 */
static void filename_from_url(const char *url, char *out, size_t out_size) {
  const char *slash = strrchr(url, '/');
  const char *name = slash ? slash + 1 : url;

  char temp[512];
  strncpy(temp, name, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = '\0';

  char *query = strchr(temp, '?');
  if (query)
    *query = '\0';

  if (temp[0] == '\0')
    strncpy(temp, "download.bin", sizeof(temp) - 1);

  strncpy(out, temp, out_size - 1);
  out[out_size - 1] = '\0';
}

static bool join_path(const char *dir, const char *filename, char *out,
                      size_t out_size) {
#ifdef _WIN32
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  size_t dir_len = strlen(dir);
  if (dir_len > 0 && dir[dir_len - 1] == sep) {
    size_t filename_len = strlen(filename);
    if (dir_len + filename_len + 1 > out_size)
      return false;
    memcpy(out, dir, dir_len);
    memcpy(out + dir_len, filename, filename_len + 1);
  } else {
    size_t filename_len = strlen(filename);
    if (dir_len + 1 + filename_len + 1 > out_size)
      return false;
    memcpy(out, dir, dir_len);
    out[dir_len] = sep;
    memcpy(out + dir_len + 1, filename, filename_len + 1);
  }
  return true;
}

/**
 * Parse --cookie, --referrer, --sha256, --limit, --header from
 * argv[i..argc-1] and fill *opts_out.
 *
 * @return true if any option was present (so opts_out is meaningful)
 */
static bool parse_u64(const char *text, uint64_t *out) {
  if (!text || text[0] == '\0' || text[0] == '-')
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0')
    return false;
  *out = (uint64_t)value;
  return true;
}

static bool parse_id(const char *text, uint32_t *out) {
  uint64_t value = 0;
  if (!parse_u64(text, &value) || value == 0 || value > UINT32_MAX)
    return false;
  *out = (uint32_t)value;
  return true;
}

static bool parse_add_options(int argc, char **argv, int first_opt_index,
                              IpcDownloadOptions *opts_out, bool *valid_out) {
  const char *cookie = NULL;
  const char *referrer = NULL;
  const char *sha256 = NULL;
  uint64_t speed_limit = 0;
  char headers_buf[4096] = {0};

  bool has_options = false;
  bool valid = (argc - first_opt_index) % 2 == 0;

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
      if (!parse_u64(argv[i + 1], &speed_limit)) {
        LOG_WARN("Invalid speed limit: %s", argv[i + 1]);
        valid = false;
      }
      has_options = true;
    } else if (strcmp(argv[i], "--header") == 0) {
      size_t current_len = strlen(headers_buf);
      size_t value_len = strlen(argv[i + 1]);
      size_t separator_len = headers_buf[0] ? 1 : 0;
      if (current_len + separator_len + value_len >= sizeof(headers_buf)) {
        LOG_WARN("Header is too long");
        valid = false;
      } else {
        if (separator_len)
          headers_buf[current_len++] = '\n';
        memcpy(headers_buf + current_len, argv[i + 1], value_len + 1);
      }
      has_options = true;
    } else {
      LOG_WARN("Unknown flag: %s", argv[i]);
      valid = false;
    }
  }

  *opts_out = (IpcDownloadOptions){
      .cookie = cookie,
      .referrer = referrer,
      .extra_headers = headers_buf[0] ? headers_buf : NULL,
      .expected_sha256 = sha256,
      .speed_limit_bps = speed_limit,
  };

  *valid_out = valid;
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
    printf("  add <url> [dest_dir] [--cookie V] [--referrer V] "
           "[--header \"K: V\"] [--sha256 HEX] [--limit BYTES_PER_SEC]\n");
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
  if (strcmp(cmd, "add") == 0 && argc >= 3) {
    const char *url = argv[2];
    int first_opt_index = 3;
    const char *dest_dir = NULL;

    if (argc >= 4 && strncmp(argv[3], "--", 2) != 0) {
      dest_dir = argv[3];
      first_opt_index = 4;
    }
    char filename[512];
    char full_path[IPC_MAX_PATH_LEN];
    if (!dest_dir || dest_dir[0] == '\0') {
      dest_dir = config_get_default_download_dir();
    }
    filename_from_url(url, filename, sizeof(filename));
    if (!join_path(dest_dir, filename, full_path, sizeof(full_path))) {
      LOG_ERROR("Destination path too long");
      ipc_client_disconnect(sock);
      return 1;
    }

    IpcDownloadOptions opts;
    bool valid_opts = false;
    bool has_opts =
        parse_add_options(argc, argv, first_opt_index, &opts, &valid_opts);
    if (!valid_opts) {
      fprintf(stderr, "Invalid add options\n");
      ipc_client_disconnect(sock);
      return 1;
    }

    uint32_t id =
        ipc_send_add_download(sock, url, full_path, has_opts ? &opts : NULL);
    if (id == 0) {
      LOG_WARN("Daemon rejected the download (invalid or unsafe destination "
               "path?)");
    }
    printf("Download added (ID: %u, saved to %s)\n", id, full_path);

  } else if (strcmp(cmd, "pause") == 0 && argc >= 3) {
    uint32_t id = 0;
    if (!parse_id(argv[2], &id)) {
      fprintf(stderr, "Invalid download ID: %s\n", argv[2]);
      ipc_client_disconnect(sock);
      return 1;
    }
    if (ipc_send_pause(sock, id) == 0)
      printf("Paused download %u\n", id);
    else {
      fprintf(stderr, "Could not pause download %u\n", id);
      ret = 1;
    }

  } else if (strcmp(cmd, "resume") == 0 && argc >= 3) {
    uint32_t id = 0;
    if (!parse_id(argv[2], &id)) {
      fprintf(stderr, "Invalid download ID: %s\n", argv[2]);
      ipc_client_disconnect(sock);
      return 1;
    }
    if (ipc_send_resume(sock, id) == 0)
      printf("Resumed download %u\n", id);
    else {
      fprintf(stderr, "Could not resume download %u\n", id);
      ret = 1;
    }

  } else if (strcmp(cmd, "cancel") == 0 && argc >= 3) {
    uint32_t id = 0;
    if (!parse_id(argv[2], &id)) {
      fprintf(stderr, "Invalid download ID: %s\n", argv[2]);
      ipc_client_disconnect(sock);
      return 1;
    }
    if (ipc_send_cancel(sock, id) == 0)
      printf("Cancelled download %u\n", id);
    else {
      fprintf(stderr, "Could not cancel download %u\n", id);
      ret = 1;
    }

  } else {
    LOG_WARN("Unknown command: %s", cmd);
    ret = 1;
  }

  ipc_client_disconnect(sock);
  return ret;
}
