#include "../utils/config.h"
#include "../utils/i18n.h"
#include "../utils/log.h"
#include "../utils/path.h"
#include "../persistence/export.h"
#include "../platform/diskspace.h"
#include "../vendor/tinyfiledialogs.h"
#include "gui_backend_sdl.h"
#include "gui_client.h"
#include "gui_controller.h"
#include "gui_model.h"

#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#include "nuklear.h"
#include "theme.h"

#define GUI_URL_CAP 2048
#define GUI_FOLDER_CAP 1024
#define GUI_OPTION_CAP 4096
#define GUI_SHA256_CAP 65
#define GUI_SEARCH_CAP 256

typedef enum { TAB_ALL, TAB_DOWNLOADING, TAB_COMPLETED, TAB_QUEUES } GuiTab;

static const struct nk_user_font *bold_fonts[6];
static const struct nk_user_font *fonts[6]; /* 11 through 16 px */

typedef struct {
  bool connected, add_open, settings_open, details_open, details_found;
  bool delete_confirm_open;
  bool advanced, toast_open, duplicate_toast_open, menu_just_opened;
  uint32_t duplicate_id;
  bool history_loading;
  uint32_t history_offset, history_total, pending_scroll_adjust;
  uint32_t selected_id, menu_id, delete_confirm_id;
  GuiTab tab;
  uint32_t category; // 0 = all categories
  uint32_t category_menu_id, category_delete_id;
  int category_scroll;
  bool category_menu_open, category_menu_just_opened;
  bool category_edit_open, category_delete_open;
  struct nk_vec2 category_menu_pos;
  IpcCategoryV1 category_draft;
  char search[GUI_SEARCH_CAP];
  DiskSpace disk;
  bool disk_available;
  uint32_t disk_checked_at;
  struct nk_vec2 menu_pos;
  GuiDownloadDetails details;
  GuiRow toast;
  char error[256], settings_message[256];
  char delete_confirm_path[IPC_MAX_PATH_LEN];
  char url[GUI_URL_CAP], folder[GUI_FOLDER_CAP];
  char cookie[1024], referrer[2048], headers[GUI_OPTION_CAP], sha256[65];
  int speed_limit;
  char directory[GUI_FOLDER_CAP];
  int concurrent, attempts, base_delay, max_delay, global_speed;
  int max_connections, connect_timeout, transfer_timeout;
  char user_agent[256];
  ProxyMode proxy_mode;
  char proxy_url[512], proxy_username[128], proxy_password[256];
  char numbers[9][16];
  bool queue_add_open, queue_delete_open, batch_add_open;
  uint32_t queue_edit_id, queue_delete_id, queue_drag_id, add_queue_id;
  Queue queue_draft;
  char queue_priority[16], queue_cap[16];
  char batch_urls[8192];
  bool clipboard_monitor, clipboard_monitor_enabled, clipboard_offer_open;
  bool folder_explicit; // GUI main thread owns the destination choice
  ThemeId theme_saved, theme_preview;
  int locale_selection; // GUI main thread; 0 = English, 1 = Spanish
  bool theme_needs_apply;
  char clipboard_seen[GUI_URL_CAP], clipboard_offer[GUI_URL_CAP];
  uint32_t clipboard_checked_at, clipboard_changed_at;
} UiState;

static ThemeId theme_from_text(const char *value) {
  if (strcmp(value, "dark") == 0)
    return THEME_DARK;
  if (strcmp(value, "light") == 0)
    return THEME_LIGHT;
  return THEME_SYSTEM;
}

static const char *theme_to_text(ThemeId id) {
  return id == THEME_DARK ? "dark" : id == THEME_LIGHT ? "light" : "system";
}

static void copy_text(char *out, size_t size, const char *text) {
  snprintf(out, size, "%s", text ? text : "");
}

static void clipboard_baseline(UiState *ui) {
  char *text = SDL_GetClipboardText();
  ui->clipboard_seen[0] = '\0';
  if (text) {
    size_t length = strlen(text);
    if (length < sizeof(ui->clipboard_seen))
      memcpy(ui->clipboard_seen, text, length + 1);
    SDL_free(text);
  }
  ui->clipboard_changed_at = SDL_GetTicks();
}

static bool clipboard_url_valid(const char *text) {
  const char *host = NULL;
  if (strncmp(text, "https://", 8) == 0)
    host = text + 8;
  else if (strncmp(text, "http://", 7) == 0)
    host = text + 7;
  if (!host || !*host || *host == '/' || *host == '?' || *host == '#')
    return false;
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    if (isspace(*p) || iscntrl(*p))
      return false;
  return true;
}

static void poll_clipboard(UiState *ui, uint32_t now) {
  if (!ui->clipboard_monitor_enabled ||
      now - ui->clipboard_checked_at < 250)
    return;
  ui->clipboard_checked_at = now;
  char *text = SDL_GetClipboardText();
  if (!text)
    return;
  size_t length = strlen(text);
  if (length >= sizeof(ui->clipboard_seen)) {
    ui->clipboard_seen[0] = '\0';
    ui->clipboard_offer_open = false;
    SDL_free(text);
    return;
  }
  if (strcmp(ui->clipboard_seen, text) != 0) {
    memcpy(ui->clipboard_seen, text, length + 1);
    ui->clipboard_changed_at = now;
    ui->clipboard_offer_open = false;
  } else if (now - ui->clipboard_changed_at >= 500 &&
             clipboard_url_valid(text) &&
             strcmp(ui->clipboard_offer, text) != 0) {
    memcpy(ui->clipboard_offer, text, length + 1);
    ui->clipboard_offer_open = true;
  }
  SDL_free(text);
}

static const char *last_separator(const char *path) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(path, '\\');
  if (backslash && (!slash || backslash > slash))
    slash = backslash;
#endif
  return slash;
}

static const char *filename_for_row(const GuiRow *row) {
  if (row->dest_path[0]) {
    const char *slash = last_separator(row->dest_path);
    return slash ? slash + 1 : row->dest_path;
  }
  const char *slash = strrchr(row->url, '/');
  return slash ? slash + 1 : row->url;
}

static GuiRow *find_row(GuiRow *rows, int count, uint32_t id) {
  for (int i = 0; i < count; ++i)
    if (rows[i].id == id)
      return &rows[i];
  return NULL;
}

static bool is_status(const GuiRow *row, const char *status) {
  return row && strcmp(row->status, status) == 0;
}

static bool can_pause(const GuiRow *row) {
  return is_status(row, "ACTIVE") || is_status(row, "QUEUED");
}

static bool contains_case_insensitive(const char *text, const char *needle) {
  if (!*needle)
    return true;
  for (; *text; ++text) {
    const char *a = text, *b = needle;
    while (*a && *b && tolower((unsigned char)*a) ==
                          tolower((unsigned char)*b)) {
      ++a;
      ++b;
    }
    if (!*b)
      return true;
  }
  return false;
}

static bool row_matches(const UiState *ui, const GuiRow *row) {
  if (ui->tab == TAB_DOWNLOADING && !can_pause(row) &&
      !is_status(row, "PAUSED"))
    return false;
  if (ui->tab == TAB_COMPLETED && !is_status(row, "DONE"))
    return false;
  if (ui->category != 0 && row->category_id != ui->category)
    return false;
  return contains_case_insensitive(filename_for_row(row), ui->search) ||
         contains_case_insensitive(row->dest_path, ui->search);
}

static struct nk_rect screen_rect(struct nk_context *ctx, float x, float y,
                                  float w, float h) {
  return nk_layout_space_rect_to_screen(ctx, nk_rect(x, y, w, h));
}

static void fill(struct nk_context *ctx, struct nk_rect r, float radius,
                 struct nk_color color) {
  nk_fill_rect(nk_window_get_canvas(ctx), r, radius, color);
}

static void outline(struct nk_context *ctx, struct nk_rect r, float radius) {
  nk_stroke_rect(nk_window_get_canvas(ctx), r, radius, 1, BORDER);
}

static void label_font(struct nk_context *ctx, struct nk_rect r,
                       const char *value, const struct nk_user_font *font,
                       struct nk_color color, struct nk_color background) {
  char text[2048];
  copy_text(text, sizeof(text), value);
  int len = (int)strlen(text);
  if (font->width(font->userdata, font->height, text, len) > r.w) {
    while (len > 0 &&
           font->width(font->userdata, font->height, text, len) >
               r.w - font->width(font->userdata, font->height, "...", 3)) {
      --len;
      while (len > 0 && ((unsigned char)text[len] & 0xc0) == 0x80)
        --len;
    }
    memcpy(text + len, "...", 4);
    len += 3;
  }
  r.y += (r.h - font->height) / 2;
  r.h = font->height;
  nk_draw_text(nk_window_get_canvas(ctx), r, text, len, font, background,
               color);
}

static void label(struct nk_context *ctx, struct nk_rect r, const char *value,
                  int size, struct nk_color color, struct nk_color background) {
  label_font(ctx, r, value, fonts[size - 11], color, background);
}

static void bold_at(struct nk_context *ctx, float x, float y, float w, float h,
                    const char *text, int size, struct nk_color color,
                    struct nk_color background) {
  label_font(ctx, screen_rect(ctx, x, y, w, h), text, bold_fonts[size - 11],
             color, background);
}

static void text_at(struct nk_context *ctx, float x, float y, float w, float h,
                    const char *text, int size, struct nk_color color,
                    struct nk_color background) {
  label(ctx, screen_rect(ctx, x, y, w, h), text, size, color, background);
}

static bool button(struct nk_context *ctx, float x, float y, float w, float h,
                   const char *text, bool enabled, bool primary) {
  struct nk_style_button style = ctx->style.button;
  style.normal = nk_style_item_color(primary ? ACCENT : SURFACE2);
  style.hover = nk_style_item_color(primary ? ACCENT_HOVER : BORDER);
  style.active = nk_style_item_color(ACCENT_DIM);
  style.text_normal = style.text_hover = style.text_active =
      enabled ? (primary ? theme_contrast_ink(ACCENT) : TEXT) : DISABLED;
  if (!enabled)
    style.hover = style.active = style.normal;
  nk_layout_space_push(ctx, nk_rect(x, y, w, h));
  if (primary)
    nk_style_push_font(ctx, bold_fonts[2]);
  if (!enabled)
    nk_widget_disable_begin(ctx);
  bool clicked = nk_button_label_styled(ctx, &style, text);
  if (!enabled)
    nk_widget_disable_end(ctx);
  if (primary)
    nk_style_pop_font(ctx);
  return enabled && clicked;
}

static void report_enqueue(UiState *ui, bool ok) {
  if (!ok)
    copy_text(ui->error, sizeof(ui->error),
              tr("gui.command_queue_is_full_please_try_again"));
}

static bool add_download(const char *url, const char *folder,
                         const char *cookie, const char *referrer,
                         const char *headers, const char *sha256,
                         uint64_t speed_limit, uint32_t queue_id,
                         bool auto_directory) {
  char filename[512];
  char path[IPC_MAX_PATH_LEN];
  char unique_path[IPC_MAX_PATH_LEN];
  path_filename_from_url(url, filename, sizeof(filename));
  bool prepared = path_join(folder, filename, path, sizeof(path));
  if (prepared && auto_directory)
    memcpy(unique_path, path, strlen(path) + 1);
  else if (prepared)
    prepared = path_make_unique(path, unique_path, sizeof(unique_path));
  if (!prepared) {
    LOG_ERROR("nuklear_gui: could not create a destination path");
    return false;
  }

  IpcDownloadOptions options = {
      .cookie = cookie[0] ? cookie : NULL,
      .referrer = referrer[0] ? referrer : NULL,
      .extra_headers = headers[0] ? headers : NULL,
      .expected_sha256 = sha256[0] ? sha256 : NULL,
      .speed_limit_bps = speed_limit,
      .queue_id = queue_id,
      .auto_directory = auto_directory,
  };
  bool has_options = options.cookie || options.referrer ||
                     options.extra_headers || options.expected_sha256 ||
                     options.speed_limit_bps > 0 || options.queue_id > 0 ||
                     options.auto_directory;
  return gui_controller_enqueue_add_auto(url, unique_path,
                                         has_options ? &options : NULL);
}

/* SDL dispatches file URIs through the desktop on Linux, Windows and macOS.
   Percent encoding avoids treating '#', spaces or non-ASCII bytes as URI
   syntax. */
