// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "platform/ipc_socket.h"
#include "platform/spawn.h"
#include "platform/daemon_autostart.h"
#include "platform/thread.h"
#include "gui/browser_popup.h"
#include "native_host/browser_install.h"
#include "utils/config.h"
#include "utils/log.h"

#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations for the three entry points */
int run_daemon(void);
int run_gui(void);
int run_cli(int argc, char **argv);

/**
 * Make sure the daemon is running.
 *
 * If it isn't, locate the current executable and spawn it as a detached
 * daemon.  Returns 0 if the daemon is (or becomes) ready, -1 on failure.
 */
static int ensure_daemon_running(void) {
  if (ipc_server_is_running())
    return 0;

  char exe_path[1024] = {0};
  get_self_exe_path(exe_path, sizeof(exe_path));
  if (exe_path[0] == '\0') {
    LOG_ERROR("Could not resolve executable path; cannot auto-start daemon");
    return -1;
  }

  LOG_INFO("Daemon not running, starting it...");
  if (spawn_daemon_detached(exe_path) != 0) {
    LOG_ERROR("Failed to start daemon");
    return -1;
  }

  /* Wait up to 5 seconds for the daemon socket to appear. */
  for (int i = 0; i < 50; i++) {
    if (ipc_server_is_running()) {
      LOG_INFO("Daemon is ready");
      return 0;
    }
    dm_thread_sleep_ms(100);
  }

  LOG_ERROR("Daemon did not become ready in time");
  return -1;
}

int main(int argc, char **argv) {
  /* Log to stderr by default (the daemon overrides this later). */
  log_init(NULL, LOG_INFO);

  /* Lifecycle commands do not need configuration; run_daemon loads it. */
  if (argc >= 2 && strcmp(argv[1], "daemon") == 0) {
    if (argc == 2)
      return run_daemon();
    if (argc == 3 && (strcmp(argv[2], "enable") == 0 ||
                      strcmp(argv[2], "disable") == 0 ||
                      strcmp(argv[2], "status") == 0))
      return daemon_autostart_command(argv[2]);
    fprintf(stderr, "Usage: cdm daemon [enable|disable|status]\n");
    return 2;
  }

  config_init(NULL);

  /* No arguments → ensure daemon is running, then launch GUI. */
  if (argc < 2) {
    if (ensure_daemon_running() != 0)
      return 1;
    return run_gui();
  }

  const char *mode = argv[1];

  if (strcmp(mode, "gui") == 0 || strcmp(mode, "ui") == 0) {
    if (ensure_daemon_running() != 0)
      return 1;
    return run_gui();
  }

  if (strcmp(mode, "cli") == 0) {
    if (ensure_daemon_running() != 0)
      return 1;
    return run_cli(argc - 1, argv + 1);
  }

  if (strcmp(mode, "browser-popup") == 0 && argc == 4 &&
      strcmp(argv[2], "--offer") == 0) {
    errno = 0;
    char *end = NULL;
    unsigned long id = strtoul(argv[3], &end, 10);
    if (errno || !end || *end || id == 0 || id > UINT32_MAX)
      return 1;
    return run_browser_popup((uint32_t)id);
  }

  if (strcmp(mode, "browser") == 0)
    return browser_install_main(argc - 1, argv + 1);

  /* User-friendly usage (not a log message). */
  fprintf(stderr, "Usage: %s [daemon [enable|disable|status]|gui|cli]\n", argv[0]);
  fprintf(stderr, "  daemon  — Run in background or manage login autostart\n");
  fprintf(stderr, "  gui     — Open the graphical interface (default)\n");
  fprintf(stderr, "  cli     — Command-line interface\n");
  return 1;
}
