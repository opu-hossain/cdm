// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "cli.h"

#include "../persistence/export.h"
#include "../platform/ipc_socket.h"
#include "../utils/config.h"
#include "../utils/i18n.h"
#include "../utils/log.h"
#include "../utils/path.h"
#include "platform/ipc_protocol.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Helpers */

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
                              char *headers_buf, size_t headers_capacity,
                              IpcDownloadOptions *opts_out, bool *valid_out) {
  const char *cookie = NULL;
  const char *referrer = NULL;
  const char *sha256 = NULL;
  uint64_t speed_limit = 0;
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
      if (current_len + separator_len + value_len >= headers_capacity) {
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

static void print_list_row(const IpcDownloadRecord *row) {
  const char *filename = strrchr(row->dest_path, '/');
  filename = filename ? filename + 1 : row->dest_path;
  char percent[16], size[32];
  if (row->progress < 0)
    snprintf(percent, sizeof(percent), "?");
  else
    snprintf(percent, sizeof(percent), "%.0f%%", row->progress * 100.0f);
  if (row->total_size)
    snprintf(size, sizeof(size), "%llu",
             (unsigned long long)row->total_size);
  else
    snprintf(size, sizeof(size), "?");
  printf("%u\t%s\t%s\t%s\t", row->id, row->status, percent, size);
  for (const char *p = filename; *p; p++)
    putchar(*p == '\t' || *p == '\n' || *p == '\r' ? ' ' : *p);
  putchar('\n');
}

static int run_list(int sock, uint16_t daemon_version, int argc, char **argv) {
  uint32_t offset = 0, limit = 100;
  char status[16] = {0};
  if ((argc - 2) % 2 != 0) {
    fprintf(stderr, "%s\n", tr("cli.usage.list"));
    return 1;
  }
  for (int i = 2; i < argc; i += 2) {
    uint64_t value;
    if (strcmp(argv[i], "--offset") == 0) {
      if (!parse_u64(argv[i + 1], &value) || value > UINT32_MAX) {
        fprintf(stderr, "%s %s\n", tr("cli.error.invalid_offset"), argv[i + 1]);
        return 1;
      }
      offset = (uint32_t)value;
    } else if (strcmp(argv[i], "--limit") == 0) {
      if (!parse_u64(argv[i + 1], &value) || value == 0 ||
          value > IPC_LIST_PAGE_MAX) {
        fprintf(stderr, "%s%d\n", tr("cli.error.invalid_limit"), IPC_LIST_PAGE_MAX);
        return 1;
      }
      limit = (uint32_t)value;
    } else if (strcmp(argv[i], "--status") == 0) {
      size_t length = strlen(argv[i + 1]);
      if (length == 0 || length >= sizeof(status)) {
        fprintf(stderr, "%s\n", tr("cli.error.invalid_status"));
        return 1;
      }
      for (size_t j = 0; j < length; j++)
        status[j] = (char)toupper((unsigned char)argv[i + 1][j]);
      if (strcmp(status, "QUEUED") && strcmp(status, "ACTIVE") &&
          strcmp(status, "PAUSED") && strcmp(status, "DONE") &&
          strcmp(status, "ERROR") && strcmp(status, "CANCELED")) {
        fprintf(stderr, "%s: %s\n", tr("cli.error.invalid_status"), argv[i + 1]);
        return 1;
      }
    } else {
      fprintf(stderr, "%s %s\n", tr("cli.error.unknown_list_flag"), argv[i]);
      return 1;
    }
  }

  if (daemon_version < 2) {
    fprintf(stderr, "%s\n", tr("cli.error.list_size_unsupported"));
    return 1;
  }
  IpcDownloadRecord *rows = calloc(IPC_LIST_PAGE_MAX, sizeof(*rows));
  if (!rows)
    return 1;
  uint32_t raw_offset = status[0] ? 0 : offset;
  uint64_t matched = 0;
  uint32_t printed = 0;
  bool header_printed = false;
  int result = 0;
  while (printed < limit) {
    uint32_t total = 0;
    uint32_t request_limit = status[0] ? IPC_LIST_PAGE_MAX : limit;
    int count = ipc_send_list_page_with_size(sock, raw_offset, request_limit,
                                              rows, IPC_LIST_PAGE_MAX, &total);
    if (count < 0) {
      fprintf(stderr, "%s\n", tr("cli.error.list_ipc"));
      result = 1;
      break;
    }
    if (!header_printed) {
      puts(tr("cli.list.header"));
      header_printed = true;
    }
    for (int i = 0; i < count && printed < limit; i++) {
      if (status[0] && strcmp(rows[i].status, status) != 0)
        continue;
      if (status[0] && matched++ < offset)
        continue;
      print_list_row(&rows[i]);
      printed++;
    }
    uint64_t next_offset = (uint64_t)raw_offset + (uint32_t)count;
    if (count == 0 || next_offset >= total || !status[0])
      break;
    raw_offset = (uint32_t)next_offset;
  }
  free(rows);
  return result;
}

static int run_export(int sock, uint16_t daemon_version, int argc,
                      char **argv) {
  const char *destination = NULL;
  bool history = false, secrets = false;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc && !destination)
      destination = argv[++i];
    else if (strcmp(argv[i], "--include-history") == 0 && !history)
      history = true;
    else if (strcmp(argv[i], "--include-secrets") == 0 && !secrets)
      secrets = true;
    else {
      fprintf(stderr, "%s\n", tr("cli.usage.export"));
      return 1;
    }
  }
  if (!destination || !destination[0]) {
    fprintf(stderr, "%s\n", tr("cli.usage.export"));
    return 1;
  }
  if (secrets)
    fprintf(stderr, "%s\n", tr("cli.warn.export_secrets"));
  if (history)
    fprintf(stderr, "%s\n", tr("cli.warn.export_history"));
  if (export_json_file(sock, daemon_version, destination, history, secrets) != 0) {
    fprintf(stderr, "%s\n", tr("cli.error.export"));
    return 1;
  }
  printf("%s %s\n", tr("cli.exported"), destination);
  return 0;
}