static void open_path(UiState *ui, const char *path, bool folder) {
  char local[IPC_MAX_PATH_LEN];
  copy_text(local, sizeof(local), path);
  if (folder) {
    const char *slash = last_separator(local);
    if (!slash) {
      copy_text(ui->error, sizeof(ui->error),
                tr("gui.cannot_open_a_relative_destination_folder"));
      return;
    }
    size_t end = (size_t)(slash - local);
#ifdef _WIN32
    if (end == 2 && local[1] == ':')
      ++end;
#endif
    local[end ? end : 1] = '\0';
  }
  const char *prefix = "file://";
#ifdef _WIN32
  for (char *p = local; *p; ++p)
    if (*p == '\\')
      *p = '/';
  if (isalpha((unsigned char)local[0]) && local[1] == ':')
    prefix = "file:///";
  else if (local[0] == '/' && local[1] == '/')
    prefix = "file:";
  else
#endif
      if (local[0] != '/') {
    copy_text(ui->error, sizeof(ui->error),
              tr("gui.opening_files_requires_an_absolute_destination_path"));
    return;
  }
  char uri[IPC_MAX_PATH_LEN * 3 + 16];
  size_t n = (size_t)snprintf(uri, sizeof(uri), "%s", prefix);
  for (const unsigned char *p = (const unsigned char *)local; *p; ++p) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || strchr("/-._~:", *p))
      uri[n++] = (char)*p;
    else {
      snprintf(uri + n, sizeof(uri) - n, "%%%02X", *p);
      n += 3;
    }
  }
  uri[n] = '\0';
  if (SDL_OpenURL(uri) != 0) {
    int written = snprintf(ui->error, sizeof(ui->error), "%s: %.190s",
                           tr("gui.could_not_open_destination"), SDL_GetError());
    if (written < 0 || (size_t)written >= sizeof(ui->error))
      ui->error[0] = '\0';
    LOG_ERROR("gui: %s", ui->error);
  }
}

static void open_settings(UiState *ui) {
  DownloadManagerConfig config;
  config_get(&config);
  copy_text(ui->directory, sizeof(ui->directory), config.default_download_dir);
  ui->concurrent = config.max_concurrent_downloads;
  ui->attempts = config.retry_max_attempts;
  ui->base_delay = config.retry_base_delay_sec;
  ui->max_delay = config.retry_max_delay_sec;
  ui->global_speed = (int)(config.max_speed_bytes_per_sec > 1000000000ULL
                               ? 1000000000
                               : config.max_speed_bytes_per_sec);
  ui->max_connections = config.max_connections_per_download;
  ui->connect_timeout = config.connect_timeout_sec;
  ui->transfer_timeout = config.transfer_timeout_sec;
  copy_text(ui->user_agent, sizeof(ui->user_agent), config.user_agent);
  ui->proxy_mode = config.proxy_mode;
  ui->clipboard_monitor = config.clipboard_monitor;
  ui->theme_saved = theme_from_text(config.ui_theme);
  ui->theme_preview = ui->theme_saved;
  ui->locale_selection = strcmp(config.ui_locale, "es") == 0 ? 1 : 0;
  ui->theme_needs_apply = true;
  copy_text(ui->proxy_url, sizeof(ui->proxy_url), config.proxy_url);
  copy_text(ui->proxy_username, sizeof(ui->proxy_username),
            config.proxy_username);
  copy_text(ui->proxy_password, sizeof(ui->proxy_password),
            config.proxy_password);
  int values[] = {ui->concurrent, ui->attempts, ui->base_delay,
                  ui->max_delay, ui->global_speed};
  for (int i = 0; i < 5; ++i)
    snprintf(ui->numbers[i], sizeof(ui->numbers[i]), "%d", values[i]);
  snprintf(ui->numbers[6], sizeof(ui->numbers[6]), "%d", ui->max_connections);
  snprintf(ui->numbers[7], sizeof(ui->numbers[7]), "%d", ui->connect_timeout);
  snprintf(ui->numbers[8], sizeof(ui->numbers[8]), "%d", ui->transfer_timeout);
  ui->settings_message[0] = '\0';
  ui->settings_open = true;
  ui->menu_id = 0;
}

/* Simple line icons keep the toolbar independent of platform glyph coverage. */
static bool tool_button(struct nk_context *ctx, float x, const char *kind,
                        const char *tooltip, bool enabled) {
  struct nk_rect r = screen_rect(ctx, x, 60, 32, 32);
  struct nk_style_button style = ctx->style.button;
  style.normal = nk_style_item_color(BG);
  style.hover = nk_style_item_color(enabled ? SURFACE2 : BG);
  style.active = nk_style_item_color(enabled ? BORDER : BG);
  nk_layout_space_push(ctx, nk_rect(x, 60, 32, 32));
  if (!enabled)
    nk_widget_disable_begin(ctx);
  bool clicked = nk_button_label_styled(ctx, &style, "");
  if (!enabled)
    nk_widget_disable_end(ctx);
  struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
  struct nk_color c = enabled ? MUTED : DISABLED;
  float cx = r.x + 16, cy = r.y + 16;
  if (!strcmp(kind, "add")) {
    nk_stroke_line(canvas, cx - 5, cy, cx + 5, cy, 1.5f, c);
    nk_stroke_line(canvas, cx, cy - 5, cx, cy + 5, 1.5f, c);
  } else if (!strcmp(kind, "cancel")) {
    nk_stroke_line(canvas, cx - 4, cy - 4, cx + 4, cy + 4, 1.5f, c);
    nk_stroke_line(canvas, cx + 4, cy - 4, cx - 4, cy + 4, 1.5f, c);
  } else if (!strcmp(kind, "pause")) {
    fill(ctx, nk_rect(cx - 4, cy - 5, 2, 10), 0, c);
    fill(ctx, nk_rect(cx + 2, cy - 5, 2, 10), 0, c);
  } else if (!strcmp(kind, "resume")) {
    nk_fill_triangle(canvas, cx - 4, cy - 6, cx + 5, cy, cx - 4, cy + 6, c);
  } else {
    nk_stroke_circle(canvas, nk_rect(cx - 5, cy - 5, 10, 10), 1.5f, c);
    nk_stroke_circle(canvas, nk_rect(cx - 1.5f, cy - 1.5f, 3, 3), 1, c);
    for (int i = -1; i <= 1; i += 2) {
      nk_stroke_line(canvas, cx + i * 5, cy, cx + i * 8, cy, 2, c);
      nk_stroke_line(canvas, cx, cy + i * 5, cx, cy + i * 8, 2, c);
      nk_stroke_line(canvas, cx + i * 4, cy - 4, cx + i * 6, cy - 6, 2, c);
      nk_stroke_line(canvas, cx + i * 4, cy + 4, cx + i * 6, cy + 6, 2, c);
    }
  }
  if (nk_input_is_mouse_hovering_rect(&ctx->input, r))
    nk_tooltip(ctx, tooltip);
  return enabled && clicked;
}

static bool nav_button(struct nk_context *ctx, float x, float y, float w,
                       float h, const char *title, bool active) {
  struct nk_style_button style = ctx->style.button;
  style.normal = nk_style_item_color(active ? ACCENT_DIM : SURFACE);
  style.hover = nk_style_item_color(active ? ACCENT_DIM : SURFACE2);
  style.active = nk_style_item_color(ACCENT_DIM);
  style.text_normal = active ? TEXT : MUTED;
  style.text_hover = style.text_active = TEXT;
  nk_layout_space_push(ctx, nk_rect(x, y, w, h));
  return nk_button_label_styled(ctx, &style, title);
}

static void draw_chrome(struct nk_context *ctx, UiState *ui,
                        const IpcCategoryV1 *categories, int category_count,
                        const int *category_counts,
                        int visible_count, float width, float height) {
  fill(ctx, screen_rect(ctx, 0, 0, width, 52), 0, SURFACE);
  fill(ctx, screen_rect(ctx, 0, 52, 220, height - 52), 0, SURFACE);
  fill(ctx, screen_rect(ctx, 0, 51, width, 1), 0, BORDER);
  fill(ctx, screen_rect(ctx, 219, 52, 1, height - 52), 0, BORDER);
  bold_at(ctx, 18, 0, 142, 52, "CDM", 15, TEXT, SURFACE);
  const char *tabs[] = {tr("gui.all"), tr("gui.downloading"), tr("gui.completed"), tr("gui.queues")};
  float tx = 173;
  const float tw[] = {43, 109, 100, 72};
  for (int i = 0; i < 4; ++i) {
    if (nav_button(ctx, tx, 11, tw[i], 30, tabs[i], ui->tab == (GuiTab)i)) {
      ui->tab = (GuiTab)i;
      if (ui->tab == TAB_QUEUES)
        report_enqueue(ui, gui_controller_request_queues());
    }
    tx += tw[i] + 4;
  }
  float search_width = width - 602;
  if (search_width > 340)
    search_width = 340;
  if (search_width > 90 && ui->tab != TAB_QUEUES) {
    nk_layout_space_push(ctx, nk_rect(width - 122 - search_width, 10,
                                      search_width, 32));
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->search,
                                   GUI_SEARCH_CAP, nk_filter_default);
    if (!ui->search[0])
      text_at(ctx, width - 110 - search_width, 10, search_width - 24, 32,
              tr("gui.search_downloads"), 13, DISABLED, BG);
  }
  struct nk_color connection = ui->connected ? GREEN : RED;
  nk_fill_circle(nk_window_get_canvas(ctx),
                 screen_rect(ctx, width - 98, 23, 7, 7), connection);
  text_at(ctx, width - 84, 0, 80, 52,
          ui->connected ? tr("gui.connected") : tr("gui.disconnected"), 13, connection,
          SURFACE);
  if (button(ctx, 14, 66, 192, 40,
             ui->tab == TAB_QUEUES ? tr("gui.add_queue") : tr("gui.new_download"), true,
             true)) {
    if (ui->tab == TAB_QUEUES) {
      memset(&ui->queue_draft, 0, sizeof(ui->queue_draft));
      copy_text(ui->queue_draft.post_action,
                sizeof(ui->queue_draft.post_action), "none");
      copy_text(ui->queue_priority, sizeof(ui->queue_priority), "0");
      copy_text(ui->queue_cap, sizeof(ui->queue_cap), "0");
      ui->queue_add_open = true;
    } else
      ui->add_open = true;
  }
  if (ui->tab != TAB_QUEUES) {
    text_at(ctx, 16, 115, 140, 28, tr("gui.categories"), 11, MUTED, SURFACE);
    if (button(ctx, 174, 115, 30, 28, "+", true, false)) {
      memset(&ui->category_draft, 0, sizeof(ui->category_draft));
      ui->category_edit_open = true;
    }
    if (nav_button(ctx, 14, 148, 192, 33, tr("gui.all_categories"),
                   ui->category == 0))
      ui->category = 0;
    int slots = (int)((height - 342) / 36);
    if (slots < 1)
      slots = 1;
    if (ui->category_scroll > category_count - slots)
      ui->category_scroll = category_count > slots ? category_count - slots : 0;
    for (int slot = 0; slot < slots && slot + ui->category_scroll < category_count;
         ++slot) {
      int i = slot + ui->category_scroll;
      float y = 184 + slot * 36;
      if (nav_button(ctx, 14, y, 192, 33, "", ui->category == categories[i].id))
        ui->category = categories[i].id;
      struct nk_color bg = ui->category == categories[i].id ? ACCENT_DIM : SURFACE;
      label(ctx, screen_rect(ctx, 24, y, 140, 33), categories[i].name,
            13, ui->category == categories[i].id ? TEXT : MUTED, bg);
      char total[16];
      int written = snprintf(total, sizeof(total), "%d", category_counts[i]);
      if (written < 0 || (size_t)written >= sizeof(total))
        total[0] = '\0';
      text_at(ctx, 176, y, 28, 33, total, 11, TEXT, bg);
      if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_RIGHT,
                                 screen_rect(ctx, 14, y, 192, 33))) {
        ui->category_menu_id = categories[i].id;
        ui->category_menu_open = true;
        ui->category_menu_just_opened = true;
        ui->category_menu_pos = ctx->input.mouse.pos;
      }
    }
    if (category_count > slots) {
      if (button(ctx, 14, height - 145, 92, 27, tr("gui.previous"),
                 ui->category_scroll > 0, false))
        ui->category_scroll--;
      if (button(ctx, 114, height - 145, 92, 27, tr("gui.next"),
                 ui->category_scroll + slots < category_count, false))
        ui->category_scroll++;
    }
  }
  fill(ctx, screen_rect(ctx, 14, height - 83, 192, 1), 0, BORDER);
  text_at(ctx, 14, height - 69, 192, 14, tr("gui.local_storage"), 11, DISABLED,
          SURFACE);
  fill(ctx, screen_rect(ctx, 14, height - 49, 192, 5), 2, BG);
  if (ui->disk_available) {
    double used = 1.0 - (double)ui->disk.free_bytes / ui->disk.total_bytes;
    float bar_width = (float)(192.0 * used);
    if (bar_width > 0 && bar_width < 2)
      bar_width = 2;
    if (bar_width > 0)
      fill(ctx, screen_rect(ctx, 14, height - 49, bar_width, 5), 2, ACCENT);
    char storage[80];
    int written = snprintf(storage, sizeof(storage), "%.1f%% %s  /  %.1f GB %s",
                           used * 100.0, tr("gui.used"),
                           (double)ui->disk.free_bytes / 1000000000.0,
                           tr("gui.free"));
    if (written < 0 || (size_t)written >= sizeof(storage))
      storage[0] = '\0';
    text_at(ctx, 14, height - 38, 192, 16, storage, 11, MUTED, SURFACE);
  } else {
    text_at(ctx, 14, height - 38, 192, 16, tr("gui.storage_usage_unavailable"), 11,
            DISABLED, SURFACE);
  }
  char total[64];
  if (ui->tab == TAB_QUEUES) {
    Queue queues[GUI_MODEL_MAX_QUEUES];
    int count = gui_model_snapshot_queues(queues, GUI_MODEL_MAX_QUEUES);
    int written = snprintf(total, sizeof(total), "%d %s", count,
                           tr("gui.queues_lower"));
    if (written < 0 || (size_t)written >= sizeof(total))
      total[0] = '\0';
  } else if (ui->category == 0 && ui->tab == TAB_ALL &&
             !ui->search[0]) {
    int written = snprintf(total, sizeof(total), "%d %s %u %s", visible_count,
                           tr("gui.of"), ui->history_total,
                           tr("gui.downloads_lower"));
    if (written < 0 || (size_t)written >= sizeof(total))
      total[0] = '\0';
  } else {
    int written = snprintf(total, sizeof(total), "%d %s", visible_count,
                           tr("gui.downloads_lower"));
    if (written < 0 || (size_t)written >= sizeof(total)) total[0] = '\0';
  }
  text_at(ctx, width - 183, 60, 170, 32, total, 12, MUTED, BG);
  fill(ctx, screen_rect(ctx, 220, 100, width - 220, 1), 0, BORDER);
}

