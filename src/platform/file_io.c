// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "file_io.h"
#include <stdint.h>

/* ================================================================== */
/*  POSIX implementation                                              */
/* ================================================================== */
#ifndef _WIN32

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int file_preallocate(const char *path, uint64_t total_size) {
  /* Create the file exclusively; fail if it already exists or is a symlink. */
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
  if (fd < 0)
    return -1;

  if (total_size == 0) {
    close(fd);
    return 0;
  }

  /* Extend to total_size - 1, then write a zero byte to set the length. */
  if (lseek(fd, (off_t)(total_size - 1), SEEK_SET) == (off_t)-1) {
    close(fd);
    return -1;
  }
  char zero = '\0';
  if (write(fd, &zero, 1) != 1) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

FileHandle file_open_rw(const char *path) {
  int fd = open(path, O_WRONLY | O_NOFOLLOW, 0644);
  if (fd < 0)
    return INVALID_FILE_HANDLE;

  /* Ensure it’s a regular file (not a symlink or device). */
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return INVALID_FILE_HANDLE;
  }
  return fd;
}

int file_pwrite(FileHandle fd, const void *buf, size_t n, uint64_t offset) {
  ssize_t written = pwrite(fd, buf, n, (off_t)offset);
  if (written < 0)
    return -1;
  return ((size_t)written == n) ? 0 : -1;
}

uint64_t file_get_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return (uint64_t)st.st_size;
}

void file_close(FileHandle fd) { close(fd); }

/* ================================================================== */
/*  Windows implementation                                            */
/* ================================================================== */
#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

FileHandle file_open_rw(const char *path) {
  HANDLE h =
      CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  return (FileHandle)h;
}

int file_pwrite(FileHandle fd, const void *buf, size_t n, uint64_t offset) {
  OVERLAPPED ov = {0};
  ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
  ov.OffsetHigh = (DWORD)(offset >> 32);

  DWORD bytesWritten = 0;
  BOOL result = WriteFile((HANDLE)fd, buf, (DWORD)n, &bytesWritten, &ov);
  if (!result)
    return -1;
  return (bytesWritten == (DWORD)n) ? 0 : -1;
}

int file_preallocate(const char *path, uint64_t total_size) {
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return -1;

  if (total_size == 0) {
    CloseHandle(h);
    return 0;
  }

  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)total_size;
  if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
    CloseHandle(h);
    return -1;
  }
  if (!SetEndOfFile(h)) {
    CloseHandle(h);
    return -1;
  }
  CloseHandle(h);
  return 0;
}

uint64_t file_get_size(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
    return 0;

  LARGE_INTEGER size;
  size.HighPart = fad.nFileSizeHigh;
  size.LowPart = fad.nFileSizeLow;
  return (uint64_t)size.QuadPart;
}

void file_close(FileHandle fd) {
  if ((HANDLE)fd != INVALID_HANDLE_VALUE && fd != NULL)
    CloseHandle((HANDLE)fd);
}

#endif /* _WIN32 */
