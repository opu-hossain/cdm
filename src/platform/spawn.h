// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_SPAWN_H
#define PLATFORM_SPAWN_H

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

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

/** Launch a separate cdm SDL/Nuklear popup for a browser offer. */
int spawn_browser_popup_detached(const char *exe_path, unsigned int offer_id);

/* Cache the ffmpeg executable path once, before daemon workers start. */
void spawn_media_tools_init(void);
bool spawn_ffmpeg_available(void);
/* Fixed argv, local input/output only; child is reaped on every path.
 * 0 success; -2 interrupted; -1 spawn failure; positive exit/timeout status. */
int spawn_ffmpeg_remux(const char *input, const char *output,
                       const _Atomic bool *cancel, const _Atomic bool *pause,
                       int timeout_sec);

int spawn_ffmpeg_merge(const char *video, const char *audio, const char *output,
                       const _Atomic bool *cancel, const _Atomic bool *pause, int timeout_sec);

/* No shell. scanner_args supports quotes/backslashes; file_path is last argv.
 * 0 clean, positive child exit (124 timeout), -1 launch/parse error,
 * -2 cancel or pause. */
int spawn_scanner(const char *command, const char *scanner_args,
                  const char *file_path, const _Atomic bool *cancel,
                  const _Atomic bool *pause, int timeout_sec);

/* Launch an explicitly enabled queue action without an implicit shell. */
int spawn_post_action(const char *action, const char *argument);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_SPAWN_H */
