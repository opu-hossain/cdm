// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_THREAD_H
#define PLATFORM_THREAD_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque thread and mutex wrappers */
#ifdef _WIN32
typedef struct dm_thread_t {
  HANDLE handle;
} dm_thread_t;
typedef struct dm_mutex_t {
  CRITICAL_SECTION cs;
} dm_mutex_t;
#else
typedef struct dm_thread_t {
  pthread_t handle;
} dm_thread_t;
typedef struct dm_mutex_t {
  pthread_mutex_t handle;
} dm_mutex_t;
#endif

/**
 * Create a new thread running `fn(arg)`.
 * @return 0 on success, -1 on error.
 */
int dm_thread_create(dm_thread_t *thread, int (*fn)(void *), void *arg);

/**
 * Wait for a thread to finish and retrieve its return value.
 * @param result output for the thread's return code (may be NULL).
 * @return 0 on success, -1 on error.
 */
int dm_thread_join(dm_thread_t *thread, int *result);

/**
 * Detach a thread so its resources are automatically freed on exit.
 * @return 0 on success, -1 on error.
 */
int dm_thread_detach(dm_thread_t *thread);

/** Sleep the calling thread for `ms` milliseconds. */
void dm_thread_sleep_ms(unsigned int ms);

/** Return the current process id. */
unsigned long long dm_current_process_id(void);

/** Return the current thread id in a printable form. */
unsigned long long dm_current_thread_id(void);

/** Initialize a mutex. Returns 0 on success, -1 on error. */
int dm_mutex_init(dm_mutex_t *mutex);
int dm_mutex_lock(dm_mutex_t *mutex);
int dm_mutex_unlock(dm_mutex_t *mutex);
void dm_mutex_destroy(dm_mutex_t *mutex);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_THREAD_H */