static int run_import(int sock, uint16_t daemon_version, int argc,
                      char **argv) {
  const char *source = NULL;
  bool replace = false, mode_set = false, yes = false;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--in") == 0 && i + 1 < argc && !source)
      source = argv[++i];
    else if (strcmp(argv[i], "--merge") == 0 && !mode_set)
      mode_set = true;
    else if (strcmp(argv[i], "--replace") == 0 && !mode_set) {
      mode_set = true;
      replace = true;
    } else if (strcmp(argv[i], "--yes") == 0 && !yes)
      yes = true;
    else {
      fprintf(stderr, "%s\n", tr("cli.usage.import"));
      return 1;
    }
  }
  if (!source || !source[0] || (yes && !replace)) {
    fprintf(stderr, "%s\n", tr("cli.usage.import"));
    return 1;
  }
  if (daemon_version < 12) {
    fprintf(stderr, "%s\n", tr("cli.error.import_version"));
    return 1;
  }
  if (replace && !yes) {
    if (!isatty(STDIN_FILENO)) {
      fprintf(stderr, "%s\n", tr("cli.error.import_yes"));
      return 1;
    }
    char answer[16];
    fputs(tr("cli.import.confirm"), stderr);
    if (!fgets(answer, sizeof(answer), stdin) ||
        (strcmp(answer, "y\n") != 0 && strcmp(answer, "yes\n") != 0))
      return 1;
  }
  /* TODO(platform): choose the native path canonicalization/console prompt. */
  char resolved[PATH_MAX];
  if (!realpath(source, resolved)) {
    fprintf(stderr, "%s %s\n", tr("cli.error.import_file"), strerror(errno));
    return 1;
  }
  IpcResult result = IPC_RESULT_ERROR;
  if (ipc_send_import_json_v1(sock, resolved, replace, &result) != 0 ||
      result != IPC_RESULT_OK) {
    fprintf(stderr, "%s (%s)\n", tr("cli.error.import"),
            result == IPC_RESULT_REJECTED ? tr("cli.error.import_rejected")
                                          : tr("cli.error.import_daemon"));
    return 1;
  }
  puts(tr(replace ? "cli.import.replaced" : "cli.import.merged"));
  return 0;
}

