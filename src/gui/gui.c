#include "../platform/thread.h"
#include "../utils/config.h"
#include "../utils/log.h"
#include "../vendor/cJSON.h"
#include "../vendor/tinyfiledialogs.h"
#include "gui_client.h"
#include "gui_model.h"
#include "gui_worker.h"
#include "webview/webview.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <gtk/gtk.h>
#endif

static void filename_from_url(const char *url, char *out, size_t out_size) {
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

static void join_path(const char *dir, const char *filename, char *out,
                      size_t out_size) {
#ifdef _WIN32
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  size_t dir_len = strlen(dir);
  if (dir_len > 0 && dir[dir_len - 1] == sep) {
    snprintf(out, out_size, "%s%s", dir, filename);
  } else {
    snprintf(out, out_size, "%s%c%s", dir, sep, filename);
  }
}

// Not static anymore — called from gui_worker.c's dispatch_result() on the
// UI thread. Declared in gui_worker.c via `extern`; kept here rather than
// a shared header since it's a narrow, single-purpose seam.
void gui_push_state_to_ui(webview_t w) {
  GuiRow rows[GUI_MODEL_MAX_ROWS];
  int count = gui_model_snapshot_rows(rows, GUI_MODEL_MAX_ROWS);

  cJSON *root = cJSON_CreateArray();
  for (int i = 0; i < count; i++) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "id", rows[i].id);
    cJSON_AddStringToObject(item, "url", rows[i].url);
    cJSON_AddStringToObject(item, "status", rows[i].status);
    cJSON_AddNumberToObject(item, "progress", rows[i].progress);
    cJSON_AddStringToObject(item, "dest_path", rows[i].dest_path);
    cJSON_AddItemToArray(root, item);
  }

  char *json_str = cJSON_PrintUnformatted(root);
  size_t script_len = strlen(json_str) + 32;
  char *script = malloc(script_len);
  snprintf(script, script_len, "window.updateState(%s);", json_str);
  webview_eval(w, script);
  free(script);
  free(json_str);
  cJSON_Delete(root);
}

void gui_return_details_json(webview_t w, const char *seq,
                             const GuiDownloadDetails *d) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "cookie", d->cookie);
  cJSON_AddStringToObject(root, "referrer", d->referrer);
  cJSON_AddStringToObject(root, "extra_headers", d->extra_headers);
  cJSON_AddStringToObject(root, "expected_sha256", d->expected_sha256);
  cJSON_AddNumberToObject(root, "speed_limit_bps", (double)d->speed_limit_bps);

  char *json_str = cJSON_PrintUnformatted(root);
  webview_return(w, seq, 0, json_str);
  cJSON_free(json_str);
  cJSON_Delete(root);
}

void gui_push_connection_state(webview_t w, bool connected) {
  const char *script =
      connected
          ? "window.setConnectionState && window.setConnectionState(true);"
          : "window.setConnectionState && window.setConnectionState(false);";
  webview_eval(w, script);
}

static void js_ready(const char *seq, const char *req, void *arg) {
  (void)req;
  webview_t w = (webview_t)arg;
  gui_push_state_to_ui(w); // runs on the UI thread already (this handler
                           // IS the UI thread) — fine, this one's a pure
                           // render call with no network I/O in it
  webview_return(w, seq, 0, "{}");
}

// ==========================================
// Javascript Bindings — thin: validate, enqueue, return immediately.
// NEVER call gui_client_* or gui_push_state_to_ui directly from here.
// ==========================================

static void js_add_download(const char *seq, const char *req, void *arg) {
  webview_t w = (webview_t)arg;
  cJSON *args = cJSON_Parse(req);

  cJSON *url_item = args ? cJSON_GetArrayItem(args, 0) : NULL;
  cJSON *dest_item = args ? cJSON_GetArrayItem(args, 1) : NULL;
  cJSON *cookie_item = args ? cJSON_GetArrayItem(args, 2) : NULL;
  cJSON *referrer_item = args ? cJSON_GetArrayItem(args, 3) : NULL;
  cJSON *headers_item = args ? cJSON_GetArrayItem(args, 4) : NULL;
  cJSON *sha256_item = args ? cJSON_GetArrayItem(args, 5) : NULL;
  cJSON *speed_item = args ? cJSON_GetArrayItem(args, 6) : NULL;

  if (!cJSON_IsString(url_item) || !url_item->valuestring ||
      url_item->valuestring[0] == '\0') {
    LOG_WARN("js_add_download: malformed request (missing/empty url)");
    webview_return(w, seq, 0, "{\"ok\":false}");
    if (args)
      cJSON_Delete(args);
    return;
  }

  const char *url = url_item->valuestring;
  const char *dest_folder =
      cJSON_IsString(dest_item) ? dest_item->valuestring : NULL;
  const char *cookie =
      cJSON_IsString(cookie_item) && cookie_item->valuestring[0]
          ? cookie_item->valuestring
          : NULL;
  const char *referrer =
      cJSON_IsString(referrer_item) && referrer_item->valuestring[0]
          ? referrer_item->valuestring
          : NULL;
  const char *headers =
      cJSON_IsString(headers_item) && headers_item->valuestring[0]
          ? headers_item->valuestring
          : NULL;
  const char *sha256 =
      cJSON_IsString(sha256_item) && sha256_item->valuestring[0]
          ? sha256_item->valuestring
          : NULL;
  uint64_t speed_limit =
      cJSON_IsNumber(speed_item) && speed_item->valuedouble > 0
          ? (uint64_t)speed_item->valuedouble
          : 0;

  LOG_INFO("js_add_download: url='%s' dest_folder='%s'", url,
           dest_folder ? dest_folder : "(null)");

  char resolved_folder[IPC_MAX_PATH_LEN];
  if (!dest_folder || dest_folder[0] == '\0') {
    strncpy(resolved_folder, config_get_default_download_dir(),
            sizeof(resolved_folder) - 1);
    resolved_folder[sizeof(resolved_folder) - 1] = '\0';
  } else {
    strncpy(resolved_folder, dest_folder, sizeof(resolved_folder) - 1);
    resolved_folder[sizeof(resolved_folder) - 1] = '\0';
  }

  char filename[512];
  char full_path[IPC_MAX_PATH_LEN];
  filename_from_url(url, filename, sizeof(filename));
  join_path(resolved_folder, filename, full_path, sizeof(full_path));

  LOG_INFO("js_add_download: enqueueing full_path='%s'", full_path);
  gui_worker_enqueue_add_download(seq, url, full_path, cookie, referrer,
                                  headers, sha256, speed_limit);

  cJSON_Delete(args);
}