static void draw_toolbar(struct nk_context *ctx, UiState *ui, GuiRow *rows,
                         int count) {
  GuiRow *selected = find_row(rows, count, ui->selected_id);
  if (tool_button(ctx, 236, "add", tr("gui.add_download"), true))
    ui->add_open = true;
  if (tool_button(ctx, 272, "cancel", tr("gui.cancel_selected_download"),
                  can_pause(selected) || is_status(selected, "PAUSED")))
    report_enqueue(ui, gui_controller_enqueue_cancel(selected->id));
  fill(ctx, screen_rect(ctx, 314, 66, 1, 20), 0, BORDER);
  if (tool_button(ctx, 325, "pause", tr("gui.pause_selected_download"),
                  can_pause(selected)))
    report_enqueue(ui, gui_controller_enqueue_pause(selected->id));
  if (tool_button(ctx, 361, "resume", tr("gui.resume_selected_download"),
                  is_status(selected, "PAUSED")))
    report_enqueue(ui, gui_controller_enqueue_resume(selected->id));
  fill(ctx, screen_rect(ctx, 403, 66, 1, 20), 0, BORDER);
  if (tool_button(ctx, 414, "settings", tr("gui.settings.title"), true))
    open_settings(ui);
}

static void file_chip(const char *filename, char ext[5],
                      struct nk_color *color) {
  const char *dot = strrchr(filename, '.');
  copy_text(ext, 5, dot && dot[1] ? dot + 1 : "FILE");
  for (char *p = ext; *p; ++p)
    *p = (char)toupper((unsigned char)*p);
  *color = STATUS_BLUE;
  if (!strcmp(ext, "ZIP") || !strcmp(ext, "GZ") || !strcmp(ext, "7Z") ||
      !strcmp(ext, "TAR"))
    *color = STATUS_PURPLE;
  else if (!strcmp(ext, "MP4") || !strcmp(ext, "MKV") || !strcmp(ext, "WEBM") ||
           !strcmp(ext, "MP3"))
    *color = STATUS_ORANGE;
  else if (!strcmp(ext, "EXE") || !strcmp(ext, "APPI"))
    *color = STATUS_GREEN;
  else if (!strcmp(ext, "PNG") || !strcmp(ext, "JPG") || !strcmp(ext, "JPEG"))
    *color = STATUS_GOLD;
}

static void draw_rows(struct nk_context *ctx, UiState *ui, GuiRow *rows,
                      int count, float width, float height) {
  float viewport = height - 101 - (ui->error[0] ? 36 : 0);
  nk_layout_space_push(ctx, nk_rect(220, 101, width - 220,
                                    viewport));
  if (!nk_group_begin(ctx, "downloads", 0))
    return;
  nk_layout_set_min_row_height(ctx, 0);
  nk_layout_row_dynamic(ctx, THEME_ROW_TIGHT_SPACER, 1);
  nk_label(ctx, "", NK_TEXT_LEFT);
  if (!count && !ui->history_loading) {
    nk_layout_row_dynamic(ctx, THEME_ROW_PREVIEW, 1);
    nk_label_colored(ctx, tr("gui.your_download_queue_is_empty"), NK_TEXT_CENTERED,
                     MUTED);
  }
  float content_height = 8;
  for (int i = 0; i < count; ++i) {
    GuiRow *row = &rows[i];
    bool progress = can_pause(row) || is_status(row, "PAUSED");
    float rh = progress ? 69 : 59;
    content_height += rh;
    nk_layout_space_begin(ctx, NK_STATIC, rh, 1);
    struct nk_rect r = screen_rect(ctx, 8, 2, width - 236, rh - 4);
    bool hovered = nk_input_is_mouse_hovering_rect(&ctx->input, r);
    struct nk_color bg = ui->selected_id == row->id ? SURFACE2
                         : hovered                  ? SURFACE
                                                    : BG;
    fill(ctx, r, 8, bg);
    char ext[5];
    struct nk_color chip;
    file_chip(filename_for_row(row), ext, &chip);
    struct nk_rect icon = nk_rect(r.x + 16, r.y + (r.h - 34) / 2, 34, 34);
    fill(ctx, icon, 8, chip);
    const struct nk_user_font *font = bold_fonts[0];
    float ew = font->width(font->userdata, font->height, ext, (int)strlen(ext));
    label_font(ctx, nk_rect(icon.x + (34 - ew) / 2, icon.y, 34, 34), ext,
               bold_fonts[0], theme_contrast_ink(chip), chip);
    float name_width = r.w - 64 - 14 - 64 - 14 - 28 - 16;
    label(ctx, nk_rect(r.x + 64, r.y + 10, name_width, 16),
          filename_for_row(row), 13, TEXT, bg);
    char meta[IPC_MAX_PATH_LEN + 32];
    int written = 0;
    if (is_status(row, "ERROR") && row->error[0])
      copy_text(meta, sizeof(meta), row->error);
    else if (is_status(row, "DONE"))
      copy_text(meta, sizeof(meta), tr("gui.completed"));
    else if (progress && row->has_v2) {
      char received[32], total[32], speed[32], eta[32];
      gui_format_bytes(row->bytes_received, received, sizeof(received));
      if (row->total_bytes)
        gui_format_bytes(row->total_bytes, total, sizeof(total));
      else
        copy_text(total, sizeof(total), "?");
      gui_format_bytes(row->speed_bps, speed, sizeof(speed));
      gui_format_eta(row->eta_seconds, eta, sizeof(eta));
      if (is_status(row, "PAUSED"))
        written = snprintf(meta, sizeof(meta), "%s / %s  ·  %s", received,
                           total, tr("gui.paused"));
      else
        written = snprintf(meta, sizeof(meta), "%s / %s  ·  %s/s  ·  %s %s",
                           received, total, speed, tr("gui.eta"), eta);
    } else if (progress)
      written = snprintf(meta, sizeof(meta), "%.0f%%  /  %s",
                         row->progress * 100,
                         is_status(row, "PAUSED") ? tr("gui.paused")
                                                  : row->dest_path);
    else
      written = snprintf(meta, sizeof(meta), "%s  /  %s", row->status,
                         row->dest_path);
    if (written < 0 || (size_t)written >= sizeof(meta))
      meta[0] = '\0';
    label(ctx, nk_rect(r.x + 64, r.y + 29, name_width, 14), meta, 11, MUTED,
          bg);
    if (progress) {
      float pct = row->progress < 0 ? 0 : row->progress > 1 ? 1 : row->progress;
      fill(ctx, nk_rect(r.x + 64, r.y + 48, 150, 5), 2, BG);
      if (pct > 0)
        fill(ctx, nk_rect(r.x + 64, r.y + 48, 150 * pct, 5), 2,
             is_status(row, "PAUSED") ? BORDER : ACCENT);
    }
    struct nk_color pill =
        is_status(row, "DONE")                                    ? GREEN
        : is_status(row, "PAUSED")                                ? AMBER
        : (is_status(row, "ERROR") || is_status(row, "CANCELED")) ? RED
                                                                  : ACCENT;
    struct nk_color ink =
        is_status(row, "DONE") ? STATUS_DONE
        : (is_status(row, "ERROR") || is_status(row, "CANCELED"))
            ? STATUS_ERROR
            : pill;
    struct nk_color tint = nk_rgb((int)(pill.r * .18f + bg.r * .82f),
                                  (int)(pill.g * .18f + bg.g * .82f),
                                  (int)(pill.b * .18f + bg.b * .82f));
    float pw = fonts[0]->width(fonts[0]->userdata, fonts[0]->height,
                               row->status, (int)strlen(row->status)) +
               20;
    if (pw < 64)
      pw = 64;
    struct nk_rect pr =
        nk_rect(r.x + r.w - 16 - 28 - 14 - pw, r.y + (r.h - 23) / 2, pw, 23);
    fill(ctx, pr, 11, tint);
    float sw = fonts[0]->width(fonts[0]->userdata, fonts[0]->height,
                               row->status, (int)strlen(row->status));
    label_font(ctx, nk_rect(pr.x + (pw - sw) / 2, pr.y, pw, 23), row->status,
               bold_fonts[0], ink, tint);
    float mx = width - 236 - 28 - 8;
    struct nk_rect mr = screen_rect(ctx, mx, (rh - 28) / 2, 28, 28);
    struct nk_style_button more = ctx->style.button;
    more.normal = nk_style_item_color(bg);
    more.hover = nk_style_item_color(BORDER);
    nk_layout_space_push(ctx, nk_rect(mx, (rh - 28) / 2, 28, 28));
    bool clicked = nk_button_label_styled(ctx, &more, "");
    for (int d = -1; d <= 1; ++d)
      nk_fill_circle(nk_window_get_canvas(ctx),
                     nk_rect(mr.x + 13, mr.y + 13 + d * 5, 2, 2), MUTED);
    bool interactive = !ui->settings_open && !ui->add_open &&
                       !ui->details_open && !ui->delete_confirm_open &&
                       !ui->menu_id;
    if (interactive &&
        (clicked || nk_input_mouse_clicked(&ctx->input, NK_BUTTON_RIGHT, r))) {
      ui->selected_id = row->id;
      ui->menu_id = ui->menu_id == row->id ? 0 : row->id;
      ui->menu_pos = nk_vec2(r.x + r.w - 184, r.y + 44);
      ui->menu_just_opened = true;
    } else if (interactive &&
               nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, r)) {
      ui->selected_id = row->id;
    }
    nk_layout_space_end(ctx);
  }
  bool all_history = ui->category == 0 && ui->tab == TAB_ALL &&
                     ui->search[0] == '\0';
  if (all_history && ui->history_loading) {
    nk_layout_row_dynamic(ctx, THEME_ROW_SECTION, 1);
    nk_label_colored(ctx, tr("gui.loading_more_downloads"), NK_TEXT_CENTERED,
                     MUTED);
    content_height += 28;
  }
  nk_group_end(ctx);
  nk_uint x_scroll = 0, y_scroll = 0;
  nk_group_get_scroll(ctx, "downloads", &x_scroll, &y_scroll);
  if (ui->pending_scroll_adjust) {
    y_scroll = y_scroll > ui->pending_scroll_adjust
                   ? y_scroll - ui->pending_scroll_adjust : 0;
    nk_group_set_scroll(ctx, "downloads", x_scroll, y_scroll);
    ui->pending_scroll_adjust = 0;
  }
  if (all_history && !ui->history_loading &&
      (uint64_t)ui->history_offset + (uint32_t)count < ui->history_total &&
      (float)y_scroll + viewport + 220 >= content_height &&
      gui_controller_request_more())
    ui->history_loading = true;
}

static void show_details(UiState *ui, uint32_t id) {
  ui->selected_id = id;
  ui->details_found = false;
  ui->details_open = true;
  report_enqueue(ui, gui_controller_enqueue_details(id));
}

