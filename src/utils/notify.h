// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef UTILS_NOTIFY_H
#define UTILS_NOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  DM_NOTIFY_INFO,  /* completion */
  DM_NOTIFY_ERROR, /* failure */
} DmNotifyUrgency;

/** One‑time initialisation (call at daemon start). */
void dm_notify_init(void);

/** Cleanup (call at daemon shutdown). */
void dm_notify_shutdown(void);

/**
 * Send a desktop notification.
 *
 * Best‑effort – a missing notification daemon or unsupported platform
 * degrades silently to a log warning.
 */
void dm_notify_send(const char *title, const char *body,
                    DmNotifyUrgency urgency);

#ifdef __cplusplus
}
#endif

#endif /* UTILS_NOTIFY_H */
