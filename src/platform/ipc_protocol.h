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
  MSG_REFRESH_URL = 40, // uint32 download ID -> uint8 IpcResult
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
  MSG_CATEGORY_LIST_V1 = 51,
  MSG_CATEGORY_CREATE_V1 = 52,
  MSG_CATEGORY_UPDATE_V1 = 53,
  MSG_CATEGORY_DELETE_V1 = 54,
  MSG_LIST_PAGE_WITH_CATEGORY_V1 = 55,
  MSG_BROWSER_OFFER_V2 = 56, // JSON request context; legacy raw offer reply
  MSG_BROWSER_CONTEXT_INFO_V1 = 57, // uint32 offer ID -> uint32 presence bits
  MSG_BROWSER_KIND_INFO_V1 = 58, // uint32 offer ID -> uint32 IpcBrowserMediaKind
  MSG_BROWSER_SITE_CAPABILITY_V1 = 59, // uint32 offer ID -> uint32 eligible
  MSG_BROWSER_CONFIRM_SITE_V1 = 60, // v2 confirm payload/reply; opt-in site tool
} MsgType;

#define IPC_PROTOCOL_VERSION 11
#define IPC_BROWSER_HAS_COOKIE 1u
#define IPC_BROWSER_HAS_USER_AGENT 2u
#define IPC_BROWSER_HAS_REFERER 4u

/* Versioned category record; fixed native ABI, with bounded NUL strings. */
typedef struct {
  uint32_t id;
  char name[128];
  char extensions[512];
  char default_dir[1024];
  int64_t created_at; // Unix seconds
} IpcCategoryV1;

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
#define IPC_CATEGORY_MAX 256

/* Offer metadata only; the legacy IpcBrowserOffer wire layout is unchanged. */
typedef enum {
  IPC_BROWSER_MEDIA_NONE = 0,
  IPC_BROWSER_MEDIA_HLS = 1,
  IPC_BROWSER_MEDIA_DASH = 2,
  IPC_BROWSER_MEDIA_VIDEO = 3
} IpcBrowserMediaKind;

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_IPC_PROTOCOL_H */
