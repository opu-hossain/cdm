// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "url.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

bool url_normalize(const char *url, char *out, size_t out_size) {
  if (!url || !out || out_size == 0)
    return false;
  out[0] = '\0';
  const char *scheme_end = strstr(url, "://");
  if (!scheme_end)
    return false;
  size_t scheme_len = (size_t)(scheme_end - url);
  bool http = scheme_len == 4 && strncasecmp(url, "http", 4) == 0;
  bool https = scheme_len == 5 && strncasecmp(url, "https", 5) == 0;
  if (!http && !https)
    return false;

  const char *authority = scheme_end + 3;
  const char *authority_end = authority + strcspn(authority, "/?#");
  if (authority == authority_end)
    return false;
  const char *fragment = strchr(authority_end, '#');
  const char *url_end = fragment ? fragment : url + strlen(url);
  const char *host = authority;
  for (const char *p = authority; p < authority_end; p++)
    if (*p == '@')
      host = p + 1;
  if (host == authority_end)
    return false;

  const char *host_end = authority_end;
  const char *port = NULL;
  if (*host == '[') {
    const char *closing = memchr(host + 1, ']', (size_t)(authority_end - host - 1));
    if (!closing || closing == host + 1)
      return false;
    host_end = closing + 1;
    if (host_end < authority_end) {
      if (*host_end != ':')
        return false;
      port = host_end + 1;
    }
  } else {
    for (const char *p = host; p < authority_end; p++) {
      if (*p == ':') {
        host_end = p;
        port = p + 1;
        break;
      }
    }
  }
  if (host == host_end)
    return false;
  for (const char *p = authority; p < authority_end; p++)
    if ((unsigned char)*p < 33 || (unsigned char)*p == 127)
      return false;
  for (const char *p = host; p < host_end; p++)
    if (*p == '@' || (*p == ':' && *host != '['))
      return false;

  bool omit_port = false;
  if (port) {
    if (port == authority_end)
      return false;
    unsigned int number = 0;
    for (const char *p = port; p < authority_end; p++) {
      if (!isdigit((unsigned char)*p))
        return false;
      number = number * 10U + (unsigned int)(*p - '0');
      if (number > UINT16_MAX)
        return false;
    }
    omit_port = number == (http ? 80U : 443U);
  }

  size_t userinfo_len = (size_t)(host - authority);
  size_t host_len = (size_t)(host_end - host);
  size_t port_len = port && !omit_port ? (size_t)(authority_end - host_end) : 0;
  size_t tail_len = (size_t)(url_end - authority_end);
  size_t needed = scheme_len + 3 + userinfo_len + host_len + port_len + tail_len;
  if (needed >= out_size)
    return false;

  size_t n = 0;
  const char *scheme = http ? "http://" : "https://";
  size_t prefix_len = scheme_len + 3;
  memcpy(out + n, scheme, prefix_len);
  n += prefix_len;
  memcpy(out + n, authority, userinfo_len);
  n += userinfo_len;
  for (const char *p = host; p < host_end; p++)
    out[n++] = (char)tolower((unsigned char)*p);
  if (port_len) {
    memcpy(out + n, host_end, port_len);
    n += port_len;
  }
  memcpy(out + n, authority_end, tail_len);
  n += tail_len;
  out[n] = '\0';
  return true;
}