static void draw_menu(struct nk_context *ctx, UiState *ui, GuiRow *rows,
                      int count, float width, float height) {
  GuiRow *row = find_row(rows, count, ui->menu_id);
  if (!row) {
    ui->menu_id = 0;
    return;
  }
  bool done = is_status(row, "DONE");
  bool paused = is_status(row, "PAUSED");
  float mh = 266;
  float x = ui->menu_pos.x, y = ui->menu_pos.y;
  if (x + 180 > width)
    x = width - 180;
  if (y + mh > height)
    y = height - mh - 8;
  struct nk_rect bounds = nk_rect(x, y, 180, mh);
  bool just_opened = ui->menu_just_opened;
  ui->menu_just_opened = false;
  if (!just_opened && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
      !nk_input_is_mouse_hovering_rect(&ctx->input, bounds)) {
    ui->menu_id = 0;
    return;
  }
  if (nk_popup_begin(ctx, NK_POPUP_STATIC, "download-menu",
                     NK_WINDOW_NO_SCROLLBAR, nk_rect(x, y, 180, mh))) {
    nk_layout_space_begin(ctx, NK_STATIC, mh, 12);
    fill(ctx, screen_rect(ctx, 0, 0, 180, mh), 8, SURFACE2);
    outline(ctx, screen_rect(ctx, .5f, .5f, 179, mh - 1), 8);
    float by = 5;
    bool close = false;
    if (done) {
      if (button(ctx, 5, by, 170, 30, tr("gui.open"), true, false)) {
        open_path(ui, row->dest_path, false);
        close = true;
      }
    } else if (can_pause(row)) {
      if (button(ctx, 5, by, 170, 30, tr("gui.pause"), true, false)) {
        report_enqueue(ui, gui_controller_enqueue_pause(row->id));
        close = true;
      }
    } else if (paused) {
      if (button(ctx, 5, by, 170, 30, tr("gui.resume"), true, false)) {
        report_enqueue(ui, gui_controller_enqueue_resume(row->id));
        close = true;
      }
    } else {
      /* ERROR rows cannot prove their partial file is still valid from GuiRow. */
      if (button(ctx, 5, by, 170, 30, tr("gui.re_download"), true, false)) {
        report_enqueue(
            ui, gui_controller_enqueue_add(row->url, row->dest_path, NULL));
        close = true;
      }
    }
    by += 30;
    if (button(ctx, 5, by, 170, 30, tr("gui.open_folder"), true, false)) {
      open_path(ui, row->dest_path, true);
      close = true;
    }
    by += 30;
    if (done) {
      if (button(ctx, 5, by, 170, 30, tr("gui.re_download"), true, false)) {
        report_enqueue(
            ui, gui_controller_enqueue_add(row->url, row->dest_path, NULL));
        close = true;
      }
    } else {
      if (button(ctx, 5, by, 170, 30, tr("gui.copy_url"), true, false)) {
        if (SDL_SetClipboardText(row->url) != 0) {
          int written = snprintf(ui->error, sizeof(ui->error), "%s: %.200s",
                                 tr("gui.could_not_copy_url"), SDL_GetError());
          if (written < 0 || (size_t)written >= sizeof(ui->error))
            ui->error[0] = '\0';
        }
        close = true;
      }
    }
    by += 30;
    if (button(ctx, 5, by, 170, 30, tr("gui.show_details"), true, false)) {
      show_details(ui, row->id);
      close = true;
    }
    by += 30;
    if (button(ctx, 5, by, 170, 30, tr("gui.refresh_url"),
               !is_status(row, "ACTIVE"), false)) {
      report_enqueue(ui, gui_controller_enqueue_refresh_url(row->id));
      close = true;
    }
    by += 36;
    fill(ctx, screen_rect(ctx, 7, by - 3, 166, 1), 0, BORDER);
    if (can_pause(row) || paused) {
      if (button(ctx, 5, by, 170, 30, tr("gui.cancel"), true, false)) {
        report_enqueue(ui, gui_controller_enqueue_cancel(row->id));
        close = true;
      }
    }
    by += 32;
    bool removable = !is_status(row, "ACTIVE");
    if (button(ctx, 5, by, 170, 30, tr("gui.remove_from_list"), removable, false)) {
      report_enqueue(ui, gui_controller_enqueue_remove(row->id, false));
      close = true;
    }
    by += 30;
    if (button(ctx, 5, by, 170, 30, tr("gui.delete_file"), removable, false)) {
      ui->delete_confirm_id = row->id;
      copy_text(ui->delete_confirm_path, sizeof(ui->delete_confirm_path),
                row->dest_path);
      ui->delete_confirm_open = true;
      close = true;
    }
    nk_layout_space_end(ctx);
    if (close) {
      nk_popup_close(ctx);
      ui->menu_id = 0;
    }
    nk_popup_end(ctx);
  } else
    ui->menu_id = 0;
}

static void number_field(struct nk_context *ctx, const char *label_text,
                         char *buffer) {
  nk_layout_row_dynamic(ctx, THEME_ROW_TEXT, 1);
  nk_label(ctx, label_text, NK_TEXT_LEFT);
  nk_layout_row_dynamic(ctx, THEME_ROW_DETAIL, 1);
  nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, buffer, 16,
                                 nk_filter_decimal);
  nk_layout_row_dynamic(ctx, THEME_ROW_WIDE_SPACER, 1);
  nk_label(ctx, "", NK_TEXT_LEFT);
}

static bool parse_number(const char *text, int min, int max, int *out) {
  char *end;
  long value = strtol(text, &end, 10);
  if (!text[0] || *end || value < min || value > max)
    return false;
  *out = (int)value;
  return true;
}

static bool close_button(struct nk_context *ctx, float x, float y) {
  bool clicked = button(ctx, x, y, 28, 28, "", true, false);
  struct nk_rect r = screen_rect(ctx, x, y, 28, 28);
  struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
  nk_stroke_line(canvas, r.x + 10, r.y + 10, r.x + 18, r.y + 18, 1.5f, MUTED);
  nk_stroke_line(canvas, r.x + 18, r.y + 10, r.x + 10, r.y + 18, 1.5f, MUTED);
  return clicked;
}

static void section(struct nk_context *ctx, const char *text) {
  nk_layout_row_dynamic(ctx, THEME_ROW_SECTION, 1);
  nk_style_push_font(ctx, fonts[0]);
  nk_label_colored(ctx, text, NK_TEXT_LEFT, MUTED);
  nk_style_pop_font(ctx);
}

static void modal_backdrop(struct nk_context *ctx, float width, float height) {
  nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                           nk_style_item_color(MODAL_CLEAR));
  if (nk_begin(ctx, "modal-backdrop", nk_rect(0, 0, width, height),
               NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT))
    fill(ctx, nk_rect(0, 0, width, height), 0, MODAL_SHADE);
  nk_end(ctx);
  nk_style_pop_style_item(ctx);
}

static bool modal_start(struct nk_context *ctx, const char *id,
                        const char *title, float x, float y, float w, float h,
                        bool *open) {
  bool visible = nk_begin(ctx, id, nk_rect(x, y, w, h), NK_WINDOW_NO_SCROLLBAR);
  if (!visible)
    return false;
  nk_layout_space_begin(ctx, NK_STATIC, h, 20);
  fill(ctx, screen_rect(ctx, 0, 0, w, h), 12, SURFACE);
  outline(ctx, screen_rect(ctx, .5f, .5f, w - 1, h - 1), 12);
  bold_at(ctx, 20, 12, w - 70, 36, title, 15, TEXT, SURFACE);
  if (close_button(ctx, w - 48, 16))
    *open = false;
  fill(ctx, screen_rect(ctx, 0, 60, w, 1), 0, BORDER);
  return true;
}

static void modal_end(struct nk_context *ctx) {
  nk_layout_space_end(ctx);
  nk_end(ctx);
}

static void settings_transfer(UiState *ui, bool importing, bool replace) {
  const char *filters[] = {"*.json"};
  char *chosen = importing
      ? tinyfd_openFileDialog(tr("gui.import_cdm_json"), NULL, 1, filters,
                              tr("gui.json_files"), 0)
      : tinyfd_saveFileDialog(tr("gui.export_cdm_json"), "cdm-export.json", 1,
                              filters, tr("gui.json_files"));
  if (!chosen)
    return;
  char *source = importing ? realpath(chosen, NULL) : NULL;
  if (importing && !source) {
    copy_text(ui->settings_message, sizeof(ui->settings_message),
              tr("gui.could_not_open_the_selected_import_file"));
    tinyfd_notifyPopup("cdm import", ui->settings_message, "error");
    return;
  }
  if (replace && !tinyfd_messageBox(tr("gui.replace_cdm_data"),
          tr("gui.replace_local_history_and_settings_a_database_backup_will_be_made"),
          "yesno", "warning", 0)) {
    free(source);
    return;
  }
  uint16_t version = 1;
  int sock = ipc_client_connect_compatible(-1, &version);
  bool ok = false;
  if (sock >= 0) {
    if (importing) {
      IpcResult result = IPC_RESULT_ERROR;
      ok = version >= 12 &&
           ipc_send_import_json_v1(sock, source, replace, &result) == 0 &&
           result == IPC_RESULT_OK;
    } else
      ok = export_json_file(sock, version, chosen, true, false) == 0;
    ipc_client_disconnect(sock);
  }
  free(source);
  if (ok && importing && replace) {
    config_init(NULL);
    open_settings(ui);
  }
  const char *message = ok ? (importing ? tr("gui.import_completed") : tr("gui.export_completed"))
                           : (importing ? tr("gui.import_failed") : tr("gui.export_failed"));
  copy_text(ui->settings_message, sizeof(ui->settings_message), message);
  tinyfd_notifyPopup("cdm", message, ok ? "info" : "error");
}

