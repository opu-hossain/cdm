// SPDX-License-Identifier: MIT
#include "i18n.h"
#include "log.h"
#include "../platform/thread.h"
#include "../vendor/tomlc17.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

typedef struct {
  char *key;
  char *value;
} Translation;
typedef struct MissingKey {
  char *key;
  struct MissingKey *next;
} MissingKey;

static dm_mutex_t g_mutex;
static once_flag g_once = ONCE_FLAG_INIT;
static Translation *g_catalog;
static size_t g_count;
static bool g_loaded;
static MissingKey *g_missing;

static void init_mutex(void) { dm_mutex_init(&g_mutex); }
static void ensure_mutex(void) { call_once(&g_once, init_mutex); }

static char *copy_string(const char *source, size_t length) {
  char *copy = malloc(length + 1);
  if (!copy)
    return NULL;
  memcpy(copy, source, length);
  copy[length] = '\0';
  return copy;
}

static void free_catalog(Translation *catalog, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(catalog[i].key);
    free(catalog[i].value);
  }
  free(catalog);
}

static bool valid_key(const char *key, size_t length) {
  if (!key || length == 0 || length > 128)
    return false;
  for (size_t i = 0; i < length; i++)
    if (!((key[i] >= 'a' && key[i] <= 'z') ||
          (key[i] >= 'A' && key[i] <= 'Z') ||
          (key[i] >= '0' && key[i] <= '9')) && key[i] != '.' &&
        key[i] != '_' && key[i] != '-')
      return false;
  return true;
}

bool tr_load_catalog(const char *path) {
  if (!path || !path[0])
    return false;
  FILE *file = fopen(path, "r");
  if (!file)
    return false;
  toml_result_t parsed = toml_parse_file(file);
  fclose(file);
  if (!parsed.ok) {
    toml_free(parsed);
    return false;
  }
  toml_datum_t root = parsed.toptab;
  bool valid = root.type == TOML_TABLE && root.u.tab.size >= 0 &&
               root.u.tab.size <= 10000;
  size_t count = valid ? (size_t)root.u.tab.size : 0;
  Translation *catalog = valid ? calloc(count ? count : 1, sizeof(*catalog)) : NULL;
  valid = valid && catalog;
  for (size_t i = 0; i < count && valid; i++) {
    const char *key = root.u.tab.key[i];
    size_t length = (size_t)root.u.tab.len[i];
    toml_datum_t value = root.u.tab.value[i];
    valid = valid_key(key, length) && value.type == TOML_STRING &&
            value.u.str.ptr && value.u.str.len >= 0 &&
            value.u.str.len <= 4096;
    if (valid) {
      catalog[i].key = copy_string(key, length);
      catalog[i].value = copy_string(value.u.str.ptr,
                                     (size_t)value.u.str.len);
      valid = catalog[i].key && catalog[i].value;
    }
  }
  toml_free(parsed);
  if (!valid) {
    free_catalog(catalog, count);
    return false;
  }
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  if (g_loaded) {
    dm_mutex_unlock(&g_mutex);
    free_catalog(catalog, count);
    return false;
  }
  g_catalog = catalog;
  g_count = count;
  g_loaded = true;
  dm_mutex_unlock(&g_mutex);
  return true;
}

const char *tr(const char *key) {
  if (!key)
    return "";
  ensure_mutex();
  dm_mutex_lock(&g_mutex);
  if (!g_loaded) {
    dm_mutex_unlock(&g_mutex);
    return key;
  }
  for (size_t i = 0; i < g_count; i++)
    if (strcmp(g_catalog[i].key, key) == 0) {
      const char *value = g_catalog[i].value;
      dm_mutex_unlock(&g_mutex);
      return value;
    }
  for (MissingKey *it = g_missing; it; it = it->next)
    if (strcmp(it->key, key) == 0) {
      dm_mutex_unlock(&g_mutex);
      return key;
    }
  size_t length = strlen(key);
  MissingKey *missing = length <= 128 ? calloc(1, sizeof(*missing)) : NULL;
  if (missing) {
    missing->key = copy_string(key, length);
    if (missing->key) {
      missing->next = g_missing;
      g_missing = missing;
    } else {
      free(missing);
      missing = NULL;
    }
  }
  dm_mutex_unlock(&g_mutex);
  if (missing)
    LOG_DEBUG("missing translation key: %s", key);
  return key;
}
