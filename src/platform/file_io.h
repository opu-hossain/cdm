// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_FILE_IO_H
#define PLATFORM_FILE_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#include <windows.h>
typedef HANDLE FileHandle;
#define INVALID_FILE_HANDLE INVALID_HANDLE_VALUE
#else
typedef int FileHandle;
#define INVALID_FILE_HANDLE (-1)
#endif

/**
 * Open an existing file for writing.
 *
 * The file must have been created previously (e.g. by file_preallocate).
 */
FileHandle file_open_rw(const char *path);

/**
 * Write `n` bytes from `buf` at the exact byte `offset`.
 *
 * Atomic on both POSIX (pwrite) and Windows (WriteFile + OVERLAPPED).
 *
 * @return 0 on success, -1 on partial write or error.
 */
int file_pwrite(FileHandle fd, const void *buf, size_t n, uint64_t offset);

/**
 * Create a new file and pre‑allocate it to `total_size` bytes.
 *
 * Creates a sparse file on supporting filesystems. Fails if the path
 * already exists (O_EXCL / CREATE_NEW semantics).
 *
 * @return 0 on success, -1 on failure.
 */
int file_preallocate(const char *path, uint64_t total_size);

/**
 * Get the current size of a file.
 *
 * @return Size in bytes, or 0 if the file cannot be stat’ed.
 */
uint64_t file_get_size(const char *path);

/** Close a file handle. */
void file_close(FileHandle fd);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_FILE_IO_H */
