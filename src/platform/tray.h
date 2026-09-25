// SPDX-License-Identifier: MIT
#ifndef PLATFORM_TRAY_H
#define PLATFORM_TRAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *label;
  bool enabled;
  void (*activate)(void *user_data);
  void *user_data;
} TrayMenuItem;

/* The API is called by the daemon loop. Menu callbacks run on the tray thread. */
int tray_init(const char *icon_path);
void tray_set_progress(uint64_t received, uint64_t total);
void tray_set_menu(const TrayMenuItem *items, size_t count);
void tray_shutdown(void);

#endif
