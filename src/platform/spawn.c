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

int spawn_browser_popup_detached(const char *exe_path, unsigned int offer_id) {
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  memset(&pi, 0, sizeof(pi));
  char cmdline[1200];
  snprintf(cmdline, sizeof(cmdline), "\"%s\" browser-popup --offer %u",
           exe_path, offer_id);
  BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                           DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                           NULL, NULL, &si, &pi);
  if (!ok)
    return -1;
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return 0;
}

#else /* Linux / macOS */

#include <fcntl.h>
#include <errno.h>
#include <poll.h>
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

int spawn_browser_popup_detached(const char *exe_path, unsigned int offer_id) {
  int ready_pipe[2];
  if (pipe(ready_pipe) != 0)
    return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    return -1;
  }
  if (pid == 0) {
    close(ready_pipe[0]);
    if (setsid() < 0)
      _exit(127);
    pid_t child = fork();
    if (child < 0)
      _exit(127);
    if (child == 0) {
      char ready_fd[16];
      snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
      setenv("CDM_BROWSER_READY_FD", ready_fd, 1);
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
          close(devnull);
      }
      char id[16];
      snprintf(id, sizeof(id), "%u", offer_id);
      execl(exe_path, exe_path, "browser-popup", "--offer", id,
            (char *)NULL);
      _exit(127);
    }
    close(ready_pipe[1]);
    _exit(0);
  }
  close(ready_pipe[1]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    close(ready_pipe[0]);
    return -1;
  }
  struct pollfd pollfd = {.fd = ready_pipe[0], .events = POLLIN};
  int ready;
  do {
    ready = poll(&pollfd, 1, 5000);
  } while (ready < 0 && errno == EINTR);
  char marker = 0;
  if (ready > 0 && (pollfd.revents & POLLIN))
    ready = (int)read(ready_pipe[0], &marker, 1);
  close(ready_pipe[0]);
  return ready == 1 && marker == 'R' ? 0 : -1;
}

#endif
