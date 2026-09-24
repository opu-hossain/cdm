// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "open_path.h"

#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

int platform_open_path(const char *path) {
  if (!path || !path[0])
    return -1;
  HINSTANCE result = ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
  return (INT_PTR)result > 32 ? 0 : -1;
}

#else

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

int platform_open_path(const char *path) {
  if (!path || !path[0] || access(path, F_OK) != 0)
    return -1;
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    if (setsid() < 0)
      _exit(127);
    pid_t child = fork();
    if (child < 0)
      _exit(127);
    if (child == 0) {
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
          close(devnull);
      }
#ifdef __APPLE__
      execlp("open", "open", path, (char *)NULL);
#else
      execlp("xdg-open", "xdg-open", path, (char *)NULL);
#endif
      _exit(127);
    }
    _exit(0);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0)
    return -1;
  return 0;
}

#endif
