// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "path.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static bool safe_disposition_name(const char *name) {
  if (!name[0] || strstr(name, ".."))
    return false;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++)
    if (*p < 32 || *p == 127 || *p == '/' || *p == '\\')
      return false;
  return true;
}

static bool valid_utf8(const unsigned char *p) {
  while (*p) {
    if (*p < 0x80) {
      p++;
    } else if (*p >= 0xC2 && *p <= 0xDF &&
               p[1] >= 0x80 && p[1] <= 0xBF) {
      p += 2;
    } else if (*p >= 0xE0 && *p <= 0xEF && p[1] && p[2] &&
               p[1] >= (*p == 0xE0 ? 0xA0 : 0x80) &&
               p[1] <= (*p == 0xED ? 0x9F : 0xBF) &&
               p[2] >= 0x80 && p[2] <= 0xBF) {
      p += 3;
    } else if (*p >= 0xF0 && *p <= 0xF4 && p[1] && p[2] && p[3] &&
               p[1] >= (*p == 0xF0 ? 0x90 : 0x80) &&
               p[1] <= (*p == 0xF4 ? 0x8F : 0xBF) &&
               p[2] >= 0x80 && p[2] <= 0xBF &&
               p[3] >= 0x80 && p[3] <= 0xBF) {
      p += 4;
    } else {
      return false;
    }
  }
  return true;
}

static bool decode_extended_name(const char *value, char *out, size_t cap) {
  if (strncasecmp(value, "UTF-8'", 6) != 0)
    return false;
  const char *encoded = strchr(value + 6, '\'');
  if (!encoded)
    return false;
  encoded++;
  size_t n = 0;
  for (const char *p = encoded; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    if (ch == '%') {
      int hi = hex_digit(p[1]);
      int lo = p[1] ? hex_digit(p[2]) : -1;
      if (hi < 0 || lo < 0)
        return false;
      ch = (unsigned char)(hi * 16 + lo);
      p += 2;
    } else if (ch >= 128) {
      return false;
    }
    if (ch == 0 || n + 1 >= cap)
      return false;
    out[n++] = (char)ch;
  }
  out[n] = '\0';
  return safe_disposition_name(out) &&
         valid_utf8((const unsigned char *)out);
}

bool path_filename_from_disposition(const char *header, char *out,
                                    size_t out_size) {
  if (!out || out_size == 0)
    return false;
  out[0] = '\0';
  if (!header)
    return false;
  const char *p = header;
  if (strncasecmp(p, "Content-Disposition:", 20) == 0)
    p += 20;
  p = strchr(p, ';');
  if (!p)
    return false;

  char plain[512] = {0};
  char extended[512] = {0};
  while (*p) {
    p++;
    while (*p == ' ' || *p == '\t')
      p++;
    const char *key = p;
    while (*p && *p != '=' && *p != ';' && *p != '\r' && *p != '\n')
      p++;
    if (*p != '=') {
      if (*p != ';')
        break;
      continue;
    }
    const char *key_end = p;
    while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t'))
      key_end--;
    p++;
    while (*p == ' ' || *p == '\t')
      p++;

    char value[512];
    size_t n = 0;
    bool valid = true;
    if (*p == '"') {
      p++;
      while (*p && *p != '"' && *p != '\r' && *p != '\n') {
        char ch = *p++;
        if (ch == '\\' && *p == '"') {
          ch = '"';
          p++;
        }
        if (n + 1 < sizeof(value))
          value[n++] = ch;
        else
          valid = false;
      }
      if (*p == '"')
        p++;
      else
        valid = false;
      while (*p && *p != ';' && *p != '\r' && *p != '\n')
        p++;
    } else {
      while (*p && *p != ';' && *p != '\r' && *p != '\n') {
        if (n + 1 < sizeof(value))
          value[n++] = *p;
        else
          valid = false;
        p++;
      }
      while (n && (value[n - 1] == ' ' || value[n - 1] == '\t'))
        n--;
    }
    value[n] = '\0';
    size_t key_len = (size_t)(key_end - key);
    if (valid && key_len == 8 && strncasecmp(key, "filename", 8) == 0 &&
        safe_disposition_name(value))
      strcpy(plain, value);
    else if (valid && key_len == 9 &&
             strncasecmp(key, "filename*", 9) == 0) {
      char decoded[512] = {0};
      if (decode_extended_name(value, decoded, sizeof(decoded)))
        strcpy(extended, decoded);
    }
    if (*p != ';')
      break;
  }
  const char *chosen = extended[0] ? extended : plain;
  size_t length = strlen(chosen);
  if (!length || length >= out_size)
    return false;
  memcpy(out, chosen, length + 1);
  return true;
}

void path_filename_from_url(const char *url, char *out, size_t out_size) {
  if (!out || out_size == 0)
    return;
  char temp[512];
  size_t url_path_len = strcspn(url, "?#");
  if (url_path_len >= sizeof(temp))
    url_path_len = sizeof(temp) - 1;
  memcpy(temp, url, url_path_len);
  temp[url_path_len] = '\0';

  const char *slash = strrchr(temp, '/');
  const char *name = slash ? slash + 1 : temp;
  if (!name[0])
    name = "download.bin";

  char decoded[512];
  size_t n = 0;
  for (size_t i = 0; name[i] && n + 1 < sizeof(decoded); i++) {
    unsigned char ch = (unsigned char)name[i];
    if (ch == '%' && name[i + 1] && name[i + 2]) {
      int hi = hex_digit(name[i + 1]);
      int lo = hex_digit(name[i + 2]);
      if (hi >= 0 && lo >= 0) {
        unsigned char escaped = (unsigned char)(hi * 16 + lo);
        if (escaped >= 32 && escaped != 127 && escaped != '/' &&
            escaped != '\\') {
          ch = escaped;
          i += 2;
        }
      }
    }
    decoded[n++] = (char)ch;
  }
  decoded[n] = '\0';

  const char *chosen = safe_disposition_name(decoded) ? decoded : name;
  if (!safe_disposition_name(chosen))
    chosen = "download.bin";
  strncpy(out, chosen, out_size - 1);
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
