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

/* English UI copy stays available when no locale catalog is installed. */
static const struct { const char *key, *value; } builtin[] = {
  {"cli.usage.main", "Usage: cdm cli <command> [args...]"},
  {"cli.usage.list", "Usage: cdm cli list [--offset N] [--limit N] [--status S]"},
  {"cli.usage.export", "Usage: cdm cli export --out FILE [--include-history] [--include-secrets]"},
  {"cli.usage.import", "Usage: cdm cli import --in FILE [--merge|--replace] [--yes]"},
  {"cli.usage.batch", "Usage: cdm cli add --file list.txt [dest_dir]"},
  {"cli.commands", "Commands:"},
  {"cli.help.add", "  add <url> [dest_dir] [--cookie V] [--referrer V] [--header \"K: V\"] [--sha256 HEX] [--limit BYTES_PER_SEC]"},
  {"cli.help.header", "         (--header may be repeated)"},
  {"cli.help.batch", "  add --file list.txt [dest_dir]  Add one URL per line"},
  {"cli.help.pause", "  pause  <id>          Pause a download"},
  {"cli.help.resume", "  resume <id>          Resume a download"},
  {"cli.help.cancel", "  cancel <id>          Cancel a download"},
  {"cli.help.list", "  list [--offset N] [--limit N] [--status S]"},
  {"cli.help.export", "  export --out FILE [--include-history] [--include-secrets]"},
  {"cli.help.import", "  import --in FILE [--merge|--replace] [--yes]"},
  {"cli.error.invalid_offset", "Invalid list offset:"},
  {"cli.error.invalid_limit", "List limit must be 1.."},
  {"cli.error.invalid_status", "Invalid list status"},
  {"cli.error.unknown_list_flag", "Unknown list flag:"},
  {"cli.error.list_size_unsupported", "Daemon does not support history rows with size"},
  {"cli.error.list_ipc", "Daemon does not support sized history pages or IPC failed"},
  {"cli.list.header", "id\tstatus\tpercent\tsize\tfilename"},
  {"cli.warn.export_secrets", "Warning: export includes proxy credentials and stored HTTP headers/cookies; HTTP Basic passwords are never exported."},
  {"cli.warn.export_history", "Warning: history URLs may contain private query values."},
  {"cli.error.export", "Could not export JSON (daemon version, IPC, or output path error)"},
  {"cli.exported", "Exported JSON to"},
  {"cli.error.import_version", "Daemon does not support JSON import"},
  {"cli.error.import_yes", "Replace requires --yes outside an interactive terminal"},
  {"cli.import.confirm", "Replace local history and settings after backup? [y/N] "},
  {"cli.error.import_file", "Could not resolve import file:"},
  {"cli.error.import", "Import failed"},
  {"cli.error.import_rejected", "invalid file or active download"},
  {"cli.error.import_daemon", "daemon or backup error"},
  {"cli.import.replaced", "Replaced history and settings"},
  {"cli.import.merged", "Merged missing history"},
  {"cli.error.destination", "Could not create a destination path"},
  {"cli.error.add_rejected", "Daemon rejected the download"},
  {"cli.add.duplicate", "Already downloading (ID"},
  {"cli.add.created", "Download added (ID:"},
  {"cli.add.initial_path", ", initial path"},
  {"cli.add.final_note", "; final filename may change after probing)"},
  {"cli.error.url_file", "Could not read URL file:"},
  {"cli.error.url_long", "URL too long"},
  {"cli.batch.line", "Line"},
  {"cli.batch.failed", "failed"},
  {"cli.error.daemon", "Is the daemon running? Start it with: cdm daemon"},
  {"cli.error.add_options", "Invalid add options"},
  {"cli.error.download_id", "Invalid download ID:"},
  {"cli.pause.done", "Paused download"},
  {"cli.pause.failed", "Could not pause download"},
  {"cli.resume.done", "Resumed download"},
  {"cli.resume.failed", "Could not resume download"},
  {"cli.cancel.done", "Cancelled download"},
  {"cli.cancel.failed", "Could not cancel download"},
};

static const char *builtin_value(const char *key) {
  for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++)
    if (strcmp(builtin[i].key, key) == 0)
      return builtin[i].value;
  return NULL;
}

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
  if (g_loaded)
    for (size_t i = 0; i < g_count; i++)
      if (strcmp(g_catalog[i].key, key) == 0) {
        const char *value = g_catalog[i].value;
        dm_mutex_unlock(&g_mutex);
        return value;
      }
  const char *english = builtin_value(key);
  if (english) {
    dm_mutex_unlock(&g_mutex);
    return english;
  }
  if (!g_loaded) {
    dm_mutex_unlock(&g_mutex);
    return key;
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