static void draw_settings(struct nk_context *ctx, UiState *ui, float width,
                          float height) {
  if (ui->theme_needs_apply) {
    theme_apply(ctx, ui->theme_preview);
    ui->theme_needs_apply = false;
  }
  float h = height * .8f;
  if (h > 650)
    h = 650;
  float w = 460, x = (width - w) / 2, y = (height - h) / 2;
  if (!modal_start(ctx, "settings", tr("gui.settings.title"), x, y, w, h,
                   &ui->settings_open)) {
    nk_end(ctx);
    return;
  }
  text_at(ctx, 20, 70, w - 40, 20,
          tr("gui.settings_save_note"), 11, MUTED, SURFACE);
  bool proxy_url_ok = true;
  nk_layout_space_push(ctx, nk_rect(20, 108, w - 40, h - 188));
  nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                           nk_style_item_color(SURFACE));
  if (nk_group_begin(ctx, "settings-fields", 0)) {
    section(ctx, tr("gui.general"));
    nk_bool monitor = ui->clipboard_monitor;
    nk_layout_row_dynamic(ctx, THEME_ROW_SECTION, 1);
    nk_checkbox_label(ctx, tr("gui.monitor_clipboard_for_urls"), &monitor);
    ui->clipboard_monitor = monitor;
    nk_layout_row_dynamic(ctx, THEME_ROW_CAPTION, 1);
    nk_label_colored(ctx, tr("gui.ask_before_adding_a_copied_url"), NK_TEXT_LEFT,
                     MUTED);
    nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
    nk_label(ctx, tr("gui.theme"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, THEME_ROW_INPUT, 1);
    const char *themes[] = {tr("gui.system"), tr("gui.light"), tr("gui.dark")};
    ThemeId selection = (ThemeId)nk_combo(ctx, themes, 3, ui->theme_preview,
                                          30, nk_vec2(400, 110));
    if (selection != ui->theme_preview) {
      ui->theme_preview = selection;
      theme_apply(ctx, selection);
    }
    nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
    nk_label(ctx, tr("gui.locale"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, THEME_ROW_INPUT, 1);
    const char *locales[] = {tr("gui.locale_english"),
                             tr("gui.locale_spanish")};
    ui->locale_selection = nk_combo(ctx, locales, 2, ui->locale_selection,
                                    30, nk_vec2(400, 90));
    nk_layout_row_dynamic(ctx, THEME_ROW_CAPTION, 1);
    nk_label_colored(ctx, tr("gui.locale_restart_required"), NK_TEXT_LEFT,
                     MUTED);
    nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
    nk_label(ctx, tr("gui.default_download_directory"), NK_TEXT_LEFT);
    nk_layout_row_begin(ctx, NK_STATIC, THEME_ROW_INPUT, 3);
    nk_layout_row_push(ctx, 312);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->directory,
                                   sizeof(ui->directory), NULL);
    nk_layout_row_push(ctx, THEME_COLUMN_GAP);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, 90);
    if (nk_button_label(ctx, tr("gui.browse"))) {
      char *chosen = tinyfd_selectFolderDialog(tr("gui.default_download_directory"),
                                               ui->directory);
      if (chosen)
        copy_text(ui->directory, sizeof(ui->directory), chosen);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, THEME_ROW_WIDE_SPACER, 1);
    nk_label(ctx, "", NK_TEXT_LEFT);
    number_field(ctx, tr("gui.maximum_concurrent_downloads"), ui->numbers[0]);
    section(ctx, tr("gui.connections"));
    number_field(ctx, tr("gui.connections_per_download_1_16"), ui->numbers[6]);
    number_field(ctx, tr("gui.connect_timeout_seconds"), ui->numbers[7]);
    number_field(ctx, tr("gui.transfer_timeout_seconds"), ui->numbers[8]);
    nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
    nk_label(ctx, tr("gui.user_agent"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, THEME_ROW_INPUT, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->user_agent,
                                   sizeof(ui->user_agent), NULL);
    if (!ui->user_agent[0]) {
      DownloadManagerConfig current;
      config_get(&current);
      nk_layout_row_dynamic(ctx, THEME_ROW_CAPTION, 1);
      nk_labelf_colored(ctx, NK_TEXT_LEFT, MUTED, "%s: %s", tr("gui.current"),
                        current.user_agent);
    }
    section(ctx, tr("gui.retries"));
    number_field(ctx, tr("gui.maximum_retry_attempts"), ui->numbers[1]);
    number_field(ctx, tr("gui.retry_base_delay_seconds"), ui->numbers[2]);
    number_field(ctx, tr("gui.retry_maximum_delay_seconds"), ui->numbers[3]);
    section(ctx, tr("gui.bandwidth"));
    number_field(ctx, tr("gui.global_speed_limit_bytes_sec_0_unlimited"),
                 ui->numbers[4]);
    nk_layout_row_dynamic(ctx, THEME_ROW_CAPTION, 1);
    nk_label_colored(ctx, "0 disables the limit entirely.", NK_TEXT_LEFT,
                     MUTED);
    section(ctx, tr("gui.proxy"));
    nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
    nk_label(ctx, tr("gui.mode"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, THEME_ROW_INPUT, 1);
    const char *modes[] = {tr("gui.none"), "HTTP", "SOCKS5"};
    ui->proxy_mode = (ProxyMode)nk_combo(ctx, modes, 3, ui->proxy_mode, 30,
                                         nk_vec2(400, 110));
    nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
    nk_label(ctx, tr("gui.proxy_url"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, THEME_ROW_INPUT, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->proxy_url,
                                   sizeof(ui->proxy_url), NULL);
    const char *scheme = strstr(ui->proxy_url, "://");
    proxy_url_ok = ui->proxy_mode == PROXY_NONE ||
                   (scheme && scheme != ui->proxy_url && scheme[3] &&
                    scheme[3] != ':' && scheme[3] != '/');
    if (!proxy_url_ok) {
      nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
      nk_label_colored(ctx, tr("gui.enter_a_proxy_url_with_scheme_and_host"),
                       NK_TEXT_LEFT, RED);
    }
    nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
    nk_label(ctx, tr("gui.username"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, THEME_ROW_INPUT, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->proxy_username,
                                   sizeof(ui->proxy_username), NULL);
    nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
    nk_label(ctx, tr("gui.password"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, THEME_ROW_INPUT, 2);
    if (nk_button_label(ctx, ui->proxy_password[0] ? tr("gui.change_password")
                                                  : tr("gui.set_password"))) {
      char *value = tinyfd_inputBox(tr("gui.proxy_password"), tr("gui.enter_proxy_password"),
                                   NULL); /* NULL requests a masked input box. */
      if (value)
        copy_text(ui->proxy_password, sizeof(ui->proxy_password), value);
    }
    if (nk_button_label(ctx, tr("gui.clear_password")))
      ui->proxy_password[0] = '\0';
    section(ctx, tr("gui.import_export"));
    nk_layout_space_begin(ctx, NK_STATIC, THEME_ROW_INPUT, 1);
    if (button(ctx, 0, 0, 390, 32, tr("gui.export_settings_and_history"), true, false))
      settings_transfer(ui, false, false);
    nk_layout_space_end(ctx);
    nk_layout_space_begin(ctx, NK_STATIC, THEME_ROW_INPUT, 2);
    if (button(ctx, 0, 0, 190, 32, tr("gui.import_missing"), true, false))
      settings_transfer(ui, true, false);
    if (button(ctx, 200, 0, 190, 32, tr("gui.replace_from_file"), true, false))
      settings_transfer(ui, true, true);
    nk_layout_space_end(ctx);
    nk_group_end(ctx);
  }
  nk_style_pop_style_item(ctx);
  text_at(ctx, 20, h - 80, w - 40, 24, ui->settings_message, 11,
          STATUS_ERROR, SURFACE);
  fill(ctx, screen_rect(ctx, 0, h - 60, w, 1), 0, BORDER);
  if (button(ctx, w - 232, h - 46, 80, 32, tr("gui.cancel"), true, false))
    ui->settings_open = false;
  if (button(ctx, w - 144, h - 46, 124, 32, tr("gui.save_settings"), true, true)) {
    bool valid = parse_number(ui->numbers[0], 1, 64, &ui->concurrent) &&
                 parse_number(ui->numbers[1], 0, 100, &ui->attempts) &&
                 parse_number(ui->numbers[2], 1, 3600, &ui->base_delay) &&
                 parse_number(ui->numbers[3], 1, 86400, &ui->max_delay) &&
                 parse_number(ui->numbers[4], 0, 1000000000, &ui->global_speed) &&
                 parse_number(ui->numbers[6], 1, 16, &ui->max_connections) &&
                 parse_number(ui->numbers[7], 1, 600, &ui->connect_timeout) &&
                 parse_number(ui->numbers[8], 1, 3600, &ui->transfer_timeout);
    DownloadManagerConfig config;
    config_get(&config);
    config.default_download_dir = ui->directory;
    config.max_concurrent_downloads = ui->concurrent;
    config.retry_max_attempts = ui->attempts;
    config.retry_base_delay_sec = ui->base_delay;
    config.retry_max_delay_sec = ui->max_delay;
    config.max_speed_bytes_per_sec = (uint64_t)ui->global_speed;
    config.max_connections_per_download = ui->max_connections;
    config.connect_timeout_sec = ui->connect_timeout;
    config.transfer_timeout_sec = ui->transfer_timeout;
    config.clipboard_monitor = ui->clipboard_monitor;
    memcpy(config.ui_theme, theme_to_text(ui->theme_preview),
           strlen(theme_to_text(ui->theme_preview)) + 1);
    memcpy(config.ui_locale, ui->locale_selection ? "es" : "en", 3);
    if (ui->user_agent[0])
      copy_text(config.user_agent, sizeof(config.user_agent), ui->user_agent);
    config.proxy_mode = ui->proxy_mode;
    copy_text(config.proxy_url, sizeof(config.proxy_url), ui->proxy_url);
    copy_text(config.proxy_username, sizeof(config.proxy_username),
              ui->proxy_username);
    copy_text(config.proxy_password, sizeof(config.proxy_password),
              ui->proxy_password);
    if (!valid)
      copy_text(ui->settings_message, sizeof(ui->settings_message),
                tr("gui.invalid_number_check_the_allowed_range_in_each_field"));
    else if (!proxy_url_ok)
      copy_text(ui->settings_message, sizeof(ui->settings_message),
                tr("gui.proxy_url_needs_a_scheme_and_host"));
    else if (config_save(&config)) {
      ui->theme_saved = ui->theme_preview;
      if (!gui_client_reload_config()) {
        copy_text(ui->settings_message, sizeof(ui->settings_message),
                  tr("gui.settings_saved_but_the_daemon_did_not_reload_them"));
      } else {
        ui->clipboard_monitor_enabled = config.clipboard_monitor;
        clipboard_baseline(ui);
        ui->clipboard_offer_open = false;
        copy_text(ui->folder, sizeof(ui->folder), ui->directory);
        ui->disk_checked_at = UINT32_MAX;
        ui->folder_explicit = false;
        ui->settings_open = false;
      }
    } else
      copy_text(ui->settings_message, sizeof(ui->settings_message),
                tr("gui.could_not_save_or_apply_settings"));
  }
  modal_end(ctx);
}

static void input_field(struct nk_context *ctx, const char *name, char *buffer,
                        int size) {
  nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
  nk_label(ctx, name, NK_TEXT_LEFT);
  nk_layout_row_dynamic(ctx, THEME_ROW_INPUT, 1);
  nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, buffer, size, NULL);
  nk_layout_row_dynamic(ctx, THEME_ROW_SPACER, 1);
  nk_label(ctx, "", NK_TEXT_LEFT);
}

static void draw_add(struct nk_context *ctx, UiState *ui, float width,
                     float height) {
  float w = 520, h = ui->advanced ? height * .85f : 350;
  if (h > 700)
    h = 700;
  if (!modal_start(ctx, "add-download", tr("gui.new_download_title"), (width - w) / 2,
                   (height - h) / 2, w, h, &ui->add_open)) {
    nk_end(ctx);
    return;
  }
  nk_layout_space_push(ctx, nk_rect(20, 72, w - 40, h - 146));
  nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                           nk_style_item_color(SURFACE));
  if (nk_group_begin(ctx, "add-fields", 0)) {
    input_field(ctx, tr("gui.url"), ui->url, sizeof(ui->url));
    nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
    nk_label(ctx, tr("gui.save_to"), NK_TEXT_LEFT);
    nk_layout_row_begin(ctx, NK_STATIC, THEME_ROW_INPUT, 3);
    nk_layout_row_push(ctx, 342);
    if (nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->folder,
                                       sizeof(ui->folder), NULL) & NK_EDIT_ACTIVE)
      ui->folder_explicit = true;
    nk_layout_row_push(ctx, THEME_COLUMN_GAP);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, 120);
    if (nk_button_label(ctx, tr("gui.choose_folder"))) {
      char *chosen =
          tinyfd_selectFolderDialog(tr("gui.choose_download_folder"), ui->folder);
      if (chosen)
        copy_text(ui->folder, sizeof(ui->folder), chosen);
      if (chosen)
        ui->folder_explicit = true;
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, THEME_ROW_WIDE_SPACER, 1);
    nk_label(ctx, "", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, THEME_ROW_SECTION, 1);
    if (nk_button_label(ctx, ui->advanced ? tr("gui.hide_advanced_options")
                                          : tr("gui.show_advanced_options")))
      ui->advanced = !ui->advanced;
    if (ui->advanced) {
      input_field(ctx, tr("gui.cookie"), ui->cookie, sizeof(ui->cookie));
      input_field(ctx, tr("gui.referrer"), ui->referrer, sizeof(ui->referrer));
      input_field(ctx, tr("gui.extra_headers"), ui->headers, sizeof(ui->headers));
      input_field(ctx, tr("gui.sha_256"), ui->sha256, sizeof(ui->sha256));
      Queue queues[GUI_MODEL_MAX_QUEUES];
      const char *names[GUI_MODEL_MAX_QUEUES];
      int queue_count = gui_model_snapshot_queues(queues, GUI_MODEL_MAX_QUEUES);
      int selected = 0;
      for (int i = 0; i < queue_count; ++i) {
        names[i] = queues[i].name;
        if (queues[i].id == (ui->add_queue_id ? ui->add_queue_id : 1))
          selected = i;
      }
      if (queue_count > 0) {
        nk_layout_row_dynamic(ctx, THEME_ROW_TEXT, 1);
        nk_label(ctx, tr("gui.queue"), NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, THEME_ROW_BUTTON, 1);
        selected = nk_combo(ctx, names, queue_count, selected, 30,
                            nk_vec2(260, 240));
        ui->add_queue_id = queues[selected].id;
      }
      number_field(ctx, tr("gui.speed_limit_bytes_sec_0_unlimited"),
                   ui->numbers[5]);
    }
    nk_group_end(ctx);
  }
  nk_style_pop_style_item(ctx);
  text_at(ctx, 20, h - 76, w - 40, 22, ui->error, 11, STATUS_ERROR,
          SURFACE);
  fill(ctx, screen_rect(ctx, 0, h - 54, w, 1), 0, BORDER);
  if (button(ctx, 20, h - 41, 200, 30, tr("gui.paste_multiple_urls"), true,
             false)) {
    ui->add_open = false;
    ui->batch_add_open = true;
    ui->error[0] = '\0';
  }
  if (button(ctx, w - 232, h - 41, 80, 30, tr("gui.cancel"), true, false))
    ui->add_open = false;
  if (button(ctx, w - 144, h - 41, 124, 30, tr("gui.add_download"), true, true)) {
    if (!ui->url[0] || !ui->folder[0])
      copy_text(ui->error, sizeof(ui->error),
                tr("gui.enter_a_url_and_destination_folder"));
    else if (!parse_number(ui->numbers[5], 0, 1000000000, &ui->speed_limit))
      copy_text(ui->error, sizeof(ui->error),
                tr("gui.speed_limit_must_be_between_0_and_1000000000"));
    else if (add_download(ui->url, ui->folder, ui->cookie, ui->referrer,
                          ui->headers, ui->sha256, (uint64_t)ui->speed_limit,
                          ui->add_queue_id, !ui->folder_explicit)) {
      ui->add_open = false;
      ui->error[0] = '\0';
      ui->url[0] = ui->cookie[0] = ui->referrer[0] = '\0';
      copy_text(ui->folder, sizeof(ui->folder),
                config_get_default_download_dir());
      ui->folder_explicit = false;
      ui->headers[0] = ui->sha256[0] = '\0';
      ui->speed_limit = 0;
      ui->add_queue_id = 0;
      copy_text(ui->numbers[5], sizeof(ui->numbers[5]), "0");
      ui->advanced = false;
    } else
      copy_text(ui->error, sizeof(ui->error),
                tr("gui.could_not_prepare_or_enqueue_the_download"));
  }
  modal_end(ctx);
}

