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
  uint64_t total_size; // internal row field; only type 36 serializes it
} DownloadListRecord;

/* Daemon-owned transfer sample; queue mutex protects every read/write. */
typedef struct {
  uint64_t sampled_bytes;
  uint64_t sampled_at_ms; // CLOCK_MONOTONIC milliseconds; 0 = no sample
  uint64_t speed_bps; // exponential moving average, alpha = 0.3
  uint64_t eta_seconds; // UINT64_MAX = unknown
} DownloadTransferMetrics;

typedef struct {
  uint32_t id;
  char name[128];
  int priority;
  int max_concurrent; // 0 = no per-queue limit
  char schedule_start[6]; // HH:MM or empty
  char schedule_stop[6];
  char post_action[16];
  char post_action_arg[512];
  int64_t created_at; // Unix seconds
} Queue;

#ifdef __cplusplus
}
#endif

#endif /* CORE_DOWNLOAD_RECORD_H */
