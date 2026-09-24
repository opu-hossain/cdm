// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "notify.h"
#include "log.h"

#ifdef __linux__

#include <libnotify/notify.h>

static bool g_notify_ready = false;

void dm_notify_init(void) {
  if (notify_is_initted()) {
    g_notify_ready = true;
    return;
  }
  g_notify_ready = notify_init("cdm");
  if (!g_notify_ready) {
    LOG_WARN("Could not initialize desktop notifications "
             "(no DBUS/notification daemon?)");
  }
}

void dm_notify_shutdown(void) {
  if (g_notify_ready && notify_is_initted())
    notify_uninit();
  g_notify_ready = false;
}

void dm_notify_send(const char *title, const char *body,
                    DmNotifyUrgency urgency) {
  if (!g_notify_ready)
    return;

  NotifyNotification *n = notify_notification_new(title, body, NULL);
  if (!n) {
    LOG_WARN("Failed to construct notification for: %s", title);
    return;
  }

  notify_notification_set_urgency(n, urgency == DM_NOTIFY_ERROR
                                         ? NOTIFY_URGENCY_CRITICAL
                                         : NOTIFY_URGENCY_NORMAL);

  GError *error = NULL;
  if (!notify_notification_show(n, &error)) {
    LOG_WARN("Failed to show notification '%s': %s", title,
             error ? error->message : "unknown error");
    if (error)
      g_error_free(error);
  }
  g_object_unref(n);
}

#else /* Windows / macOS – not yet implemented */

static bool g_warned_once = false;

void dm_notify_init(void) {
  if (!g_warned_once) {
    LOG_INFO("Desktop notifications are not yet implemented on this "
             "platform – downloads will proceed silently");
    g_warned_once = true;
  }
}

void dm_notify_shutdown(void) {}

void dm_notify_send(const char *title, const char *body,
                    DmNotifyUrgency urgency) {
  (void)title;
  (void)body;
  (void)urgency;
}

#endif
