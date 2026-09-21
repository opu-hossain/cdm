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
} MsgType;

/* Every message on the wire starts with this header. */
typedef struct {
  uint32_t length;
  MsgType type;
} MsgHeader;

#define IPC_MAX_URL_LEN 2048
#define IPC_MAX_PATH_LEN 1024
#define IPC_MAX_FRAME_SIZE 16384

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_IPC_PROTOCOL_H */
