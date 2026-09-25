// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef UTILS_URL_H
#define UTILS_URL_H

#include <stdbool.h>
#include <stddef.h>

/* Normalize an HTTP(S) URL for duplicate comparison. Returns false on an
 * invalid URL or insufficient output space. Scheme/host are lowercased,
 * default port and fragment are removed; userinfo, path and query retain case.
 */
bool url_normalize(const char *url, char *out, size_t out_size);

#endif /* UTILS_URL_H */
