// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "path.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void path_filename_from_url(const char *url, char *out, size_t out_size) {
  if (!out || out_size == 0)
    return;

  const char *slash = strrchr(url, '/');
  const char *name = slash ? slash + 1 : url;
  char temp[512];
  strncpy(temp, name, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = '\0';

  char *query = strchr(temp, '?');
  if (query)
    *query = '\0';
  if (temp[0] == '\0')
    strncpy(temp, "download.bin", sizeof(temp) - 1);

  strncpy(out, temp, out_size - 1);
  out[out_size - 1] = '\0';
}

bool path_join(const char *dir, const char *filename, char *out,
               size_t out_size) {
#ifdef _WIN32
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  size_t dir_len = strlen(dir);
  size_t filename_len = strlen(filename);
  bool has_separator = dir_len > 0 && dir[dir_len - 1] == sep;
  size_t required = dir_len + filename_len + (has_separator ? 1 : 2);

  if (required > out_size)
    return false;

  memcpy(out, dir, dir_len);
  if (!has_separator)
    out[dir_len++] = sep;
  memcpy(out + dir_len, filename, filename_len + 1);
  return true;
}

static bool path_exists(const char *path) {
  struct stat info;
#ifdef _WIN32
  return stat(path, &info) == 0 || errno != ENOENT;
#else
  return lstat(path, &info) == 0 || errno != ENOENT;
#endif
}

bool path_make_unique(const char *path, char *out, size_t out_size) {
  if (!path || !out || out_size == 0)
    return false;

  if (!path_exists(path)) {
    if (strlen(path) >= out_size)
      return false;
    strcpy(out, path);
    return true;
  }

  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(path, '\\');
  if (backslash && (!slash || backslash > slash))
    slash = backslash;
#endif
  const char *filename = slash ? slash + 1 : path;
  size_t prefix_len = slash ? (size_t)(filename - path) : 0;
  const char *extension = strrchr(filename, '.');
  if (!extension || extension == filename)
    extension = filename + strlen(filename);
  /* Keep common tar archive suffixes together when numbering archives. */
  if (extension > filename &&
      (strcmp(extension, ".gz") == 0 || strcmp(extension, ".bz2") == 0 ||
       strcmp(extension, ".xz") == 0 || strcmp(extension, ".zst") == 0)) {
    const char *dot = extension;
    while (dot > filename && dot[-1] != '.')
      dot--;
    if (dot > filename && (size_t)(extension - dot) == 3 &&
        memcmp(dot, "tar", 3) == 0)
      extension = dot - 1;
  }
  size_t stem_len = (size_t)(extension - filename);
  size_t extension_len = strlen(extension);

  for (unsigned int suffix = 1; suffix < 1000000; suffix++) {
    int written = snprintf(out, out_size, "%.*s%.*s(%u)%.*s", (int)prefix_len,
                           path, (int)stem_len, filename, suffix,
                           (int)extension_len, extension);
    if (written < 0 || (size_t)written >= out_size)
      return false;
    if (!path_exists(out))
      return true;
  }

  return false;
}
