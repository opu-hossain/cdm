// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_IPC_SOCKET_H
#define PLATFORM_IPC_SOCKET_H

#include "../core/download_record.h"
#include "ipc_protocol.h"
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_LIST_ALL_MAX 200
#define IPC_BROWSER_REQUEST_ID_MAX 128
#define IPC_BROWSER_FILENAME_MAX 512
#define IPC_BROWSER_MIME_MAX 128

typedef enum {
  IPC_BROWSER_WAITING = 0,
  IPC_BROWSER_CONFIRMED = 1,
  IPC_BROWSER_DISMISSED = 2,
} IpcBrowserOfferState;

typedef struct {
  uint32_t offer_id;
  uint32_t download_id;
  uint64_t total_bytes;
  IpcBrowserOfferState state;
  char request_id[IPC_BROWSER_REQUEST_ID_MAX];
  char url[IPC_MAX_URL_LEN];
  char filename[IPC_BROWSER_FILENAME_MAX];
  char mime[IPC_BROWSER_MIME_MAX];
  char referrer[IPC_MAX_URL_LEN];
} IpcBrowserOffer;

typedef struct {
  uint32_t download_id;
  uint64_t bytes_received;
  uint64_t total_bytes;
  float progress;
  char status[24];
  char error[256];
  char dest_path[IPC_MAX_PATH_LEN];
} IpcBrowserProgress;

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
/* 0: acquired; 1: another daemon holds it; -1: error. Caller closes fd. */
int ipc_daemon_lock_acquire(int *fd_out);
bool ipc_server_is_running(void);
/* 1: running with PID, 0: no listener, -1: probe error (errno set). */
int ipc_server_get_pid(pid_t *pid_out);
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
uint32_t ipc_send_add_download_auto(int sock, const char *url,
                                    const char *dest_path,
                                    const IpcDownloadOptions *options);
int ipc_send_pause(int sock, uint32_t id);
int ipc_send_resume(int sock, uint32_t id);
int ipc_send_cancel(int sock, uint32_t id);
int ipc_send_list_all(int sock, IpcDownloadRecord *out, int max);
int ipc_send_get_details(int sock, uint32_t id, IpcDownloadDetails *out);
int ipc_send_reload_config(int sock);
void ipc_send_subscribe(int sock);
int ipc_browser_offer(int sock, const IpcBrowserOffer *offer,
                      IpcBrowserOffer *out);
int ipc_browser_get_offer(int sock, uint32_t offer_id, IpcBrowserOffer *out);
int ipc_browser_confirm(int sock, uint32_t offer_id, const char *dest_path,
                        uint32_t *download_id);
int ipc_browser_dismiss(int sock, uint32_t offer_id);
int ipc_browser_subscribe_progress(int sock, uint32_t download_id,
                                   IpcBrowserProgress *initial);

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
