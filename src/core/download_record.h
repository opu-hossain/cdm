// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef CORE_DOWNLOAD_RECORD_H
#define CORE_DOWNLOAD_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t id;
  char url[2048];
  char dest_path[1024];
  char status[16];
  float progress;
} DownloadListRecord;

#ifdef __cplusplus
}
#endif

#endif /* CORE_DOWNLOAD_RECORD_H */
