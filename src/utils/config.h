// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef UTILS_CONFIG_H
#define UTILS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load configuration from a TOML file.
 *
 * If `path` is NULL the default location is used
 * ($HOME/.local/share/downloadmgr/config.toml on Unix,
 * %USERPROFILE%/.local/share/downloadmgr/config.toml on Windows).
 * Missing file or parse errors are silently absorbed – all values fall
 * back to built‑in defaults.
 */
void config_init(const char *path);

int config_get_max_concurrent_downloads(void);     // default 3
const char *config_get_default_download_dir(void); // default $HOME/Downloads
int config_get_retry_max_attempts(void);           // default 5
int config_get_retry_base_delay_sec(void);         // default 2
int config_get_retry_max_delay_sec(void);          // default 60
uint64_t config_get_max_speed_bytes_per_sec(void); // default 0 (unlimited)

typedef struct {
  int max_concurrent_downloads;
  const char *default_download_dir;
  int retry_max_attempts;
  int retry_base_delay_sec;
  int retry_max_delay_sec;
  uint64_t max_speed_bytes_per_sec;
} DownloadManagerConfig;

void config_get(DownloadManagerConfig *out);
bool config_save(const DownloadManagerConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* UTILS_CONFIG_H */
