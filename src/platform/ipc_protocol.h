// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef PLATFORM_IPC_PROTOCOL_H
#define PLATFORM_IPC_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Message types exchanged between CLI / GUI clients and the daemon. */
typedef enum {
  MSG_ADD_DOWNLOAD = 1,
  MSG_PAUSE,
  MSG_RESUME,
  MSG_CANCEL,
  MSG_LIST,
  MSG_STATUS_EVENT,
  MSG_PROGRESS_EVENT,
  MSG_SUBSCRIBE,
  MSG_LIST_ALL,
  MSG_GET_DETAILS,
  MSG_RELOAD_CONFIG,
  MSG_BROWSER_OFFER,
  MSG_BROWSER_GET_OFFER,
  MSG_BROWSER_CONFIRM,
  MSG_BROWSER_DISMISS,
  MSG_BROWSER_SUBSCRIBE_PROGRESS,
  MSG_BROWSER_PROGRESS_EVENT,
  /* Same payload as MSG_ADD_DOWNLOAD; daemon may resolve the provisional
   * URL-derived destination after probing response headers. */
  MSG_ADD_DOWNLOAD_AUTO = 32,
  MSG_HELLO = 41, // v1 header, empty request; uint16_t version response.
} MsgType;

#define IPC_PROTOCOL_VERSION 2

/* Keep the v1 frame header unchanged for legacy clients. Version negotiation
 * uses MSG_HELLO; future versioned payloads use distinct message types. */
typedef struct {
  uint32_t length;
  MsgType type;
} MsgHeader;

typedef enum {
  IPC_RESULT_OK = 0,
  IPC_RESULT_NOT_FOUND = 1,
  IPC_RESULT_REJECTED = 2,
  IPC_RESULT_ERROR = 3,
} IpcResult;

#define IPC_MAX_URL_LEN 2048
#define IPC_MAX_PATH_LEN 1024
#define IPC_MAX_FRAME_SIZE 16384

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_IPC_PROTOCOL_H */
