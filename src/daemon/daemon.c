// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "daemon.h"

#include "../core/queue_manager.h"
#include "../core/scheduler.h"
#include "../persistence/db.h"
#include "../platform/bandwidth.h"
#include "../platform/ipc_socket.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../utils/notify.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                  */
/* ------------------------------------------------------------------ */

/**
 * Signal handler – stops the IPC server, closes the database, and
 * cleans up global state before exiting.
 */
static void cleanup_and_exit(int sig) {
  (void)sig;
  LOG_INFO("Received shutdown signal, cleaning up");
  ipc_server_stop();
  db_close();
  curl_global_cleanup();
  dm_notify_shutdown();
  log_close();
  exit(0);
}

/** Create a directory if it doesn't already exist. */
static void ensure_data_dir(const char *dir) { mkdir(dir, 0755); }

/**
 * Return the daemon's data directory (~/.local/share/downloadmgr),
 * creating it on first call.
 */
static const char *get_data_dir(void) {
  static char path[1024] = {0};
  if (path[0] == '\0') {
    const char *home = getenv("HOME");
    if (!home)
      home = "/tmp";
    snprintf(path, sizeof(path), "%s/.local/share/downloadmgr", home);
    ensure_data_dir(path);
  }
  return path;
}

/**
 * Daemonise the process (double‑fork, new session, redirect
 * stdin/stdout/stderr to /dev/null).
 */
static void daemonize(void) {
  pid_t pid = fork();
  if (pid < 0)
    exit(1);
  if (pid > 0)
    exit(0); /* parent exits */

  setsid();

  pid = fork();
  if (pid < 0)
    exit(1);
  if (pid > 0)
    exit(0); /* first child exits */

  int devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > 2)
      close(devnull);
  }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int run_daemon(void) {
  /* Daemonise if stdin is not a terminal (i.e. launched from a
     service manager or background shell). */
  if (!isatty(STDIN_FILENO))
    daemonize();

  /* Initialise logging – log file lives inside the data directory. */
  char log_path[1024];
  snprintf(log_path, sizeof(log_path), "%s/daemon.log", get_data_dir());

#ifdef DM_DEBUG
  LogLevel level = LOG_DEBUG;
#else
  LogLevel level = LOG_INFO;
#endif

  if (!log_init(log_path, level)) {
    /* If we're daemonised, stderr already redirects to /dev/null;
       this fprintf is a best‑effort warning. */
    fprintf(stderr, "Warning: could not open log file %s\n", log_path);
  }

  config_init(NULL);

  /* Guard against multiple instances. */
  if (ipc_server_is_running()) {
    LOG_ERROR("Daemon is already running");
    return 0;
  }

  /* Open (or create) the database. */
  char db_path[1024];
  snprintf(db_path, sizeof(db_path), "%s/downloads.db", get_data_dir());
  if (db_init(db_path) != 0) {
    LOG_ERROR("Failed to initialize database at %s", db_path);
    return 1;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  dm_notify_init();

  /* Restore any downloads that were in progress before a crash. */
  db_restore_queue();
  queue_manager_seed_next_id(db_get_max_id() + 1);

  if (ipc_server_start() != 0) {
    LOG_ERROR("Failed to start IPC server");
    curl_global_cleanup();
    db_close();
    return 1;
  }

  signal(SIGINT, cleanup_and_exit);
  signal(SIGTERM, cleanup_and_exit);
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  LOG_INFO("Daemon started, listening on socket, logging to %s", log_path);

  /* Main event loop (tick ≈ 200 ms). */
  int iteration = 0;
  for (;;) {
    if (iteration % 25 == 0) /* roughly every 5 seconds */
      LOG_DEBUG("Daemon heartbeat: iter %d", iteration);
    iteration++;

    scheduler_tick();
    scheduler_report_progress();
    bandwidth_tick();
    ipc_server_poll();
    dm_thread_sleep_ms(200);
  }

  /* Unreachable – cleanup_and_exit() calls exit(0). */
  return 0;
}