static int submit_add(int sock, uint16_t daemon_version, const char *url,
                      const char *dest_dir, const IpcDownloadOptions *opts) {
  IpcDownloadOptions selected_opts = opts ? *opts : (IpcDownloadOptions){0};
  selected_opts.auto_directory = !dest_dir || !dest_dir[0];
  char filename[512];
  char full_path[IPC_MAX_PATH_LEN];
  char unique_path[IPC_MAX_PATH_LEN];
  if (!dest_dir || !dest_dir[0])
    dest_dir = config_get_default_download_dir();
  path_filename_from_url(url, filename, sizeof(filename));
  bool prepared = path_join(dest_dir, filename, full_path, sizeof(full_path));
  if (prepared && selected_opts.auto_directory && daemon_version >= 6)
    memcpy(unique_path, full_path, strlen(full_path) + 1);
  else if (prepared)
    prepared = path_make_unique(full_path, unique_path, sizeof(unique_path));
  if (!prepared) {
    fprintf(stderr, "%s\n", tr("cli.error.destination"));
    return 1;
  }
  IpcAddResponse add = {0};
  if (daemon_version >= 6) {
    if (ipc_send_add_download_v3(sock, url, unique_path, &selected_opts,
                                 true, &add) != 0)
      add.result = IPC_RESULT_ERROR;
  } else if (daemon_version >= 3) {
    if (ipc_send_add_download_v2(sock, url, unique_path, opts, true, &add) != 0)
      add.result = IPC_RESULT_ERROR;
  } else {
    add.id = ipc_send_add_download_auto(sock, url, unique_path, opts);
    add.result = add.id ? IPC_RESULT_OK : IPC_RESULT_ERROR;
  }
  if (!add.id) {
    fprintf(stderr, "%s\n", tr("cli.error.add_rejected"));
    return 1;
  }
  if (add.result == IPC_RESULT_REJECTED)
    printf("%s %u)\n", tr("cli.add.duplicate"), add.id);
  else
    printf("%s %u%s %s%s\n", tr("cli.add.created"), add.id,
           tr("cli.add.initial_path"), unique_path,
           tr("cli.add.final_note"));
  return 0;
}

static bool batch_url_valid(const char *url) {
  const char *host = NULL;
  if (strncmp(url, "https://", 8) == 0)
    host = url + 8;
  else if (strncmp(url, "http://", 7) == 0)
    host = url + 7;
  if (!host || !*host || *host == '/' || *host == '?' || *host == '#')
    return false;
  for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
    if (isspace(*p) || iscntrl(*p))
      return false;
  return true;
}

static int run_batch_add(int sock, uint16_t daemon_version, const char *file,
                         const char *dest_dir) {
  FILE *fp = fopen(file, "r");
  if (!fp) {
    fprintf(stderr, "%s %s\n", tr("cli.error.url_file"), strerror(errno));
    return 2;
  }
  char line[4096];
  unsigned long line_number = 0;
  int result = 0;
  while (fgets(line, sizeof(line), fp)) {
    line_number++;
    size_t length = strlen(line);
    bool complete = length && line[length - 1] == '\n';
    if (!complete && !feof(fp)) {
      int ch;
      while ((ch = fgetc(fp)) != '\n' && ch != EOF) {}
      fprintf(stderr, "%s %lu: %s\n", tr("cli.batch.line"), line_number,
              tr("cli.error.url_long"));
      result = 1;
      continue;
    }
    while (length && (line[length - 1] == '\n' ||
                      line[length - 1] == '\r' ||
                      isspace((unsigned char)line[length - 1])))
      line[--length] = '\0';
    char *url = line;
    while (isspace((unsigned char)*url))
      url++;
    if (!*url || *url == '#')
      continue;
    printf("%s %lu: ", tr("cli.batch.line"), line_number);
    if (!batch_url_valid(url) ||
        submit_add(sock, daemon_version, url, dest_dir, NULL) != 0) {
      puts(tr("cli.batch.failed"));
      result = 1;
    }
  }
  if (ferror(fp)) {
    fprintf(stderr, "%s %s\n", tr("cli.error.url_file"), strerror(errno));
    result = 2;
  }
  fclose(fp);
  return result;
}

/* Public API */