static void js_action_download(const char *seq, const char *req, void *arg) {
  webview_t w = (webview_t)arg;
  cJSON *args = cJSON_Parse(req);
  cJSON *action_item = args ? cJSON_GetArrayItem(args, 0) : NULL;
  cJSON *id_item = args ? cJSON_GetArrayItem(args, 1) : NULL;

  if (!cJSON_IsString(action_item) || !cJSON_IsNumber(id_item)) {
    LOG_WARN("js_action_download: malformed request");
    webview_return(w, seq, 0, "{\"ok\":false}");
    if (args)
      cJSON_Delete(args);
    return;
  }

  const char *action = action_item->valuestring;
  uint32_t id = (uint32_t)id_item->valuedouble;

  if (strcmp(action, "pause") == 0) {
    gui_worker_enqueue_pause(seq, id);
  } else if (strcmp(action, "resume") == 0) {
    gui_worker_enqueue_resume(seq, id);
  } else if (strcmp(action, "cancel") == 0) {
    gui_worker_enqueue_cancel(seq, id);
  } else {
    LOG_WARN("js_action_download: unknown action '%s'", action);
    webview_return(w, seq, 0, "{\"ok\":false}");
  }

  cJSON_Delete(args);
}

static void js_get_details(const char *seq, const char *req, void *arg) {
  (void)arg;
  cJSON *args = cJSON_Parse(req);
  cJSON *id_item = args ? cJSON_GetArrayItem(args, 0) : NULL;

  if (!cJSON_IsNumber(id_item)) {
    LOG_WARN("js_get_details: malformed request");
    webview_return((webview_t)arg, seq, 0, "{\"ok\":false}");
    if (args)
      cJSON_Delete(args);
    return;
  }

  uint32_t id = (uint32_t)id_item->valuedouble;
  gui_worker_enqueue_get_details(seq, id);

  cJSON_Delete(args);
}

#ifndef _WIN32
static char *pick_folder_native(void) {
  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      "Choose Download Folder", NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);

  const char *default_dir = config_get_default_download_dir();
  if (default_dir && default_dir[0] != '\0') {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), default_dir);
  }

  char *selected = NULL;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    selected = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
  }

  gtk_widget_destroy(dialog);
  return selected;
}
#endif

static void js_pick_folder(const char *seq, const char *req, void *arg) {
  (void)req;
  webview_t w = (webview_t)arg;
  char *picked = NULL;

#ifdef _WIN32
  picked = tinyfd_selectFolderDialog("Choose Download Folder", "");
#else
  picked = pick_folder_native();
#endif

  char result[IPC_MAX_PATH_LEN + 32];
  if (picked) {
    snprintf(result, sizeof(result), "\"%s\"", picked);
    free(picked);
  } else {
    strcpy(result, "null");
  }
  webview_return(w, seq, 0, result);
}

// ==========================================
// Main Entry
// ==========================================
int run_gui(void) {
  if (!gui_client_connect()) {
    fprintf(stderr, "Cannot connect to daemon.\n");
    return 1;
  }
  gui_model_init();

  GuiDownloadRecord *records = NULL;
  int count = 0;
  if (gui_client_list_all(&records, &count)) {
    gui_model_apply_snapshot(records, count);
    free(records);
  }

  webview_t w = webview_create(0, NULL);
  webview_set_title(w, "Download Manager");
  webview_set_size(w, 800, 600, WEBVIEW_HINT_NONE);

  webview_bind(w, "c_add_download", js_add_download, w);
  webview_bind(w, "c_action_download", js_action_download, w);
  webview_bind(w, "c_pick_folder", js_pick_folder, w);
  webview_bind(w, "c_ready", js_ready, w);
  webview_bind(w, "c_get_details", js_get_details, w);

  char file_url[512];
  const char *pwd = getenv("PWD");
  if (!pwd) {
    LOG_WARN("PWD not set, falling back to relative path");
    pwd = ".";
  }
  snprintf(file_url, sizeof(file_url), "file://%s/src/gui/ui/index.html", pwd);
  webview_navigate(w, file_url);

  gui_worker_start(w);

  webview_run(w);

  gui_worker_stop();
  webview_destroy(w);
  gui_client_disconnect();
  return 0;
}
