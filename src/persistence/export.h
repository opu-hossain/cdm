// SPDX-License-Identifier: MIT
#ifndef PERSISTENCE_EXPORT_H
#define PERSISTENCE_EXPORT_H
#include <stdbool.h>
#include <stdint.h>

/* Schema 1: {version, settings, downloads}. Byte counts are decimal strings
 * so uint64 values survive JSON readers that use IEEE-754 doubles. Browser
 * session values and HTTP Basic passwords are never exportable. */
int export_json_file(int daemon_socket, uint16_t daemon_version,
                     const char *destination, bool include_history,
                     bool include_secrets);
/* Daemon-only import: 0 success, -1 invalid input, -2 active downloads,
 * -3 backup/DB/configuration failure. Merge preserves existing IDs/settings. */
int import_json_file(const char *source, bool replace);
#endif
