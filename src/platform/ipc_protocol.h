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
  MSG_STATUS_EVENT_V2 = 33,
  MSG_SUBSCRIBE_V2 = 34,
  MSG_LIST_PAGE = 35,
  MSG_LIST_PAGE_WITH_SIZE = 36,
  MSG_GET_DETAILS_V2 = 37,
  MSG_HELLO = 41, // v1 header, empty request; uint16_t version response.
  MSG_ADD_DOWNLOAD_V2 = 42,
  MSG_BROWSER_CONFIRM_V2 = 43,
  MSG_REMOVE_DOWNLOAD = 44, // 34 is already MSG_SUBSCRIBE_V2.
  MSG_QUEUE_LIST = 45,
  MSG_QUEUE_CREATE = 46,
  MSG_QUEUE_UPDATE = 47,
  MSG_QUEUE_DELETE = 48,
  MSG_QUEUE_REORDER = 49,
  MSG_ADD_DOWNLOAD_V3 = 50, // v2 JSON plus auto_directory; v2 response
} MsgType;

#define IPC_PROTOCOL_VERSION 6

/* Keep the v1 frame header unchanged for legacy clients. Version negotiation
 * uses MSG_HELLO; future versioned payloads use distinct message types. */
typedef struct {
  uint32_t length;
  MsgType type;
} MsgHeader;

/* Payload of MSG_STATUS_EVENT_V2. Numeric fields are native-endian, as in v1.
 * This is a separate message so the existing v1 frame stays byte-compatible. */
typedef struct {
  uint32_t download_id;
  uint64_t bytes_received;
  uint64_t total_bytes; // 0 = unknown
  uint64_t speed_bps; // 0 = unknown until scheduler sampling is available
  uint64_t eta_seconds; // UINT64_MAX = unknown
  float progress; // 0..1, -1 = indeterminate
  char status[24];
  char error[256];
} IpcProgressV2;

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
