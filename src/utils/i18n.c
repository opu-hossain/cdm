// SPDX-License-Identifier: MIT
#include "i18n.h"
#include "log.h"
#include "../platform/spawn.h"
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
  {"gui.locale", "Language"},
  {"gui.locale_english", "English"},
  {"gui.locale_spanish", "Spanish"},
  {"gui.locale_restart_required", "Restart cdm to apply the language."},
  {"notify.download_complete", "Download Complete"},
  {"notify.download_failed", "Download Failed"},
  {"host.error.invalid_site_exclusions", "invalid site exclusion list"},
  {"host.error.invalid_excluded_hostname", "invalid excluded hostname"},
  {"host.error.invalid_offer", "invalid or unsupported download offer"},
  {"host.error.invalid_json", "invalid JSON"},
  {"host.error.invalid_url_or_mode", "invalid download offer URL or mode"},
  {"host.error.invalid_or_oversized_offer", "invalid or oversized download offer"},
  {"host.error.too_many_pending", "too many pending downloads"},
  {"host.error.daemon_start", "cdm daemon could not start"},
  {"host.error.daemon_unavailable", "cdm daemon is unavailable"},
  {"host.error.media_unsupported", "daemon does not support media offers"},
  {"host.error.context_unsupported", "daemon does not support browser context"},
  {"host.error.offer_rejected", "daemon rejected download offer"},
  {"host.error.popup_missing", "cannot locate cdm popup binary"},
  {"host.error.popup_launch", "could not launch cdm popup"},
  {"host.error.daemon_disconnected", "cdm daemon disconnected"},
  {"host.error.offer_expired", "download offer expired"},
  {"gui.file_size", "File size"},
  {"gui.already_downloading", "Already downloading"},
  {"gui.of", "of"},
  {"gui.downloaded", "downloaded"},
  {"gui.speed", "Speed"},
  {"gui.time_remaining", "Time remaining"},
  {"gui.unknown", "unknown"},
  {"gui.seconds_short", "sec"},
  {"gui.used", "used"},
  {"gui.free", "free"},
  {"gui.queues_lower", "queues"},
  {"gui.downloads_lower", "downloads"},
  {"gui.eta", "ETA"},
  {"gui.speed_limit", "Speed limit"},
  {"gui.headers", "Headers"},
  {"gui.could_not_open_destination", "Could not open destination"},
  {"gui.could_not_copy_url", "Could not copy URL"},
  {"gui.current", "Current"},
  {"gui.cannot_connect_to_daemon", "Cannot connect to daemon."},
  {"gui.command_queue_is_full_please_try_again", "Command queue is full. Please try again."},
  {"gui.cannot_open_a_relative_destination_folder", "Cannot open a relative destination folder."},
  {"gui.opening_files_requires_an_absolute_destination_path", "Opening files requires an absolute destination path."},
  {"gui.all", "All"},
  {"gui.downloading", "Downloading"},
  {"gui.completed", "Completed"},
  {"gui.queues", "Queues"},
  {"gui.search_downloads", "Search downloads..."},
  {"gui.connected", "Connected"},
  {"gui.disconnected", "Disconnected"},
  {"gui.add_queue", "+ Add queue"},
  {"gui.new_download", "+ New Download"},
  {"gui.categories", "CATEGORIES"},
  {"gui.all_categories", "All categories"},
  {"gui.previous", "Previous"},
  {"gui.next", "Next"},
  {"gui.local_storage", "LOCAL STORAGE"},
  {"gui.storage_usage_unavailable", "Storage usage unavailable"},
  {"gui.add_download", "Add download"},
  {"gui.cancel_selected_download", "Cancel selected download"},
  {"gui.pause_selected_download", "Pause selected download"},
  {"gui.resume_selected_download", "Resume selected download"},
  {"gui.settings.title", "Settings"},
  {"gui.your_download_queue_is_empty", "Your download queue is empty"},
  {"gui.paused", "Paused"},
  {"gui.loading_more_downloads", "Loading more downloads..."},
  {"gui.open", "Open"},
  {"gui.pause", "Pause"},
  {"gui.resume", "Resume"},
  {"gui.re_download", "Re-download"},
  {"gui.open_folder", "Open folder"},
  {"gui.copy_url", "Copy URL"},
  {"gui.show_details", "Show details"},
  {"gui.refresh_url", "Refresh URL"},
  {"gui.cancel", "Cancel"},
  {"gui.remove_from_list", "Remove from list"},
  {"gui.delete_file", "Delete file"},
  {"gui.import_cdm_json", "Import cdm JSON"},
  {"gui.json_files", "JSON files"},
  {"gui.export_cdm_json", "Export cdm JSON"},
  {"gui.could_not_open_the_selected_import_file", "Could not open the selected import file"},
  {"gui.replace_cdm_data", "Replace cdm data"},
  {"gui.replace_local_history_and_settings_a_database_backup_will_be_made", "Replace local history and settings? A database backup will be made."},
  {"gui.import_completed", "Import completed"},
  {"gui.export_completed", "Export completed"},
  {"gui.import_failed", "Import failed"},
  {"gui.export_failed", "Export failed"},
  {"gui.settings_save_note", "Changes are saved to config.toml."},
  {"gui.general", "GENERAL"},
  {"gui.monitor_clipboard_for_urls", "Monitor clipboard for URLs"},
  {"gui.ask_before_adding_a_copied_url", "Ask before adding a copied URL."},
  {"gui.theme", "Theme"},
  {"gui.system", "System"},
  {"gui.light", "Light"},
  {"gui.dark", "Dark"},
  {"gui.default_download_directory", "Default download directory"},
  {"gui.browse", "Browse"},
  {"gui.maximum_concurrent_downloads", "Maximum concurrent downloads"},
  {"gui.connections", "CONNECTIONS"},
  {"gui.connections_per_download_1_16", "Connections per download (1-16)"},
  {"gui.connect_timeout_seconds", "Connect timeout (seconds)"},
  {"gui.transfer_timeout_seconds", "Transfer timeout (seconds)"},
  {"gui.user_agent", "User-Agent"},
  {"gui.retries", "RETRIES"},
  {"gui.maximum_retry_attempts", "Maximum retry attempts"},
  {"gui.retry_base_delay_seconds", "Retry base delay (seconds)"},
  {"gui.retry_maximum_delay_seconds", "Retry maximum delay (seconds)"},
  {"gui.bandwidth", "BANDWIDTH"},
  {"gui.global_speed_limit_bytes_sec_0_unlimited", "Global speed limit (bytes/sec, 0 = unlimited)"},
  {"gui.proxy", "PROXY"},
  {"gui.mode", "Mode"},
  {"gui.none", "None"},
  {"gui.proxy_url", "Proxy URL"},
  {"gui.enter_a_proxy_url_with_scheme_and_host", "Enter a proxy URL with scheme and host."},
  {"gui.username", "Username"},
  {"gui.password", "Password"},
  {"gui.change_password", "Change password..."},
  {"gui.set_password", "Set password..."},
  {"gui.proxy_password", "Proxy password"},
  {"gui.enter_proxy_password", "Enter proxy password"},
  {"gui.clear_password", "Clear password"},
  {"gui.import_export", "IMPORT / EXPORT"},
  {"gui.export_settings_and_history", "Export settings and history..."},
  {"gui.import_missing", "Import missing..."},
  {"gui.replace_from_file", "Replace from file..."},
  {"gui.save_settings", "Save settings"},
  {"gui.invalid_number_check_the_allowed_range_in_each_field", "Invalid number. Check the allowed range in each field."},
  {"gui.proxy_url_needs_a_scheme_and_host", "Proxy URL needs a scheme and host."},
  {"gui.settings_saved_but_the_daemon_did_not_reload_them", "Settings saved, but the daemon did not reload them"},
  {"gui.could_not_save_or_apply_settings", "Could not save or apply settings"},
  {"gui.new_download_title", "New Download"},
  {"gui.url", "URL"},
  {"gui.save_to", "Save to"},
  {"gui.choose_folder", "Choose folder"},
  {"gui.choose_download_folder", "Choose download folder"},
  {"gui.hide_advanced_options", "- Advanced options"},
  {"gui.show_advanced_options", "+ Advanced options"},
  {"gui.cookie", "Cookie"},
  {"gui.referrer", "Referrer"},
  {"gui.extra_headers", "Extra headers"},
  {"gui.sha_256", "SHA-256"},
  {"gui.queue", "Queue"},
  {"gui.speed_limit_bytes_sec_0_unlimited", "Speed limit (bytes/sec, 0 = unlimited)"},
  {"gui.paste_multiple_urls", "Paste multiple URLs"},
  {"gui.enter_a_url_and_destination_folder", "Enter a URL and destination folder."},
  {"gui.speed_limit_must_be_between_0_and_1000000000", "Speed limit must be between 0 and 1000000000."},
  {"gui.could_not_prepare_or_enqueue_the_download", "Could not prepare or enqueue the download."},
  {"gui.one_http_s_url_per_line_blank_and_lines_are_ignored", "One HTTP(S) URL per line; blank and # lines are ignored."},
  {"gui.add_all", "Add all"},
  {"gui.invalid_url_destination_or_command_queue_full", "Invalid URL, destination, or command queue full."},
  {"gui.paste_at_least_one_url", "Paste at least one URL."},
  {"gui.enter_a_name_priority_0_1000_and_max_concurrent_0_64", "Enter a name, priority 0-1000, and max concurrent 0-64."},
  {"gui.enter_two_different_hh_mm_times_or_leave_both_empty", "Enter two different HH:MM times, or leave both empty."},
  {"gui.enable_this_post_action_in_post_actions_before_saving", "Enable this post-action in [post_actions] before saving."},
  {"gui.enter_a_command_to_run", "Enter a command to run."},
  {"gui.name", "Name"},
  {"gui.priority_0_1000", "Priority (0-1000)"},
  {"gui.max_concurrent_0_unlimited", "Max concurrent (0 = unlimited)"},
  {"gui.schedule_start_hh_mm", "Schedule start (HH:MM)"},
  {"gui.schedule_stop_hh_mm", "Schedule stop (HH:MM)"},
  {"gui.always", "Always"},
  {"gui.use_two_different_hh_mm_times_00_00_23_59", "Use two different HH:MM times (00:00-23:59)."},
  {"gui.active_during_this_time_window", "Active during this time window"},
  {"gui.shut_down", "Shut down"},
  {"gui.sleep", "Sleep"},
  {"gui.run_command", "Run command"},
  {"gui.post_action", "Post action"},
  {"gui.enable_this_action_under_post_actions_in_config_toml", "Enable this action under [post_actions] in config.toml."},
  {"gui.power_and_command_actions_need_explicit_config_enablement", "Power and command actions need explicit config enablement."},
  {"gui.command_executable_and_arguments", "Command (executable and arguments)"},
  {"gui.add_queue_title", "Add queue"},
  {"gui.order", "Order"},
  {"gui.priority", "Priority"},
  {"gui.max", "Max"},
  {"gui.schedule", "Schedule"},
  {"gui.drag", "Drag"},
  {"gui.edit", "Edit"},
  {"gui.delete", "Delete"},
  {"gui.save", "Save"},
  {"gui.delete_queue", "Delete queue"},
  {"gui.downloads_in_this_queue_will_move_to_default", "Downloads in this queue will move to Default."},
  {"gui.edit_category", "Edit category"},
  {"gui.add_category", "Add category"},
  {"gui.extensions_comma_separated", "Extensions (comma separated)"},
  {"gui.default_directory_optional", "Default directory (optional)"},
  {"gui.category_name_is_required", "Category name is required"},
  {"gui.delete_category", "Delete category"},
  {"gui.downloads_in_this_category_will_move_to_default", "Downloads in this category will move to Default."},
  {"gui.download_details", "Download details"},
  {"gui.loading_details", "Loading details..."},
  {"gui.close", "Close"},
  {"gui.delete_download_file", "Delete download file"},
  {"gui.delete_this_download_record_and_its_file_from_disk", "Delete this download record and its file from disk?"},
  {"gui.url_copied_to_clipboard", "URL copied to clipboard"},
  {"gui.browser.review", "Review download"},
  {"gui.dismiss", "Dismiss"},
  {"gui.download_complete", "Download complete"},
  {"gui.core_download_manager", "Core Download Manager"},
  {"gui.media_hls", "Media: HLS"},
  {"gui.media_dash", "Media: DASH"},
  {"gui.media_direct_video", "Media: direct video"},
  {"gui.download_url", "Download URL"},
  {"gui.save_as", "Save as"},
  {"gui.destination_folder", "Destination folder"},
  {"gui.browse_ellipsis", "Browse..."},
  {"gui.file_size_unknown", "File size: unknown"},
  {"gui.includes_browser_cookies", "Includes browser cookies"},
  {"gui.includes_browser_request_headers", "Includes browser request headers"},
  {"gui.user_agent_and_referer_available", "User-Agent and Referer available"},
  {"gui.user_agent_available", "User-Agent available"},
  {"gui.referer_available", "Referer available"},
  {"gui.confirm_to_use_this_browser_session", "Confirm to use this browser session"},
  {"gui.download", "Download"},
  {"gui.choose_a_valid_folder_and_filename", "Choose a valid folder and filename."},
  {"gui.could_not_start_download_check_the_destination", "Could not start download. Check the destination."},
  {"gui.progress_connection_lost_retrying", "Progress connection lost; retrying..."},
  {"gui.time_remaining_unknown", "Time remaining: unknown"},
  {"gui.url_refreshed_resume_from_the_main_window", "URL refreshed. Resume from the main window."},
  {"gui.refresh_failed_re_offer_or_re_download_if_content_changed", "Refresh failed; re-offer or re-download if content changed."},
  {"gui.open_file", "Open file"},
  {"gui.could_not_open_file", "Could not open file."},
  {"gui.could_not_open_folder", "Could not open folder."},
  {"gui.cancel_download", "Cancel download"},
  {"gui.cdm_browser_download", "cdm — Browser download"},
  {"gui.queues_could_not_be_loaded", "Queues could not be loaded"},
  {"gui.categories_could_not_be_loaded", "Categories could not be loaded"},
  {"gui.history_could_not_be_loaded", "History could not be loaded"},
  {"gui.gui_operation_failed", "GUI operation failed"},
  {"gui.download_could_not_be_added", "Download could not be added"},
  {"gui.download_details_are_unavailable", "Download details are unavailable"},
  {"gui.category_operation_failed", "Category operation failed"},
  {"gui.queue_operation_failed", "Queue operation failed"},
  {"gui.download_operation_failed", "Download operation failed"},
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
  {"cli.warn.export_secrets", "Warning: export includes proxy credentials, scanner command/arguments, and stored HTTP headers/cookies; HTTP Basic passwords are never exported."},
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

