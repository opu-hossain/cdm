// SPDX-License-Identifier: MIT
#ifndef CDM_UTILS_I18N_H
#define CDM_UTILS_I18N_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load one flat TOML catalog at startup. Quote dotted keys in the file.
 * A successfully loaded catalog is immutable for the process lifetime so
 * pointers returned by tr() remain valid across threads. */
bool tr_load_catalog(const char *path);
/* Load the selected packaged locale once; "en" uses the built-in strings. */
bool tr_load_locale(const char *locale);
/* Missing keys return the caller's key and are logged once at DEBUG. */
const char *tr(const char *key);

#ifdef __cplusplus
}
#endif

#endif
