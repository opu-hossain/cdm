// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "daemon.h"

#include "../core/queue_manager.h"
#include "../core/scheduler.h"
#include "../persistence/db.h"
#include "../platform/bandwidth.h"
#include "../platform/file_io.h"
#include "../platform/ipc_socket.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../utils/notify.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Internal helpers */

static volatile sig_atomic_t g_shutdown_requested = 0;
static bool g_data_dir_migrated = false;

static void request_shutdown(int sig) {
  (void)sig;
  g_shutdown_requested = 1;
}

/** Create a directory if it doesn't already exist. */
static void ensure_data_dir(const char *dir) { file_ensure_directory(dir); }

/**
 * Return the daemon's data directory (~/.local/share/cdm),
 * creating it on first call.
 */
static const char *get_data_dir(void) {
  static char path[1024] = {0};
  if (path[0] == '\0') {
    const char *home = getenv("HOME");
    if (!home)
      home = "/tmp";
    char old_path[1024];
    snprintf(path, sizeof(path), "%s/.local/share/cdm", home);
    snprintf(old_path, sizeof(old_path), "%s/.local/share/downloadmgr", home);
    if (access(old_path, F_OK) == 0 && access(path, F_OK) != 0) {
      char parent[1024];
      snprintf(parent, sizeof(parent), "%s/.local/share", home);
      if (file_ensure_directory(parent) == 0 && rename(old_path, path) == 0)
        g_data_dir_migrated = true;
    }
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

/* Public API */

int run_daemon(void) {
  int lock_fd = -1;
  int lock_result = ipc_daemon_lock_acquire(&lock_fd);
  if (lock_result == 1) {
    pid_t existing_pid = 0;
    int probe = ipc_server_get_pid(&existing_pid);
    if (probe == 1)
      fprintf(stderr, "cdm daemon: already running (pid %ld)\n",
              (long)existing_pid);
    else if (probe == 0)
      fprintf(stderr, "cdm daemon: already starting\n");
    else
      fprintf(stderr, "cdm daemon: another daemon holds the lock; "
                      "could not determine its PID: %s\n", strerror(errno));
    return 1;
  }
  if (lock_result != 0) {
    fprintf(stderr, "cdm daemon: could not acquire daemon lock\n");
    return 1;
  }

  pid_t existing_pid = 0;
  int probe = ipc_server_get_pid(&existing_pid);
  if (probe == 1) {
    fprintf(stderr, "cdm daemon: already running (pid %ld)\n",
            (long)existing_pid);
    close(lock_fd);
    return 1;
  }
  if (probe < 0) {
    fprintf(stderr, "cdm daemon: could not inspect IPC listener: %s\n",
            strerror(errno));
    close(lock_fd);
    return 1;
  }

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
  if (g_data_dir_migrated)
    LOG_INFO("Migrated data directory from ~/.local/share/downloadmgr to "
             "~/.local/share/cdm");

  config_init(NULL);

  /* Guard against multiple instances. */
  if (ipc_server_is_running()) {
    LOG_ERROR("Daemon is already running");
    close(lock_fd);
    return 1;
  }

  /* Open (or create) the database. */
  char db_path[1024];
  snprintf(db_path, sizeof(db_path), "%s/downloads.db", get_data_dir());
  if (db_init(db_path) != 0) {
    LOG_ERROR("Failed to initialize database at %s", db_path);
    close(lock_fd);
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
    close(lock_fd);
    return 1;
  }

  signal(SIGINT, request_shutdown);
  signal(SIGTERM, request_shutdown);
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  LOG_INFO("Daemon started, listening on socket, logging to %s", log_path);

  /* Main event loop (tick ≈ 200 ms). */
  int iteration = 0;
  while (!g_shutdown_requested) {
    if (iteration % 25 == 0) /* roughly every 5 seconds */
      LOG_DEBUG("Daemon heartbeat: iter %d", iteration);
    iteration++;

    scheduler_tick();
    scheduler_report_progress();
    bandwidth_tick();
    ipc_server_poll();
    dm_thread_sleep_ms(200);
  }

  LOG_INFO("Received shutdown signal, cleaning up");
  ipc_server_stop();
  scheduler_shutdown();
  db_close();
  curl_global_cleanup();
  dm_notify_shutdown();
  log_close();
  close(lock_fd);
  return 0;
}
