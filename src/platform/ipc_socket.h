// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_IPC_SOCKET_H
#define PLATFORM_IPC_SOCKET_H

#include "../core/download_record.h"
#include "ipc_protocol.h"
#include <stdbool.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_LIST_ALL_MAX 200

/* Data structures for list‑all responses */
typedef DownloadListRecord IpcDownloadRecord;

typedef struct {
  char cookie[1024];
  char referrer[2048];
  char extra_headers[4096];
  char expected_sha256[65];
  uint64_t speed_limit_bps;
} IpcDownloadDetails;

/* Options sent with an MSG_ADD_DOWNLOAD request */
typedef struct {
  const char *cookie;
  const char *referrer;
  const char *extra_headers;
  const char *expected_sha256;
  uint64_t speed_limit_bps;
} IpcDownloadOptions;

/* Server operations. */
bool ipc_server_is_running(void);
int ipc_server_start(void);
void ipc_server_poll(void);
void ipc_server_stop(void);

/* Client operations. */
int ipc_client_connect(void);
void ipc_client_disconnect(int sock);
int ipc_client_connect_timeout(int timeout_ms);

/* High-level requests and responses. */
uint32_t ipc_send_add_download(int sock, const char *url, const char *dest_path,
                               const IpcDownloadOptions *options);
int ipc_send_pause(int sock, uint32_t id);
int ipc_send_resume(int sock, uint32_t id);
int ipc_send_cancel(int sock, uint32_t id);
int ipc_send_list_all(int sock, IpcDownloadRecord *out, int max);
int ipc_send_get_details(int sock, uint32_t id, IpcDownloadDetails *out);
int ipc_send_reload_config(int sock);
void ipc_send_subscribe(int sock);

/* Low-level I/O for external event listeners. */
int ipc_read_exact(int sock, void *buf, size_t len);
int ipc_write_exact(int sock, const void *buf, size_t len);

/* Daemon-to-client status updates. */
void ipc_broadcast_status(uint32_t download_id, const char *status,
                          float progress);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_IPC_SOCKET_H */
