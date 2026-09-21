// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "spawn.h"

#include "../utils/log.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>

void get_self_exe_path(char *buf, size_t buf_size) {
  DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)buf_size);
  if (len == 0 || len == buf_size)
    buf[0] = '\0';
}

int spawn_daemon_detached(const char *exe_path) {
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  memset(&pi, 0, sizeof(pi));

  char cmdline[1024];
  snprintf(cmdline, sizeof(cmdline), "\"%s\" daemon", exe_path);

  BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                           DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL,
                           NULL, &si, &pi);
  if (!ok) {
    fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
    return -1;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return 0;
}

#else /* Linux / macOS */

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
void get_self_exe_path(char *buf, size_t buf_size) {
  uint32_t size = (uint32_t)buf_size;
  if (_NSGetExecutablePath(buf, &size) != 0)
    buf[0] = '\0';
}
#else
void get_self_exe_path(char *buf, size_t buf_size) {
  ssize_t len = readlink("/proc/self/exe", buf, buf_size - 1);
  if (len == -1)
    buf[0] = '\0';
  else
    buf[len] = '\0';
}
#endif

int spawn_daemon_detached(const char *exe_path) {
  pid_t pid = fork();
  if (pid < 0) {
    LOG_ERROR("first fork failed for '%s'", exe_path);
    return -1;
  }

  if (pid == 0) {
    setsid();
    pid_t pid2 = fork();
    if (pid2 == 0) {
      execl(exe_path, exe_path, "daemon", (char *)NULL);
      LOG_ERROR("execl failed for '%s'", exe_path);
      _exit(127);
    }
    _exit(0);
  }
  waitpid(pid, NULL, 0);
  LOG_INFO("launched '%s' as daemon", exe_path);
  return 0;
}

#endif