static void draw_batch_add(struct nk_context *ctx, UiState *ui, float width,
                           float height) {
  float w = 520, h = 550;
  if (!modal_start(ctx, "batch-add", tr("gui.paste_multiple_urls"),
                   (width - w) / 2, (height - h) / 2, w, h,
                   &ui->batch_add_open)) {
    nk_end(ctx);
    return;
  }
  text_at(ctx, 20, 68, w - 40, 20,
          tr("gui.one_http_s_url_per_line_blank_and_lines_are_ignored"),
          11, MUTED, SURFACE);
  nk_layout_space_push(ctx, nk_rect(20, 98, w - 40, 260));
  nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX, ui->batch_urls,
                                 sizeof(ui->batch_urls), NULL);
  text_at(ctx, 20, 368, w - 40, 20, tr("gui.save_to"), 11, MUTED, SURFACE);
  nk_layout_space_push(ctx, nk_rect(20, 390, w - 40, 32));
  if (nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->folder,
                                     sizeof(ui->folder), NULL) & NK_EDIT_ACTIVE)
    ui->folder_explicit = true;
  text_at(ctx, 20, h - 90, w - 40, 22, ui->error, 11, RED, SURFACE);
  fill(ctx, screen_rect(ctx, 0, h - 54, w, 1), 0, BORDER);
  if (button(ctx, w - 232, h - 41, 80, 30, tr("gui.cancel"), true, false))
    ui->batch_add_open = false;
  if (button(ctx, w - 144, h - 41, 124, 30, tr("gui.add_all"), true, true)) {
    size_t consumed = 0;
    int added = 0;
    bool failed = false;
    size_t length = strlen(ui->batch_urls);
    while (consumed < length) {
      size_t line_end = consumed;
      while (line_end < length && ui->batch_urls[line_end] != '\n')
        line_end++;
      size_t next = line_end < length ? line_end + 1 : line_end;
      char line[GUI_URL_CAP];
      size_t segment_length = line_end - consumed;
      if (segment_length >= sizeof(line)) {
        memmove(ui->batch_urls, ui->batch_urls + consumed,
                length - consumed + 1);
        failed = true;
        break;
      }
      memcpy(line, ui->batch_urls + consumed, segment_length);
      line[segment_length] = '\0';
      char *url = line;
      while (isspace((unsigned char)*url))
        url++;
      size_t url_length = strlen(url);
      while (url_length && isspace((unsigned char)url[url_length - 1]))
        url[--url_length] = '\0';
      if (*url && *url != '#') {
        if (!clipboard_url_valid(url) || !ui->folder[0] ||
            !add_download(url, ui->folder, "", "", "", "", 0, 0,
                          !ui->folder_explicit)) {
          memmove(ui->batch_urls, ui->batch_urls + consumed,
                  length - consumed + 1);
          failed = true;
          break;
        }
        added++;
      }
      consumed = next;
    }
    if (failed)
      copy_text(ui->error, sizeof(ui->error),
                tr("gui.invalid_url_destination_or_command_queue_full"));
    else if (!added)
      copy_text(ui->error, sizeof(ui->error), tr("gui.paste_at_least_one_url"));
    else {
      ui->batch_urls[0] = '\0';
      ui->batch_add_open = false;
      ui->error[0] = '\0';
    }
  }
  modal_end(ctx);
}

static bool queue_draft_valid(UiState *ui) {
  int priority = 0, cap = 0;
  if (!ui->queue_draft.name[0] ||
      !parse_number(ui->queue_priority, 0, 1000, &priority) ||
      !parse_number(ui->queue_cap, 0, 64, &cap)) {
    copy_text(ui->error, sizeof(ui->error),
              tr("gui.enter_a_name_priority_0_1000_and_max_concurrent_0_64"));
    return false;
  }
  if (!gui_schedule_valid(ui->queue_draft.schedule_start,
                          ui->queue_draft.schedule_stop)) {
    copy_text(ui->error, sizeof(ui->error),
              tr("gui.enter_two_different_hh_mm_times_or_leave_both_empty"));
    return false;
  }
  if (ui->queue_draft.post_action[0] &&
      strcmp(ui->queue_draft.post_action, "none") != 0 &&
      !config_post_action_enabled(ui->queue_draft.post_action)) {
    copy_text(ui->error, sizeof(ui->error),
              tr("gui.enable_this_post_action_in_post_actions_before_saving"));
    return false;
  }
  if (strcmp(ui->queue_draft.post_action, "command") == 0 &&
      !ui->queue_draft.post_action_arg[0]) {
    copy_text(ui->error, sizeof(ui->error), tr("gui.enter_a_command_to_run"));
    return false;
  }
  ui->error[0] = '\0';
  ui->queue_draft.priority = priority;
  ui->queue_draft.max_concurrent = cap;
  if (!ui->queue_draft.post_action[0])
    copy_text(ui->queue_draft.post_action,
              sizeof(ui->queue_draft.post_action), "none");
  return true;
}

static void queue_fields(struct nk_context *ctx, UiState *ui) {
  input_field(ctx, tr("gui.name"), ui->queue_draft.name,
              sizeof(ui->queue_draft.name));
  number_field(ctx, tr("gui.priority_0_1000"), ui->queue_priority);
  number_field(ctx, tr("gui.max_concurrent_0_unlimited"), ui->queue_cap);
  input_field(ctx, tr("gui.schedule_start_hh_mm"), ui->queue_draft.schedule_start,
              sizeof(ui->queue_draft.schedule_start));
  input_field(ctx, tr("gui.schedule_stop_hh_mm"), ui->queue_draft.schedule_stop,
              sizeof(ui->queue_draft.schedule_stop));
  nk_layout_row_dynamic(ctx, THEME_ROW_CAPTION, 1);
  if (!ui->queue_draft.schedule_start[0] &&
      !ui->queue_draft.schedule_stop[0])
    nk_label_colored(ctx, tr("gui.always"), NK_TEXT_LEFT, MUTED);
  else if (!gui_schedule_valid(ui->queue_draft.schedule_start,
                               ui->queue_draft.schedule_stop))
    nk_label_colored(ctx, tr("gui.use_two_different_hh_mm_times_00_00_23_59"),
                     NK_TEXT_LEFT, RED);
  else
    nk_label_colored(ctx, tr("gui.active_during_this_time_window"), NK_TEXT_LEFT,
                     MUTED);
  static const char *actions[] = {"none", "shutdown", "sleep", "command"};
  const char *labels[] = {tr("gui.none"), tr("gui.shut_down"), tr("gui.sleep"), tr("gui.run_command")};
  int selected = 0;
  for (int i = 1; i < 4; ++i)
    if (strcmp(ui->queue_draft.post_action, actions[i]) == 0)
      selected = i;
  nk_layout_row_dynamic(ctx, THEME_ROW_LABEL, 1);
  nk_label(ctx, tr("gui.post_action"), NK_TEXT_LEFT);
  nk_layout_row_dynamic(ctx, THEME_ROW_INPUT, 1);
  if (nk_combo_begin_label(ctx, labels[selected], nk_vec2(400, 150))) {
    nk_layout_row_dynamic(ctx, THEME_ROW_ACTION, 1);
    for (int i = 0; i < 4; ++i) {
      bool enabled = i == 0 || config_post_action_enabled(actions[i]);
      if (!enabled)
        nk_widget_disable_begin(ctx);
      if (nk_combo_item_label(ctx, labels[i], NK_TEXT_LEFT) && enabled) {
        copy_text(ui->queue_draft.post_action,
                  sizeof(ui->queue_draft.post_action), actions[i]);
        if (i != 3)
          ui->queue_draft.post_action_arg[0] = '\0';
      }
      if (!enabled)
        nk_widget_disable_end(ctx);
    }
    nk_combo_end(ctx);
  }
  nk_layout_row_dynamic(ctx, THEME_ROW_CAPTION, 1);
  if (selected && !config_post_action_enabled(actions[selected]))
    nk_label_colored(ctx,
        tr("gui.enable_this_action_under_post_actions_in_config_toml"),
        NK_TEXT_LEFT, MUTED);
  else
    nk_label_colored(ctx,
        tr("gui.power_and_command_actions_need_explicit_config_enablement"),
        NK_TEXT_LEFT, MUTED);
  if (strcmp(ui->queue_draft.post_action, "command") == 0)
    input_field(ctx, tr("gui.command_executable_and_arguments"),
                ui->queue_draft.post_action_arg,
                sizeof(ui->queue_draft.post_action_arg));
}

static void draw_queue_add(struct nk_context *ctx, UiState *ui, float width,
                           float height) {
  float w = 520, h = height * .82f;
  if (h > 660)
    h = 660;
  if (!modal_start(ctx, "queue-add", tr("gui.add_queue_title"), (width - w) / 2,
                   (height - h) / 2, w, h, &ui->queue_add_open)) {
    nk_end(ctx);
    return;
  }
  nk_layout_space_push(ctx, nk_rect(20, 70, w - 40, h - 140));
  if (nk_group_begin(ctx, "queue-add-fields", 0)) {
    queue_fields(ctx, ui);
    nk_group_end(ctx);
  }
  text_at(ctx, 20, h - 74, w - 40, 20, ui->error, 11, RED, SURFACE);
  fill(ctx, screen_rect(ctx, 0, h - 54, w, 1), 0, BORDER);
  if (button(ctx, w - 230, h - 41, 80, 30, tr("gui.cancel"), true, false))
    ui->queue_add_open = false;
  if (button(ctx, w - 142, h - 41, 122, 30, tr("gui.add_queue_title"), true, true) &&
      queue_draft_valid(ui)) {
    bool queued = gui_controller_enqueue_queue_create(&ui->queue_draft);
    report_enqueue(ui, queued);
    if (queued) {
      ui->queue_add_open = false;
      ui->error[0] = '\0';
    }
  }
  modal_end(ctx);
}

static void draw_queues(struct nk_context *ctx, UiState *ui, float width,
                        float height) {
  Queue queues[GUI_MODEL_MAX_QUEUES];
  int count = gui_model_snapshot_queues(queues, GUI_MODEL_MAX_QUEUES);
  if (count < 0)
    count = 0;
  nk_layout_space_push(ctx, nk_rect(220, 102, width - 220, height - 102));
  if (!nk_group_begin(ctx, "named-queues", 0))
    return;
  float content = width - 236;
  float priority_x = content * .33f, cap_x = content * .44f;
  float schedule_x = content * .56f, action_x = content * .74f;
  nk_layout_space_begin(ctx, NK_STATIC, THEME_ROW_BUTTON, 1);
  text_at(ctx, 14, 0, 48, 30, tr("gui.order"), 11, MUTED, BG);
  text_at(ctx, 68, 0, priority_x - 72, 30, tr("gui.name"), 11, MUTED, BG);
  text_at(ctx, priority_x, 0, cap_x - priority_x, 30, tr("gui.priority"), 11, MUTED,
          BG);
  text_at(ctx, cap_x, 0, schedule_x - cap_x, 30, tr("gui.max"), 11, MUTED, BG);
  text_at(ctx, schedule_x, 0, action_x - schedule_x, 30, tr("gui.schedule"), 11,
          MUTED, BG);
  text_at(ctx, action_x, 0, content - action_x - 120, 30, tr("gui.post_action"), 11,
          MUTED, BG);
  nk_layout_space_end(ctx);
  for (int i = 0; i < count; ++i) {
    Queue *queue = &queues[i];
    nk_layout_space_begin(ctx, NK_STATIC, 56, 1);
    struct nk_rect row = screen_rect(ctx, 8, 1, content, 52);
    fill(ctx, row, 8, SURFACE);
    button(ctx, 14, 12, 44, 30, tr("gui.drag"), true, false);
    struct nk_rect grip = screen_rect(ctx, 14, 12, 44, 30);
    if (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
        nk_input_is_mouse_hovering_rect(&ctx->input, grip))
      ui->queue_drag_id = queue->id;
    if (ui->queue_drag_id && ui->queue_drag_id != queue->id &&
        nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT) &&
        nk_input_is_mouse_hovering_rect(&ctx->input, row)) {
      int from = -1;
      for (int j = 0; j < count; ++j)
        if (queues[j].id == ui->queue_drag_id)
          from = j;
      if (from >= 0) {
        uint32_t ids[GUI_MODEL_MAX_QUEUES];
        for (int j = 0; j < count; ++j)
          ids[j] = queues[j].id;
        uint32_t moved = ids[from];
        if (from < i)
          memmove(&ids[from], &ids[from + 1],
                  (size_t)(i - from) * sizeof(ids[0]));
        else
          memmove(&ids[i + 1], &ids[i],
                  (size_t)(from - i) * sizeof(ids[0]));
        ids[i] = moved;
        report_enqueue(ui, gui_controller_enqueue_queue_order(ids, count));
      }
      ui->queue_drag_id = 0;
    }
    label(ctx, screen_rect(ctx, 68, 10, priority_x - 72, 32), queue->name, 13,
          TEXT, SURFACE);
    char number[32];
    int written = snprintf(number, sizeof(number), "%d", queue->priority);
    if (written < 0 || (size_t)written >= sizeof(number))
      number[0] = '\0';
    label(ctx, screen_rect(ctx, priority_x, 10, cap_x - priority_x, 32),
          number, 12, MUTED, SURFACE);
    written = snprintf(number, sizeof(number), "%d", queue->max_concurrent);
    if (written < 0 || (size_t)written >= sizeof(number))
      number[0] = '\0';
    label(ctx, screen_rect(ctx, cap_x, 10, schedule_x - cap_x, 32), number, 12,
          MUTED, SURFACE);
    char schedule[24];
    if (queue->schedule_start[0] && queue->schedule_stop[0]) {
      written = snprintf(schedule, sizeof(schedule), "%s-%s",
                         queue->schedule_start, queue->schedule_stop);
      if (written < 0 || (size_t)written >= sizeof(schedule))
        schedule[0] = '\0';
    } else
      copy_text(schedule, sizeof(schedule), tr("gui.always"));
    label(ctx, screen_rect(ctx, schedule_x, 10, action_x - schedule_x, 32),
          schedule, 12, MUTED, SURFACE);
    label(ctx, screen_rect(ctx, action_x, 10, content - action_x - 124, 32),
          queue->post_action, 12, MUTED, SURFACE);
    if (button(ctx, content - 112, 12, 48, 30, tr("gui.edit"), true, false)) {
      ui->queue_edit_id = queue->id;
      ui->queue_draft = *queue;
      written = snprintf(ui->queue_priority, sizeof(ui->queue_priority), "%d",
                         queue->priority);
      if (written < 0 || (size_t)written >= sizeof(ui->queue_priority))
        ui->queue_priority[0] = '\0';
      written = snprintf(ui->queue_cap, sizeof(ui->queue_cap), "%d",
                         queue->max_concurrent);
      if (written < 0 || (size_t)written >= sizeof(ui->queue_cap))
        ui->queue_cap[0] = '\0';
    }
    if (queue->id == 1)
      text_at(ctx, content - 53, 12, 44, 30, "-", 12, DISABLED, SURFACE);
    else if (button(ctx, content - 60, 12, 54, 30, tr("gui.delete"), true, false)) {
      ui->queue_delete_id = queue->id;
      ui->queue_delete_open = true;
    }
    nk_layout_space_end(ctx);
    if (ui->queue_edit_id == queue->id) {
      nk_layout_space_begin(ctx, NK_STATIC, 500, 1);
      nk_layout_space_push(ctx, nk_rect(14, 0, content - 28, 490));
      if (nk_group_begin(ctx, "queue-inline-edit", 0)) {
        queue_fields(ctx, ui);
        nk_layout_row_dynamic(ctx, THEME_ROW_BUTTON, 2);
        if (nk_button_label(ctx, tr("gui.cancel")))
          ui->queue_edit_id = 0;
        if (nk_button_label(ctx, tr("gui.save")) && queue_draft_valid(ui)) {
          bool queued = gui_controller_enqueue_queue_update(&ui->queue_draft);
          report_enqueue(ui, queued);
          if (queued)
            ui->queue_edit_id = 0;
        }
        nk_group_end(ctx);
      }
      nk_layout_space_end(ctx);
    }
  }
  if (nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT))
    ui->queue_drag_id = 0;
  nk_group_end(ctx);
}

