// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_SPAWN_H
#define PLATFORM_SPAWN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write the absolute path of the currently running executable into `buf`.
 *
 * The buffer must be at least `buf_size` bytes. On failure `buf` is set
 * to an empty string.
 */
void get_self_exe_path(char *buf, size_t buf_size);

/**
 * Launch a fully detached daemon process.
 *
 * Equivalent to running `<exe_path> daemon`. Returns 0 on success.
 */
int spawn_daemon_detached(const char *exe_path);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_SPAWN_H */
