// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "thread.h"

#ifdef _WIN32

#include <stdlib.h>
#include <windows.h>

unsigned long long dm_current_process_id(void) {
  return (unsigned long long)GetCurrentProcessId();
}

unsigned long long dm_current_thread_id(void) {
  return (unsigned long long)GetCurrentThreadId();
}

int dm_thread_create(dm_thread_t *thread, int (*fn)(void *), void *arg) {
  thread->handle =
      CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
  return thread->handle ? 0 : -1;
}

int dm_thread_join(dm_thread_t *thread, int *result) {
  DWORD rc = WaitForSingleObject(thread->handle, INFINITE);
  if (rc != WAIT_OBJECT_0)
    return -1;

  if (result) {
    DWORD exit_code;
    GetExitCodeThread(thread->handle, &exit_code);
    *result = (int)exit_code;
  }
  CloseHandle(thread->handle);
  return 0;
}

int dm_thread_detach(dm_thread_t *thread) {
  CloseHandle(thread->handle);
  return 0;
}

void dm_thread_sleep_ms(unsigned int ms) { Sleep((DWORD)ms); }

int dm_mutex_init(dm_mutex_t *mutex) {
  InitializeCriticalSection(&mutex->cs);
  return 0;
}
int dm_mutex_lock(dm_mutex_t *mutex) {
  EnterCriticalSection(&mutex->cs);
  return 0;
}
int dm_mutex_unlock(dm_mutex_t *mutex) {
  LeaveCriticalSection(&mutex->cs);
  return 0;
}
int dm_mutex_destroy(dm_mutex_t *mutex) {
  DeleteCriticalSection(&mutex->cs);
  return 0;
}

#else /* POSIX */

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

unsigned long long dm_current_process_id(void) {
  return (unsigned long long)getpid();
}

unsigned long long dm_current_thread_id(void) {
  return (unsigned long long)(uintptr_t)pthread_self();
}

int dm_thread_create(dm_thread_t *thread, int (*fn)(void *), void *arg) {
  return pthread_create(&thread->handle, NULL, (void *(*)(void *))fn, arg) == 0
             ? 0
             : -1;
}

int dm_thread_join(dm_thread_t *thread, int *result) {
  void *ret = NULL;
  int rc = pthread_join(thread->handle, &ret);
  if (result)
    *result = (int)(intptr_t)ret;
  return rc == 0 ? 0 : -1;
}

void dm_thread_sleep_ms(unsigned int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

int dm_thread_detach(dm_thread_t *thread) {
  return pthread_detach(thread->handle) == 0 ? 0 : -1;
}

int dm_mutex_init(dm_mutex_t *mutex) {
  return pthread_mutex_init(&mutex->handle, NULL) == 0 ? 0 : -1;
}
int dm_mutex_lock(dm_mutex_t *mutex) {
  return pthread_mutex_lock(&mutex->handle) == 0 ? 0 : -1;
}
int dm_mutex_unlock(dm_mutex_t *mutex) {
  return pthread_mutex_unlock(&mutex->handle) == 0 ? 0 : -1;
}
void dm_mutex_destroy(dm_mutex_t *mutex) {
  pthread_mutex_destroy(&mutex->handle);
}

#endif
