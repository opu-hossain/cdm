// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef UTILS_LOG_H
#define UTILS_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LOG_DEBUG = 0,
  LOG_INFO,
  LOG_WARN,
  LOG_ERROR,
} LogLevel;

/**
 * Initialise the logging subsystem.
 *
 * @param log_path   File to write to, or NULL for stderr only.
 * @param min_level  Messages below this level are suppressed.
 * @return true on success, false if the log file could not be opened.
 */
bool log_init(const char *log_path, LogLevel min_level);

/** Flush and close the log file. */
void log_close(void);

/**
 * Write a log message (prefer the convenience macros below).
 *
 * The macros capture __FILE__ and __LINE__ automatically and avoid
 * evaluating arguments when the level is filtered out.
 */
void log_write(LogLevel level, const char *file, const char *function,
               int line, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

#define LOG_DEBUG(...)                                                         \
  log_write(LOG_DEBUG, __FILE__, __func__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)                                                          \
  log_write(LOG_INFO, __FILE__, __func__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)                                                          \
  log_write(LOG_WARN, __FILE__, __func__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...)                                                         \
  log_write(LOG_ERROR, __FILE__, __func__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* UTILS_LOG_H */