static void draw_queue_delete(struct nk_context *ctx, UiState *ui,
                              float width, float height) {
  float w = 480, h = 220;
  if (!modal_start(ctx, "queue-delete", tr("gui.delete_queue"), (width - w) / 2,
                   (height - h) / 2, w, h, &ui->queue_delete_open)) {
    nk_end(ctx);
    return;
  }
  text_at(ctx, 20, 78, w - 40, 48,
          tr("gui.downloads_in_this_queue_will_move_to_default"), 13, TEXT,
          SURFACE);
  if (button(ctx, w - 222, h - 43, 84, 30, tr("gui.cancel"), true, false))
    ui->queue_delete_open = false;
  if (button(ctx, w - 132, h - 43, 112, 30, tr("gui.delete"), true, true)) {
    bool queued = gui_controller_enqueue_queue_delete(ui->queue_delete_id);
    report_enqueue(ui, queued);
    if (queued)
      ui->queue_delete_open = false;
  }
  modal_end(ctx);
}

static void draw_category_menu(struct nk_context *ctx, UiState *ui,
                               float width, float height) {
  if (!ui->category_menu_open)
    return;
  IpcCategoryV1 categories[GUI_MODEL_MAX_CATEGORIES];
  int count = gui_model_snapshot_categories(categories,
                                             GUI_MODEL_MAX_CATEGORIES);
  IpcCategoryV1 *selected = NULL;
  for (int i = 0; i < count; ++i)
    if (categories[i].id == ui->category_menu_id)
      selected = &categories[i];
  if (!selected || selected->id == 1) {
    ui->category_menu_open = false;
    return;
  }
  float x = ui->category_menu_pos.x, y = ui->category_menu_pos.y;
  if (x + 174 > width)
    x = width - 174;
  if (y + 76 > height)
    y = height - 76;
  struct nk_rect bounds = nk_rect(x, y, 174, 76);
  bool just_opened = ui->category_menu_just_opened;
  ui->category_menu_just_opened = false;
  if (!just_opened && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
      !nk_input_is_mouse_hovering_rect(&ctx->input, bounds)) {
    ui->category_menu_open = false;
    return;
  }
  if (nk_popup_begin(ctx, NK_POPUP_STATIC, "category-menu",
                     NK_WINDOW_NO_SCROLLBAR, bounds)) {
    nk_layout_space_begin(ctx, NK_STATIC, 76, 2);
    fill(ctx, screen_rect(ctx, 0, 0, 174, 76), 8, SURFACE2);
    if (button(ctx, 5, 5, 164, 30, tr("gui.edit"), true, false)) {
      ui->category_draft = *selected;
      ui->category_edit_open = true;
      ui->category_menu_open = false;
    }
    if (button(ctx, 5, 39, 164, 30, tr("gui.delete"), true, false)) {
      ui->category_delete_id = selected->id;
      ui->category_delete_open = true;
      ui->category_menu_open = false;
    }
    nk_layout_space_end(ctx);
    if (!ui->category_menu_open)
      nk_popup_close(ctx);
    nk_popup_end(ctx);
  } else {
    ui->category_menu_open = false;
  }
}

static void draw_category_edit(struct nk_context *ctx, UiState *ui,
                               float width, float height) {
  float w = 520, h = 360;
  if (!modal_start(ctx, "category-edit",
                   ui->category_draft.id ? tr("gui.edit_category") : tr("gui.add_category"),
                   (width - w) / 2, (height - h) / 2, w, h,
                   &ui->category_edit_open)) {
    nk_end(ctx);
    return;
  }
  nk_layout_space_push(ctx, nk_rect(20, 65, w - 40, 220));
  if (nk_group_begin(ctx, "category-fields", 0)) {
    input_field(ctx, tr("gui.name"), ui->category_draft.name,
                sizeof(ui->category_draft.name));
    input_field(ctx, tr("gui.extensions_comma_separated"),
                ui->category_draft.extensions,
                sizeof(ui->category_draft.extensions));
    input_field(ctx, tr("gui.default_directory_optional"),
                ui->category_draft.default_dir,
                sizeof(ui->category_draft.default_dir));
    nk_group_end(ctx);
  }
  text_at(ctx, 20, 283, w - 40, 24, ui->error, 11, RED, SURFACE);
  if (button(ctx, w - 230, h - 42, 80, 30, tr("gui.cancel"), true, false))
    ui->category_edit_open = false;
  if (button(ctx, w - 142, h - 42, 122, 30, tr("gui.save"), true, true)) {
    if (!ui->category_draft.name[0])
      copy_text(ui->error, sizeof(ui->error), tr("gui.category_name_is_required"));
    else {
      bool queued = ui->category_draft.id
          ? gui_controller_enqueue_category_update(&ui->category_draft)
          : gui_controller_enqueue_category_create(&ui->category_draft);
      report_enqueue(ui, queued);
      if (queued)
        ui->category_edit_open = false;
    }
  }
  modal_end(ctx);
}

static void draw_category_delete(struct nk_context *ctx, UiState *ui,
                                 float width, float height) {
  float w = 480, h = 220;
  if (!modal_start(ctx, "category-delete", tr("gui.delete_category"),
                   (width - w) / 2, (height - h) / 2, w, h,
                   &ui->category_delete_open)) {
    nk_end(ctx);
    return;
  }
  text_at(ctx, 20, 78, w - 40, 48,
          tr("gui.downloads_in_this_category_will_move_to_default"),
          13, TEXT, SURFACE);
  if (button(ctx, w - 222, h - 43, 84, 30, tr("gui.cancel"), true, false))
    ui->category_delete_open = false;
  if (button(ctx, w - 132, h - 43, 112, 30, tr("gui.delete"), true, true)) {
    bool queued = gui_controller_enqueue_category_delete(ui->category_delete_id);
    report_enqueue(ui, queued);
    if (queued) {
      if (ui->category == ui->category_delete_id)
        ui->category = 0;
      ui->category_delete_open = false;
    }
  }
  modal_end(ctx);
}

static void draw_details(struct nk_context *ctx, UiState *ui, float width,
                         float height) {
  float w = 580, h = 390;
  if (!modal_start(ctx, "details", tr("gui.download_details"), (width - w) / 2,
                   (height - h) / 2, w, h, &ui->details_open)) {
    nk_end(ctx);
    return;
  }
  nk_layout_space_push(ctx, nk_rect(20, 74, w - 40, h - 130));
  if (nk_group_begin(ctx, "details-fields", 0)) {
    nk_layout_row_dynamic(ctx, THEME_ROW_CHOICE, 1);
    if (!ui->details_found)
      nk_label(ctx, tr("gui.loading_details"), NK_TEXT_LEFT);
    else {
      nk_labelf(ctx, NK_TEXT_LEFT, "ID %u  |  %s: %llu bytes/sec",
                ui->selected_id, tr("gui.speed_limit"),
                (unsigned long long)ui->details.speed_limit_bps);
      nk_labelf_wrap(ctx, "%s: %s", tr("gui.cookie"), ui->details.cookie);
      nk_labelf_wrap(ctx, "%s: %s", tr("gui.referrer"), ui->details.referrer);
      nk_labelf_wrap(ctx, "%s: %s", tr("gui.headers"),
                      ui->details.extra_headers);
      nk_labelf_wrap(ctx, "SHA-256: %s", ui->details.expected_sha256);
    }
    nk_group_end(ctx);
  }
  text_at(ctx, 20, h - 56, w - 125, 36, ui->error, 11, STATUS_ERROR,
          SURFACE);
  if (button(ctx, w - 100, h - 46, 80, 30, tr("gui.close"), true, false))
    ui->details_open = false;
  modal_end(ctx);
}

static void draw_delete_confirmation(struct nk_context *ctx, UiState *ui,
                                     float width, float height) {
  float w = 520, h = 230;
  if (!modal_start(ctx, "delete-file", tr("gui.delete_download_file"),
                   (width - w) / 2, (height - h) / 2, w, h,
                   &ui->delete_confirm_open)) {
    nk_end(ctx);
    return;
  }
  text_at(ctx, 20, 76, w - 40, 22,
          tr("gui.delete_this_download_record_and_its_file_from_disk"), 12, TEXT,
          SURFACE);
  text_at(ctx, 20, 111, w - 40, 22, ui->delete_confirm_path, 11, MUTED,
          SURFACE);
  fill(ctx, screen_rect(ctx, 0, h - 54, w, 1), 0, BORDER);
  if (button(ctx, w - 232, h - 41, 80, 30, tr("gui.cancel"), true, false))
    ui->delete_confirm_open = false;
  if (button(ctx, w - 144, h - 41, 124, 30, tr("gui.delete_file"), true, true)) {
    report_enqueue(ui, gui_controller_enqueue_remove(ui->delete_confirm_id,
                                                      true));
    ui->delete_confirm_open = false;
  }
  modal_end(ctx);
}

static void draw_toast(struct nk_context *ctx, UiState *ui, float width,
                       float height) {
  if (ui->clipboard_offer_open) {
    if (nk_begin(ctx, "clipboard-offer",
                 nk_rect(width - 360, height - 300, 340, 116),
                 NK_WINDOW_NO_SCROLLBAR)) {
      nk_layout_space_begin(ctx, NK_STATIC, 116, 4);
      fill(ctx, screen_rect(ctx, 0, 0, 340, 116), 10, SURFACE2);
      outline(ctx, screen_rect(ctx, .5f, .5f, 339, 115), 10);
      bold_at(ctx, 16, 12, 308, 20, tr("gui.url_copied_to_clipboard"), 13,
              ACCENT, SURFACE2);
      text_at(ctx, 16, 38, 308, 20, ui->clipboard_offer, 11, TEXT,
              SURFACE2);
      if (button(ctx, 16, 74, 148, 30, tr("gui.browser.review"), true, true)) {
        copy_text(ui->url, sizeof(ui->url), ui->clipboard_offer);
        ui->add_open = true;
        ui->clipboard_offer_open = false;
      }
      if (button(ctx, 176, 74, 148, 30, tr("gui.dismiss"), true, false))
        ui->clipboard_offer_open = false;
      nk_layout_space_end(ctx);
    }
    nk_end(ctx);
  }
  if (ui->duplicate_toast_open) {
    if (nk_begin(ctx, "duplicate-toast",
                 nk_rect(width - 300, height - 100, 280, 80),
                 NK_WINDOW_NO_SCROLLBAR)) {
      nk_layout_row_dynamic(ctx, THEME_ROW_TEXT, 1);
      nk_labelf(ctx, NK_TEXT_LEFT, "%s (ID %u)",
                tr("gui.already_downloading"), ui->duplicate_id);
      nk_layout_row_dynamic(ctx, THEME_ROW_SECTION, 1);
      if (nk_button_label(ctx, tr("gui.close")))
        ui->duplicate_toast_open = false;
    }
    nk_end(ctx);
  }
  if (!ui->toast_open)
    return;
  if (nk_begin(ctx, "completion-toast",
               nk_rect(width - 300, height - 170, 280, 150),
               NK_WINDOW_NO_SCROLLBAR)) {
    nk_layout_space_begin(ctx, NK_STATIC, 150, 4);
    fill(ctx, screen_rect(ctx, 0, 0, 280, 150), 10, SURFACE2);
    outline(ctx, screen_rect(ctx, .5f, .5f, 279, 149), 10);
    bold_at(ctx, 16, 14, 220, 20, tr("gui.download_complete"), 13, GREEN, SURFACE2);
    if (close_button(ctx, 240, 10))
      ui->toast_open = false;
    text_at(ctx, 16, 42, 248, 18, filename_for_row(&ui->toast), 13, TEXT,
            SURFACE2);
    char directory[IPC_MAX_PATH_LEN];
    copy_text(directory, sizeof(directory), ui->toast.dest_path);
    const char *slash = last_separator(directory);
    if (slash)
      directory[slash == directory ? 1 : (size_t)(slash - directory)] = '\0';
    text_at(ctx, 16, 64, 248, 18, directory, 11, MUTED, SURFACE2);
    if (button(ctx, 16, 100, 120, 30, tr("gui.open"), true, true))
      open_path(ui, ui->toast.dest_path, false);
    if (button(ctx, 144, 100, 120, 30, tr("gui.open_folder"), true, false))
      open_path(ui, ui->toast.dest_path, true);
    nk_layout_space_end(ctx);
  }
  nk_end(ctx);
}