int run_cli(int argc, char **argv) {
  /* ---------- usage ---------- */
  if (argc < 2) {
    puts(tr("cli.usage.main"));
    puts(tr("cli.commands"));
    puts(tr("cli.help.add"));
    puts(tr("cli.help.header"));
    puts(tr("cli.help.batch"));
    puts(tr("cli.help.pause"));
    puts(tr("cli.help.resume"));
    puts(tr("cli.help.cancel"));
    puts(tr("cli.help.list"));
    puts(tr("cli.help.export"));
    puts(tr("cli.help.import"));
    return 1;
  }

  if (argc >= 4 && strcmp(argv[1], "add") == 0 &&
      strcmp(argv[2], "--file") == 0) {
    FILE *probe = fopen(argv[3], "r");
    if (!probe) {
      fprintf(stderr, "%s %s\n", tr("cli.error.url_file"), strerror(errno));
      return 2;
    }
    fclose(probe);
  }

  /* ---------- connect to daemon ---------- */
  uint16_t daemon_version = 1;
  int sock = ipc_client_connect_compatible(-1, &daemon_version);
  if (sock < 0) {
    LOG_ERROR("Cannot connect to daemon");
    fprintf(stderr, "%s\n", tr("cli.error.daemon"));
    return 1;
  }

  const char *cmd = argv[1];
  int ret = 0;

  /* ---------- dispatch ---------- */
  if (strcmp(cmd, "list") == 0) {
    ret = run_list(sock, daemon_version, argc, argv);

  } else if (strcmp(cmd, "export") == 0) {
    ret = run_export(sock, daemon_version, argc, argv);

  } else if (strcmp(cmd, "import") == 0) {
    ret = run_import(sock, daemon_version, argc, argv);

  } else if (strcmp(cmd, "add") == 0 && argc >= 3 &&
             strcmp(argv[2], "--file") == 0) {
    if (argc < 4 || argc > 5) {
      fprintf(stderr, "%s\n", tr("cli.usage.batch"));
      ret = 1;
    } else
      ret = run_batch_add(sock, daemon_version, argv[3],
                          argc == 5 ? argv[4] : NULL);

  } else if (strcmp(cmd, "add") == 0 && argc >= 3) {
    const char *url = argv[2];
    int first_opt_index = 3;
    const char *dest_dir = NULL;

    if (argc >= 4 && strncmp(argv[3], "--", 2) != 0) {
      dest_dir = argv[3];
      first_opt_index = 4;
    }
    IpcDownloadOptions opts;
    char headers_buf[4096] = {0};
    bool valid_opts = false;
    bool has_opts =
        parse_add_options(argc, argv, first_opt_index, headers_buf,
                          sizeof(headers_buf), &opts, &valid_opts);
    if (!valid_opts) {
      fprintf(stderr, "%s\n", tr("cli.error.add_options"));
      ipc_client_disconnect(sock);
      return 1;
    }

    ret = submit_add(sock, daemon_version, url, dest_dir,
                     has_opts ? &opts : NULL);

  } else if (strcmp(cmd, "pause") == 0 && argc >= 3) {
    uint32_t id = 0;
    if (!parse_id(argv[2], &id)) {
      fprintf(stderr, "%s %s\n", tr("cli.error.download_id"), argv[2]);
      ipc_client_disconnect(sock);
      return 1;
    }
    if (ipc_send_pause(sock, id) == 0)
      printf("%s %u\n", tr("cli.pause.done"), id);
    else {
      fprintf(stderr, "%s %u\n", tr("cli.pause.failed"), id);
      ret = 1;
    }

  } else if (strcmp(cmd, "resume") == 0 && argc >= 3) {
    uint32_t id = 0;
    if (!parse_id(argv[2], &id)) {
      fprintf(stderr, "%s %s\n", tr("cli.error.download_id"), argv[2]);
      ipc_client_disconnect(sock);
      return 1;
    }
    if (ipc_send_resume(sock, id) == 0)
      printf("%s %u\n", tr("cli.resume.done"), id);
    else {
      fprintf(stderr, "%s %u\n", tr("cli.resume.failed"), id);
      ret = 1;
    }

  } else if (strcmp(cmd, "cancel") == 0 && argc >= 3) {
    uint32_t id = 0;
    if (!parse_id(argv[2], &id)) {
      fprintf(stderr, "%s %s\n", tr("cli.error.download_id"), argv[2]);
      ipc_client_disconnect(sock);
      return 1;
    }
    if (ipc_send_cancel(sock, id) == 0)
      printf("%s %u\n", tr("cli.cancel.done"), id);
    else {
      fprintf(stderr, "%s %u\n", tr("cli.cancel.failed"), id);
      ret = 1;
    }

  } else {
    LOG_WARN("Unknown command: %s", cmd);
    ret = 1;
  }

  ipc_client_disconnect(sock);
  return ret;
}
