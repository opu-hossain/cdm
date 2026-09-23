#include "diskspace.h"

#ifndef _WIN32
#include <sys/statvfs.h>

bool diskspace_get(const char *path, DiskSpace *out) {
  if (!path || !*path || !out)
    return false;
  struct statvfs fs;
  if (statvfs(path, &fs) != 0 || !fs.f_frsize || !fs.f_blocks)
    return false;
  if (fs.f_blocks > UINT64_MAX / fs.f_frsize ||
      fs.f_bavail > UINT64_MAX / fs.f_frsize)
    return false;
  out->total_bytes = (uint64_t)fs.f_blocks * fs.f_frsize;
  out->free_bytes = (uint64_t)fs.f_bavail * fs.f_frsize;
  if (out->free_bytes > out->total_bytes)
    out->free_bytes = out->total_bytes;
  return true;
}
#else
bool diskspace_get(const char *path, DiskSpace *out) {
  (void)path;
  (void)out;
  /* TODO(windows): Use GetDiskFreeSpaceExW for the directory's volume. */
  return false;
}
#endif