bool tr_load_locale(const char *locale) {
  if (!locale || strcmp(locale, "en") == 0)
    return locale != NULL;
  if (strcmp(locale, "es") != 0)
    return false;
  char executable[1024] = {0};
  get_self_exe_path(executable, sizeof(executable));
  /* TODO(platform): locate installed catalogs beside Windows/macOS bundles. */
  char *slash = strrchr(executable, '/');
  if (slash) {
    *slash = '\0';
    char path[1200];
    int written = snprintf(path, sizeof(path), "%s/i18n/es.toml", executable);
    if (written > 0 && (size_t)written < sizeof(path) &&
        tr_load_catalog(path))
      return true;
    written = snprintf(path, sizeof(path),
                       "%s/../share/cdm/i18n/es.toml", executable);
    if (written > 0 && (size_t)written < sizeof(path) &&
        tr_load_catalog(path))
      return true;
    written = snprintf(path, sizeof(path), "%s/../i18n/es.toml", executable);
    if (written > 0 && (size_t)written < sizeof(path) &&
        tr_load_catalog(path))
      return true;
  }
#ifdef CDM_INSTALLED_I18N_DIR
  char installed[1200];
  int written = snprintf(installed, sizeof(installed),
                         "%s/es.toml", CDM_INSTALLED_I18N_DIR);
  if (written > 0 && (size_t)written < sizeof(installed) &&
      tr_load_catalog(installed))
    return true;
#endif
  LOG_WARN("Could not load Spanish catalog; using built-in English text");
  return false;
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
