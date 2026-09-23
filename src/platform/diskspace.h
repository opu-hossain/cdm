#ifndef PLATFORM_DISKSPACE_H
#define PLATFORM_DISKSPACE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint64_t total_bytes;
  uint64_t free_bytes;
} DiskSpace;

/* Space on the filesystem containing path, available to this user. */
bool diskspace_get(const char *path, DiskSpace *out);

#endif
