// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#ifndef UTILS_PATH_H
#define UTILS_PATH_H

#include <stdbool.h>
#include <stddef.h>

/** Derive a safe filename from the final URL path segment. */
void path_filename_from_url(const char *url, char *out, size_t out_size);

/** Join a directory and filename, returning false if the result is too long. */
bool path_join(const char *dir, const char *filename, char *out,
               size_t out_size);

/** Choose an unused path, inserting " (n)" before the extension if needed. */
bool path_make_unique(const char *path, char *out, size_t out_size);

typedef bool (*PathConflictFn)(const char *path, void *context);

bool path_make_unique_with_conflict(const char *path, char *out,
                                    size_t out_size, PathConflictFn conflict,
                                    void *context);

#endif /* UTILS_PATH_H */