// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "log.h"
#include "../platform/thread.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */
#define LOG_MAX_BYTES (5 * 1024 * 1024) /* rotate once exceeded */

/* ------------------------------------------------------------------ */
/*  Global state                                                      */
/* ------------------------------------------------------------------ */
static FILE *g_log_fp = NULL;
static char g_log_path[1024] = {0};
static LogLevel g_min_level = LOG_INFO;
static dm_mutex_t g_log_mutex;
static bool g_mutex_ready = false;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static const char *level_name(LogLevel level) {
  switch (level) {
  case LOG_DEBUG:
    return "DEBUG";
  case LOG_INFO:
    return "INFO";
  case LOG_WARN:
    return "WARN";
  case LOG_ERROR:
    return "ERROR";
  default:
    return "?";
  }
}

/**
 * Rotate the log file if it exceeds LOG_MAX_BYTES, keeping one previous
 * copy (log.txt -> log.txt.1).
 *
 * Must be called with g_log_mutex held.
 */
static void rotate_if_needed(void) {
  if (!g_log_fp || g_log_path[0] == '\0')
    return;

  long size = ftell(g_log_fp);
  if (size < LOG_MAX_BYTES)
    return;

  fclose(g_log_fp);

  char backup_path[1040];
  snprintf(backup_path, sizeof(backup_path), "%s.1", g_log_path);
  remove(backup_path);
  rename(g_log_path, backup_path);

  g_log_fp = fopen(g_log_path, "a");
  if (g_log_fp)
    setvbuf(g_log_fp, NULL, _IOLBF, 0);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

bool log_init(const char *log_path, LogLevel min_level) {
  if (!g_mutex_ready) {
    dm_mutex_init(&g_log_mutex);
    g_mutex_ready = true;
  }

  dm_mutex_lock(&g_log_mutex);

  g_min_level = min_level;

  if (g_log_fp) {
    fclose(g_log_fp);
    g_log_fp = NULL;
  }

  if (log_path) {
    strncpy(g_log_path, log_path, sizeof(g_log_path) - 1);
    g_log_fp = fopen(log_path, "a");
    if (!g_log_fp) {
      dm_mutex_unlock(&g_log_mutex);
      return false;
    }
    setvbuf(g_log_fp, NULL, _IOLBF, 0); /* line-buffered */
  } else {
    g_log_path[0] = '\0';
  }

  dm_mutex_unlock(&g_log_mutex);
  return true;
}

void log_close(void) {
  if (!g_mutex_ready)
    return;
  dm_mutex_lock(&g_log_mutex);
  if (g_log_fp) {
    fclose(g_log_fp);
    g_log_fp = NULL;
  }
  dm_mutex_unlock(&g_log_mutex);
}

void log_write(LogLevel level, const char *file, int line, const char *fmt,
               ...) {
  if (level < g_min_level)
    return;

  if (!g_mutex_ready) {
    dm_mutex_init(&g_log_mutex);
    g_mutex_ready = true;
  }

  /* Keep only the filename for readability. */
  const char *base = strrchr(file, '/');
  base = base ? base + 1 : file;

  time_t now = time(NULL);
  struct tm tm_buf;
  localtime_r(&now, &tm_buf);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_buf);

  dm_mutex_lock(&g_log_mutex);

  FILE *target = g_log_fp ? g_log_fp : stderr;

  fprintf(target, "%s [%s] %s:%d: ", timestamp, level_name(level), base, line);
  va_list args;
  va_start(args, fmt);
  vfprintf(target, fmt, args);
  va_end(args);
  fprintf(target, "\n");

  if (g_log_fp) {
    fflush(g_log_fp);
    rotate_if_needed();
  }

  dm_mutex_unlock(&g_log_mutex);
}