static void maybe_complete(UiState *ui, GuiRow *old, const char *status) {
  if (old && strcmp(old->status, "DONE") && !strcmp(status, "DONE")) {
    ui->toast = *old;
    copy_text(ui->toast.status, sizeof(ui->toast.status), "DONE");
    ui->toast.progress = 1;
    ui->toast_open = true;
  }
}

static void consume_events(UiState *ui) {
  GuiControllerEvent event;
  while (gui_controller_poll(&event)) {
    switch (event.type) {
    case GUI_CONTROLLER_EVENT_STATUS: {
      if (strcmp(event.data.status.status, "REMOVED") == 0) {
        gui_model_remove_local_row(event.data.status.download_id);
        if (ui->selected_id == event.data.status.download_id)
          ui->selected_id = 0;
        break;
      }
      GuiRow previous[GUI_MODEL_MAX_ROWS];
      int n = gui_model_snapshot_rows(previous, GUI_MODEL_MAX_ROWS);
      maybe_complete(ui, find_row(previous, n, event.data.status.download_id),
                     event.data.status.status);
      if (event.data.status.has_v2)
        gui_model_apply_status_update_v2(event.data.status.download_id,
                                         event.data.status.status,
                                         &event.data.status.v2);
      else
        gui_model_apply_status_update(event.data.status.download_id,
                                      event.data.status.status,
                                      event.data.status.progress);
      break;
    }
    case GUI_CONTROLLER_EVENT_SNAPSHOT: {
      GuiRow previous[GUI_MODEL_MAX_ROWS];
      int n = gui_model_snapshot_rows(previous, GUI_MODEL_MAX_ROWS);
      if (event.data.snapshot.offset > ui->history_offset) {
        uint32_t dropped = event.data.snapshot.offset - ui->history_offset;
        if (dropped > (uint32_t)n)
          dropped = (uint32_t)n;
        for (uint32_t i = 0; i < dropped; i++)
          ui->pending_scroll_adjust +=
              !strcmp(previous[i].status, "ACTIVE") ||
                      !strcmp(previous[i].status, "PAUSED") ||
                      !strcmp(previous[i].status, "QUEUED")
                  ? 69 : 59;
      }
      for (int i = 0; i < event.data.snapshot.count; ++i)
        maybe_complete(ui,
                       find_row(previous, n, event.data.snapshot.records[i].id),
                       event.data.snapshot.records[i].status);
      gui_model_apply_snapshot(event.data.snapshot.records,
                               event.data.snapshot.count);
      if (event.data.snapshot.offset != ui->history_offset ||
          event.data.snapshot.count != n ||
          (uint64_t)event.data.snapshot.offset +
                  (uint32_t)event.data.snapshot.count >=
              event.data.snapshot.total)
        ui->history_loading = false;
      ui->history_offset = event.data.snapshot.offset;
      ui->history_total = event.data.snapshot.total;
      break;
    }
    case GUI_CONTROLLER_EVENT_CONNECTION:
      ui->connected = event.data.connection.connected;
      if (!ui->connected)
        ui->history_loading = false;
      break;
    case GUI_CONTROLLER_EVENT_OPERATION:
      if (event.data.operation.succeeded) {
        uint32_t id = event.data.operation.download_id;
        switch (event.data.operation.operation) {
        case GUI_CONTROLLER_OPERATION_ADD:
          if (event.data.operation.duplicate) {
            ui->selected_id = id;
            ui->duplicate_id = id;
            ui->duplicate_toast_open = true;
          } else {
            gui_model_add_local_row(id, event.data.operation.url,
                                    event.data.operation.dest_path);
          }
          break;
        case GUI_CONTROLLER_OPERATION_PAUSE:
          gui_model_apply_optimistic(id, "PAUSED");
          break;
        case GUI_CONTROLLER_OPERATION_RESUME:
          gui_model_apply_optimistic(id, "QUEUED");
          break;
        case GUI_CONTROLLER_OPERATION_CANCEL:
          gui_model_apply_optimistic(id, "CANCELED");
          break;
        case GUI_CONTROLLER_OPERATION_REFRESH_URL:
          break;
        case GUI_CONTROLLER_OPERATION_REMOVE:
          gui_model_remove_local_row(id);
          if (ui->selected_id == id)
            ui->selected_id = 0;
          break;
        }
      }
      break;
    case GUI_CONTROLLER_EVENT_DETAILS:
      if (event.data.details.download_id == ui->selected_id) {
        ui->details = event.data.details.details;
        ui->details_found = event.data.details.found;
      }
      break;
    case GUI_CONTROLLER_EVENT_ERROR:
      copy_text(ui->error, sizeof(ui->error), event.data.error.message);
      ui->history_loading = false;
      break;
    case GUI_CONTROLLER_EVENT_QUEUES:
      gui_model_apply_queues(event.data.queues.queues,
                             event.data.queues.count);
      break;
    case GUI_CONTROLLER_EVENT_CATEGORIES:
      gui_model_apply_categories(event.data.categories.categories,
                                  event.data.categories.count);
      break;
    }
  }
}

int run_gui(void) {
  if (!gui_client_connect()) {
    fprintf(stderr, "%s\n", tr("gui.cannot_connect_to_daemon"));
    return 1;
  }
  gui_model_init();
  GuiSdlBackendConfig config = {.width = 1100,
                                .height = 720,
                                .title = tr("gui.core_download_manager"),
                                .font_size = 13};
  GuiSdlBackend *backend = gui_sdl_backend_create(&config);
  if (!backend) {
    gui_client_disconnect();
    return 1;
  }
  struct nk_context *ctx = gui_sdl_backend_context(backend);
  for (int i = 0; i < 6; ++i) {
    fonts[i] = gui_sdl_backend_font(backend, 11 + i);
    bold_fonts[i] = gui_sdl_backend_bold_font(backend, 11 + i);
  }
  DownloadManagerConfig initial_config;
  config_get(&initial_config);
  theme_apply(ctx, theme_from_text(initial_config.ui_theme));
  if (!gui_controller_start()) {
    gui_sdl_backend_destroy(backend);
    gui_client_disconnect();
    return 1;
  }
  UiState ui = {.connected = true, .history_loading = true,
                .disk_checked_at = UINT32_MAX};
  DownloadManagerConfig saved_config;
  config_get(&saved_config);
  ui.clipboard_monitor_enabled = saved_config.clipboard_monitor;
  ui.clipboard_monitor = saved_config.clipboard_monitor;
  clipboard_baseline(&ui);
  copy_text(ui.numbers[5], sizeof(ui.numbers[5]), "0");
  copy_text(ui.folder, sizeof(ui.folder), config_get_default_download_dir());
  while (gui_sdl_backend_poll(backend)) {
    consume_events(&ui);
    GuiRow rows[GUI_MODEL_MAX_ROWS];
    int n = gui_model_snapshot_rows(rows, GUI_MODEL_MAX_ROWS);
    GuiRow visible[GUI_MODEL_MAX_ROWS];
    int visible_count = 0;
    IpcCategoryV1 categories[GUI_MODEL_MAX_CATEGORIES];
    int category_count = gui_model_snapshot_categories(
        categories, GUI_MODEL_MAX_CATEGORIES);
    int category_counts[GUI_MODEL_MAX_CATEGORIES] = {0};
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j < category_count; ++j)
        if (rows[i].category_id == categories[j].id) {
          ++category_counts[j];
          break;
        }
      if (row_matches(&ui, &rows[i]))
        visible[visible_count++] = rows[i];
    }
    if (ui.selected_id && !find_row(visible, visible_count, ui.selected_id))
      ui.selected_id = 0;
    uint32_t now = SDL_GetTicks();
    poll_clipboard(&ui, now);
    if (ui.disk_checked_at == UINT32_MAX ||
        now - ui.disk_checked_at >= 10000) {
      /* Verified: this checks the configured directory's filesystem, not '/'. */
      ui.disk_available = diskspace_get(config_get_default_download_dir(),
                                        &ui.disk);
      ui.disk_checked_at = now;
    }
    int window_width, window_height;
    SDL_GetWindowSize(SDL_GL_GetCurrentWindow(), &window_width, &window_height);
    float width = (float)window_width, height = (float)window_height;
    bool modal = ui.settings_open || ui.add_open || ui.batch_add_open ||
                 ui.details_open ||
                 ui.delete_confirm_open || ui.queue_add_open ||
                 ui.queue_delete_open || ui.category_edit_open ||
                 ui.category_delete_open;
    if (modal) {
        float mw = ui.settings_open ? 460
                 : ui.category_delete_open ? 480
                 : ui.category_edit_open ? 520
                 : ui.queue_delete_open ? 480
                 : ui.add_open || ui.batch_add_open || ui.delete_confirm_open || ui.queue_add_open
                     ? 520 : 580;
      float mh =
          ui.settings_open ? (height * .8f > 650 ? 650 : height * .8f)
          : ui.delete_confirm_open ? 230
          : ui.category_delete_open ? 220
          : ui.category_edit_open ? 360
          : ui.queue_delete_open ? 220
          : ui.queue_add_open ? (height * .82f > 660 ? 660 : height * .82f)
          : ui.batch_add_open ? 550
          : ui.add_open
              ? (ui.advanced ? (height * .85f > 700 ? 700 : height * .85f)
                             : 350)
              : 390;
      struct nk_rect modal_bounds =
          nk_rect((width - mw) / 2, (height - mh) / 2, mw, mh);
      if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_ESCAPE] ||
          (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
           !nk_input_is_mouse_hovering_rect(&ctx->input, modal_bounds))) {
        ui.settings_open = ui.add_open = ui.batch_add_open = ui.details_open =
            ui.delete_confirm_open = false;
        ui.queue_add_open = ui.queue_delete_open = false;
        ui.category_edit_open = ui.category_delete_open = false;
        modal = false;
      }
    }
    gui_sdl_backend_begin_frame(backend);
    if (nk_begin(ctx, "Download Manager", nk_rect(0, 0, width, height),
                 NK_WINDOW_NO_SCROLLBAR | (modal ? NK_WINDOW_NO_INPUT : 0))) {
      nk_layout_space_begin(ctx, NK_STATIC, height, 32);
      draw_chrome(ctx, &ui, categories, category_count, category_counts,
                  visible_count, width, height);
      if (ui.tab == TAB_QUEUES)
        draw_queues(ctx, &ui, width, height);
      else {
        draw_rows(ctx, &ui, visible, visible_count, width, height);
        draw_toolbar(ctx, &ui, visible, visible_count);
      }
      if (ui.error[0]) {
        text_at(ctx, 236, height - 34, width - 290, 32, ui.error, 12,
                STATUS_ERROR, BG);
        if (close_button(ctx, width - 42, height - 31))
          ui.error[0] = '\0';
      }
      nk_layout_space_end(ctx);
      if (!modal)
        draw_menu(ctx, &ui, visible, visible_count, width, height);
      if (!modal)
        draw_category_menu(ctx, &ui, width, height);
    }
    nk_end(ctx);
    if (!modal)
      draw_toast(ctx, &ui, width, height);
    if (ui.settings_open || ui.add_open || ui.batch_add_open || ui.details_open ||
        ui.delete_confirm_open || ui.queue_add_open || ui.queue_delete_open ||
        ui.category_edit_open || ui.category_delete_open) {
      modal_backdrop(ctx, width, height);
      if (ui.settings_open)
        draw_settings(ctx, &ui, width, height);
      else if (ui.add_open)
        draw_add(ctx, &ui, width, height);
      else if (ui.batch_add_open)
        draw_batch_add(ctx, &ui, width, height);
      else if (ui.delete_confirm_open)
        draw_delete_confirmation(ctx, &ui, width, height);
      else if (ui.queue_add_open)
        draw_queue_add(ctx, &ui, width, height);
      else if (ui.queue_delete_open)
        draw_queue_delete(ctx, &ui, width, height);
      else if (ui.category_edit_open)
        draw_category_edit(ctx, &ui, width, height);
      else if (ui.category_delete_open)
        draw_category_delete(ctx, &ui, width, height);
      else
        draw_details(ctx, &ui, width, height);
    }
    if (!ui.settings_open && ui.theme_preview != ui.theme_saved) {
      theme_apply(ctx, ui.theme_saved);
      ui.theme_preview = ui.theme_saved;
    }
    gui_sdl_backend_end_frame(backend);
    SDL_Delay(8);
  }
  gui_controller_stop();
  gui_sdl_backend_destroy(backend);
  gui_client_disconnect();
  return 0;
}
