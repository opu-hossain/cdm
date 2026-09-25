#include "../utils/config.h"
#include "../utils/log.h"
#include "../utils/path.h"
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

#define GUI_URL_CAP 2048
#define GUI_FOLDER_CAP 1024
#define GUI_OPTION_CAP 4096
#define GUI_SHA256_CAP 65
#define GUI_SEARCH_CAP 256

typedef enum { TAB_ALL, TAB_DOWNLOADING, TAB_COMPLETED, TAB_QUEUES } GuiTab;
typedef enum {
  CATEGORY_ALL, CATEGORY_DOCUMENTS, CATEGORY_COMPRESSED,
  CATEGORY_MUSIC, CATEGORY_VIDEO, CATEGORY_PROGRAMS, CATEGORY_COUNT
} GuiCategory;

/* Exact tokens from docs/ui/mockup.html. */
static const struct nk_color BG = {11, 15, 20, 255};
static const struct nk_color SURFACE = {17, 23, 34, 255};
static const struct nk_color SURFACE2 = {22, 30, 44, 255};
static const struct nk_color BORDER = {35, 45, 61, 255};
static const struct nk_color TEXT = {232, 236, 241, 255};
static const struct nk_color MUTED = {124, 138, 157, 255};
static const struct nk_color ACCENT = {61, 157, 207, 255};
static const struct nk_color ACCENT_DIM = {31, 92, 128, 255};
static const struct nk_color GREEN = {63, 143, 95, 255};
static const struct nk_color RED = {163, 68, 72, 255};
static const struct nk_color AMBER = {184, 130, 63, 255};
static const struct nk_color WHITE = {255, 255, 255, 255};
static const struct nk_color DISABLED = {80, 93, 111, 255};
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
  GuiCategory category;
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
  bool queue_add_open, queue_delete_open;
  uint32_t queue_edit_id, queue_delete_id, queue_drag_id, add_queue_id;
  Queue queue_draft;
  char queue_priority[16], queue_cap[16];
  bool clipboard_monitor, clipboard_monitor_enabled, clipboard_offer_open;
  char clipboard_seen[GUI_URL_CAP], clipboard_offer[GUI_URL_CAP];
  uint32_t clipboard_checked_at, clipboard_changed_at;
} UiState;

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

static bool ascii_equal(const char *a, const char *b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
      return false;
  }
  return !*a && !*b;
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

static GuiCategory category_for_row(const GuiRow *row) {
  const char *filename = filename_for_row(row);
  const char *dot = strrchr(filename, '.');
  if (!dot || dot == filename || !dot[1])
    return CATEGORY_ALL;
  const char *ext = dot + 1;
  static const char *extensions[CATEGORY_COUNT] = {
      "", "pdf doc docx txt xls xlsx ppt pptx odt csv",
      "zip rar 7z tar gz tgz bz2 xz", "mp3 wav flac ogg m4a aac",
      "mp4 mkv avi mov webm flv", "exe msi dmg apk deb rpm appimage sh"};
  for (int category = CATEGORY_DOCUMENTS; category < CATEGORY_COUNT;
       ++category) {
    const char *p = extensions[category];
    while (*p) {
      const char *end = strchr(p, ' ');
      size_t len = end ? (size_t)(end - p) : strlen(p);
      if (strlen(ext) == len) {
        char candidate[16];
        memcpy(candidate, p, len);
        candidate[len] = '\0';
        if (ascii_equal(ext, candidate))
          return (GuiCategory)category;
      }
      if (!end)
        break;
      p = end + 1;
    }
  }
  return CATEGORY_ALL;
}

static bool row_matches(const UiState *ui, const GuiRow *row) {
  if (ui->tab == TAB_DOWNLOADING && !can_pause(row) &&
      !is_status(row, "PAUSED"))
    return false;
  if (ui->tab == TAB_COMPLETED && !is_status(row, "DONE"))
    return false;
  if (ui->category != CATEGORY_ALL && category_for_row(row) != ui->category)
    return false;
  return contains_case_insensitive(filename_for_row(row), ui->search) ||
         contains_case_insensitive(row->dest_path, ui->search);
}

static void apply_theme(struct nk_context *ctx) {
  struct nk_color colors[NK_COLOR_COUNT];
  for (int i = 0; i < NK_COLOR_COUNT; ++i)
    colors[i] = SURFACE2;
  colors[NK_COLOR_TEXT] = TEXT;
  colors[NK_COLOR_WINDOW] = BG;
  colors[NK_COLOR_HEADER] = SURFACE;
  colors[NK_COLOR_BORDER] = BORDER;
  colors[NK_COLOR_BUTTON] = SURFACE2;
  colors[NK_COLOR_BUTTON_HOVER] = BORDER;
  colors[NK_COLOR_BUTTON_ACTIVE] = ACCENT_DIM;
  colors[NK_COLOR_EDIT] = BG;
  colors[NK_COLOR_EDIT_CURSOR] = TEXT;
  colors[NK_COLOR_PROPERTY] = BG;
  colors[NK_COLOR_SELECT_ACTIVE] = ACCENT_DIM;
  colors[NK_COLOR_TOGGLE_CURSOR] = ACCENT;
  colors[NK_COLOR_SCROLLBAR] = BG;
  colors[NK_COLOR_SCROLLBAR_CURSOR] = BORDER;
  colors[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = MUTED;
  colors[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = ACCENT;
  nk_style_from_table(ctx, colors);
  ctx->style.window.padding = nk_vec2(0, 0);
  ctx->style.window.group_padding = nk_vec2(0, 0);
  ctx->style.window.spacing = nk_vec2(0, 0);
  ctx->style.window.border = 0;
  ctx->style.window.group_border = 0;
  ctx->style.window.popup_padding = nk_vec2(0, 0);
  ctx->style.window.popup_border = 0;
  ctx->style.window.scrollbar_size = nk_vec2(5, 5);
  ctx->style.button.rounding = 7;
  ctx->style.button.padding = nk_vec2(7, 4);
  ctx->style.button.border = 0;
  ctx->style.edit.rounding = 7;
  ctx->style.edit.padding = nk_vec2(10, 8);
  ctx->style.edit.border = 1;
  ctx->style.edit.cursor_size = 1.5f;
  ctx->style.property.rounding = 7;
  ctx->style.property.border = 1;
  ctx->style.property.padding = nk_vec2(10, 8);
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
  style.hover = nk_style_item_color(primary ? nk_rgb(75, 171, 224) : BORDER);
  style.active = nk_style_item_color(ACCENT_DIM);
  style.text_normal = style.text_hover = style.text_active =
      enabled ? (primary ? WHITE : TEXT) : DISABLED;
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
              "Command queue is full. Please try again.");
}

static bool add_download(const char *url, const char *folder,
                         const char *cookie, const char *referrer,
                         const char *headers, const char *sha256,
                         uint64_t speed_limit, uint32_t queue_id) {
  char filename[512];
  char path[IPC_MAX_PATH_LEN];
  char unique_path[IPC_MAX_PATH_LEN];
  path_filename_from_url(url, filename, sizeof(filename));
  if (!path_join(folder, filename, path, sizeof(path)) ||
      !path_make_unique(path, unique_path, sizeof(unique_path))) {
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
  };
  bool has_options = options.cookie || options.referrer ||
                     options.extra_headers || options.expected_sha256 ||
                     options.speed_limit_bps > 0 || options.queue_id > 0;
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
                "Cannot open a relative destination folder.");
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
              "Opening files requires an absolute destination path.");
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
    snprintf(ui->error, sizeof(ui->error), "Could not open destination: %.190s",
             SDL_GetError());
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
                        const int category_counts[CATEGORY_COUNT],
                        int visible_count, float width, float height) {
  fill(ctx, screen_rect(ctx, 0, 0, width, 52), 0, SURFACE);
  fill(ctx, screen_rect(ctx, 0, 52, 220, height - 52), 0, SURFACE);
  fill(ctx, screen_rect(ctx, 0, 51, width, 1), 0, BORDER);
  fill(ctx, screen_rect(ctx, 219, 52, 1, height - 52), 0, BORDER);
  bold_at(ctx, 18, 0, 142, 52, "CDM", 15, TEXT, SURFACE);
  const char *tabs[] = {"All", "Downloading", "Completed", "Queues"};
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
              "Search downloads...", 13, DISABLED, BG);
  }
  struct nk_color connection = ui->connected ? GREEN : RED;
  nk_fill_circle(nk_window_get_canvas(ctx),
                 screen_rect(ctx, width - 98, 23, 7, 7), connection);
  text_at(ctx, width - 84, 0, 80, 52,
          ui->connected ? "Connected" : "Disconnected", 13, connection,
          SURFACE);
  if (button(ctx, 14, 66, 192, 40,
             ui->tab == TAB_QUEUES ? "+ Add queue" : "+ New Download", true,
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
  const char *categories[] = {"All categories", "Documents", "Compressed",
                              "Music",          "Video",     "Programs"};
  const char *icons[] = {"=", "D", "Z", "M", "V", "P"};
  for (int i = 0; i < 6 && ui->tab != TAB_QUEUES; ++i) {
    float y = 122 + i * 36;
    if (nav_button(ctx, 14, y, 192, 33, "", ui->category == (GuiCategory)i))
      ui->category = (GuiCategory)i;
    struct nk_color bg = ui->category == (GuiCategory)i ? ACCENT_DIM : SURFACE;
    text_at(ctx, 24, y, 16, 33, icons[i], 12, MUTED, bg);
    text_at(ctx, 50, y, 122, 33, categories[i], 13,
            ui->category == (GuiCategory)i ? TEXT : MUTED, bg);
    char total[16];
    snprintf(total, sizeof(total), "%d", category_counts[i]);
    text_at(ctx, 181, y, 23, 33, total, 11, nk_rgb(207, 232, 247), bg);
  }
  fill(ctx, screen_rect(ctx, 14, height - 83, 192, 1), 0, BORDER);
  text_at(ctx, 14, height - 69, 192, 14, "LOCAL STORAGE", 11, DISABLED,
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
    snprintf(storage, sizeof(storage), "%.1f%% used  /  %.1f GB free",
             used * 100.0, (double)ui->disk.free_bytes / 1000000000.0);
    text_at(ctx, 14, height - 38, 192, 16, storage, 11, MUTED, SURFACE);
  } else {
    text_at(ctx, 14, height - 38, 192, 16, "Storage usage unavailable", 11,
            DISABLED, SURFACE);
  }
  char total[64];
  if (ui->tab == TAB_QUEUES) {
    Queue queues[GUI_MODEL_MAX_QUEUES];
    int count = gui_model_snapshot_queues(queues, GUI_MODEL_MAX_QUEUES);
    int written = snprintf(total, sizeof(total), "%d queues", count);
    if (written < 0 || (size_t)written >= sizeof(total))
      total[0] = '\0';
  } else if (ui->category == CATEGORY_ALL && ui->tab == TAB_ALL &&
             !ui->search[0])
    snprintf(total, sizeof(total), "%d of %u downloads", visible_count,
             ui->history_total);
  else
    snprintf(total, sizeof(total), "%d downloads", visible_count);
  text_at(ctx, width - 183, 60, 170, 32, total, 12, MUTED, BG);
  fill(ctx, screen_rect(ctx, 220, 100, width - 220, 1), 0, BORDER);
}

static void draw_toolbar(struct nk_context *ctx, UiState *ui, GuiRow *rows,
                         int count) {
  GuiRow *selected = find_row(rows, count, ui->selected_id);
  if (tool_button(ctx, 236, "add", "Add download", true))
    ui->add_open = true;
  if (tool_button(ctx, 272, "cancel", "Cancel selected download",
                  can_pause(selected) || is_status(selected, "PAUSED")))
    report_enqueue(ui, gui_controller_enqueue_cancel(selected->id));
  fill(ctx, screen_rect(ctx, 314, 66, 1, 20), 0, BORDER);
  if (tool_button(ctx, 325, "pause", "Pause selected download",
                  can_pause(selected)))
    report_enqueue(ui, gui_controller_enqueue_pause(selected->id));
  if (tool_button(ctx, 361, "resume", "Resume selected download",
                  is_status(selected, "PAUSED")))
    report_enqueue(ui, gui_controller_enqueue_resume(selected->id));
  fill(ctx, screen_rect(ctx, 403, 66, 1, 20), 0, BORDER);
  if (tool_button(ctx, 414, "settings", "Settings", true))
    open_settings(ui);
}

static void file_chip(const char *filename, char ext[5],
                      struct nk_color *color) {
  const char *dot = strrchr(filename, '.');
  copy_text(ext, 5, dot && dot[1] ? dot + 1 : "FILE");
  for (char *p = ext; *p; ++p)
    *p = (char)toupper((unsigned char)*p);
  *color = nk_rgb(61, 127, 176);
  if (!strcmp(ext, "ZIP") || !strcmp(ext, "GZ") || !strcmp(ext, "7Z") ||
      !strcmp(ext, "TAR"))
    *color = nk_rgb(122, 95, 176);
  else if (!strcmp(ext, "MP4") || !strcmp(ext, "MKV") || !strcmp(ext, "WEBM") ||
           !strcmp(ext, "MP3"))
    *color = nk_rgb(192, 85, 63);
  else if (!strcmp(ext, "EXE") || !strcmp(ext, "APPI"))
    *color = nk_rgb(63, 143, 111);
  else if (!strcmp(ext, "PNG") || !strcmp(ext, "JPG") || !strcmp(ext, "JPEG"))
    *color = nk_rgb(176, 135, 61);
}

static void draw_rows(struct nk_context *ctx, UiState *ui, GuiRow *rows,
                      int count, float width, float height) {
  float viewport = height - 101 - (ui->error[0] ? 36 : 0);
  nk_layout_space_push(ctx, nk_rect(220, 101, width - 220,
                                    viewport));
  if (!nk_group_begin(ctx, "downloads", 0))
    return;
  nk_layout_set_min_row_height(ctx, 0);
  nk_layout_row_dynamic(ctx, 8, 1);
  nk_label(ctx, "", NK_TEXT_LEFT);
  if (!count && !ui->history_loading) {
    nk_layout_row_dynamic(ctx, 100, 1);
    nk_label_colored(ctx, "Your download queue is empty", NK_TEXT_CENTERED,
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
               bold_fonts[0], WHITE, chip);
    float name_width = r.w - 64 - 14 - 64 - 14 - 28 - 16;
    label(ctx, nk_rect(r.x + 64, r.y + 10, name_width, 16),
          filename_for_row(row), 13, TEXT, bg);
    char meta[IPC_MAX_PATH_LEN + 32];
    if (is_status(row, "ERROR") && row->error[0])
      copy_text(meta, sizeof(meta), row->error);
    else if (is_status(row, "DONE"))
      copy_text(meta, sizeof(meta), "Completed");
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
        snprintf(meta, sizeof(meta), "%s / %s  ·  Paused", received, total);
      else
        snprintf(meta, sizeof(meta), "%s / %s  ·  %s/s  ·  ETA %s",
                 received, total, speed, eta);
    } else if (progress)
      snprintf(meta, sizeof(meta), "%.0f%%  /  %s", row->progress * 100,
               is_status(row, "PAUSED") ? "Paused" : row->dest_path);
    else
      snprintf(meta, sizeof(meta), "%s  /  %s", row->status, row->dest_path);
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
        is_status(row, "DONE") ? nk_rgb(111, 189, 140)
        : (is_status(row, "ERROR") || is_status(row, "CANCELED"))
            ? nk_rgb(224, 132, 136)
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
  bool all_history = ui->category == CATEGORY_ALL && ui->tab == TAB_ALL &&
                     ui->search[0] == '\0';
  if (all_history && ui->history_loading) {
    nk_layout_row_dynamic(ctx, 28, 1);
    nk_label_colored(ctx, "Loading more downloads...", NK_TEXT_CENTERED,
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
  float mh = 236;
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
      if (button(ctx, 5, by, 170, 30, "Open", true, false)) {
        open_path(ui, row->dest_path, false);
        close = true;
      }
    } else if (can_pause(row)) {
      if (button(ctx, 5, by, 170, 30, "Pause", true, false)) {
        report_enqueue(ui, gui_controller_enqueue_pause(row->id));
        close = true;
      }
    } else if (paused) {
      if (button(ctx, 5, by, 170, 30, "Resume", true, false)) {
        report_enqueue(ui, gui_controller_enqueue_resume(row->id));
        close = true;
      }
    } else {
      /* ERROR rows cannot prove their partial file is still valid from GuiRow. */
      if (button(ctx, 5, by, 170, 30, "Re-download", true, false)) {
        report_enqueue(
            ui, gui_controller_enqueue_add(row->url, row->dest_path, NULL));
        close = true;
      }
    }
    by += 30;
    if (button(ctx, 5, by, 170, 30, "Open folder", true, false)) {
      open_path(ui, row->dest_path, true);
      close = true;
    }
    by += 30;
    if (done) {
      if (button(ctx, 5, by, 170, 30, "Re-download", true, false)) {
        report_enqueue(
            ui, gui_controller_enqueue_add(row->url, row->dest_path, NULL));
        close = true;
      }
    } else {
      if (button(ctx, 5, by, 170, 30, "Copy URL", true, false)) {
        if (SDL_SetClipboardText(row->url) != 0)
          snprintf(ui->error, sizeof(ui->error), "Could not copy URL: %.200s",
                   SDL_GetError());
        close = true;
      }
    }
    by += 30;
    if (button(ctx, 5, by, 170, 30, "Show details", true, false)) {
      show_details(ui, row->id);
      close = true;
    }
    by += 36;
    fill(ctx, screen_rect(ctx, 7, by - 3, 166, 1), 0, BORDER);
    if (can_pause(row) || paused) {
      if (button(ctx, 5, by, 170, 30, "Cancel", true, false)) {
        report_enqueue(ui, gui_controller_enqueue_cancel(row->id));
        close = true;
      }
    }
    by += 32;
    bool removable = !is_status(row, "ACTIVE");
    if (button(ctx, 5, by, 170, 30, "Remove from list", removable, false)) {
      report_enqueue(ui, gui_controller_enqueue_remove(row->id, false));
      close = true;
    }
    by += 30;
    if (button(ctx, 5, by, 170, 30, "Delete file", removable, false)) {
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
  nk_layout_row_dynamic(ctx, 24, 1);
  nk_label(ctx, label_text, NK_TEXT_LEFT);
  nk_layout_row_dynamic(ctx, 36, 1);
  nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, buffer, 16,
                                 nk_filter_decimal);
  nk_layout_row_dynamic(ctx, 12, 1);
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
  nk_layout_row_dynamic(ctx, 28, 1);
  nk_style_push_font(ctx, fonts[0]);
  nk_label_colored(ctx, text, NK_TEXT_LEFT, MUTED);
  nk_style_pop_font(ctx);
}

static void modal_backdrop(struct nk_context *ctx, float width, float height) {
  nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                           nk_style_item_color(nk_rgba(0, 0, 0, 0)));
  if (nk_begin(ctx, "modal-backdrop", nk_rect(0, 0, width, height),
               NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT))
    fill(ctx, nk_rect(0, 0, width, height), 0, nk_rgba(4, 6, 9, 140));
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

static void draw_settings(struct nk_context *ctx, UiState *ui, float width,
                          float height) {
  float h = height * .8f;
  if (h > 650)
    h = 650;
  float w = 460, x = (width - w) / 2, y = (height - h) / 2;
  if (!modal_start(ctx, "settings", "Settings", x, y, w, h,
                   &ui->settings_open)) {
    nk_end(ctx);
    return;
  }
  text_at(ctx, 20, 70, w - 40, 20,
          "Saved to config.toml and applied immediately.", 11, MUTED, SURFACE);
  bool proxy_url_ok = true;
  nk_layout_space_push(ctx, nk_rect(20, 108, w - 40, h - 188));
  nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                           nk_style_item_color(SURFACE));
  if (nk_group_begin(ctx, "settings-fields", 0)) {
    section(ctx, "GENERAL");
    nk_bool monitor = ui->clipboard_monitor;
    nk_layout_row_dynamic(ctx, 28, 1);
    nk_checkbox_label(ctx, "Monitor clipboard for URLs", &monitor);
    ui->clipboard_monitor = monitor;
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, "Ask before adding a copied URL.", NK_TEXT_LEFT,
                     MUTED);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Default download directory", NK_TEXT_LEFT);
    nk_layout_row_begin(ctx, NK_STATIC, 34, 3);
    nk_layout_row_push(ctx, 312);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->directory,
                                   sizeof(ui->directory), NULL);
    nk_layout_row_push(ctx, 8);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, 90);
    if (nk_button_label(ctx, "Browse")) {
      char *chosen = tinyfd_selectFolderDialog("Default download directory",
                                               ui->directory);
      if (chosen)
        copy_text(ui->directory, sizeof(ui->directory), chosen);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 12, 1);
    nk_label(ctx, "", NK_TEXT_LEFT);
    number_field(ctx, "Maximum concurrent downloads", ui->numbers[0]);
    section(ctx, "CONNECTIONS");
    number_field(ctx, "Connections per download (1-16)", ui->numbers[6]);
    number_field(ctx, "Connect timeout (seconds)", ui->numbers[7]);
    number_field(ctx, "Transfer timeout (seconds)", ui->numbers[8]);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "User-Agent", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 34, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->user_agent,
                                   sizeof(ui->user_agent), NULL);
    if (!ui->user_agent[0]) {
      DownloadManagerConfig current;
      config_get(&current);
      nk_layout_row_dynamic(ctx, 20, 1);
      nk_labelf_colored(ctx, NK_TEXT_LEFT, MUTED, "Current: %s",
                        current.user_agent);
    }
    section(ctx, "RETRIES");
    number_field(ctx, "Maximum retry attempts", ui->numbers[1]);
    number_field(ctx, "Retry base delay (seconds)", ui->numbers[2]);
    number_field(ctx, "Retry maximum delay (seconds)", ui->numbers[3]);
    section(ctx, "BANDWIDTH");
    number_field(ctx, "Global speed limit (bytes/sec, 0 = unlimited)",
                 ui->numbers[4]);
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, "0 disables the limit entirely.", NK_TEXT_LEFT,
                     MUTED);
    section(ctx, "PROXY");
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Mode", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 34, 1);
    const char *modes[] = {"None", "HTTP", "SOCKS5"};
    ui->proxy_mode = (ProxyMode)nk_combo(ctx, modes, 3, ui->proxy_mode, 30,
                                         nk_vec2(400, 110));
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Proxy URL", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 34, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->proxy_url,
                                   sizeof(ui->proxy_url), NULL);
    const char *scheme = strstr(ui->proxy_url, "://");
    proxy_url_ok = ui->proxy_mode == PROXY_NONE ||
                   (scheme && scheme != ui->proxy_url && scheme[3] &&
                    scheme[3] != ':' && scheme[3] != '/');
    if (!proxy_url_ok) {
      nk_layout_row_dynamic(ctx, 22, 1);
      nk_label_colored(ctx, "Enter a proxy URL with scheme and host.",
                       NK_TEXT_LEFT, RED);
    }
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Username", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 34, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->proxy_username,
                                   sizeof(ui->proxy_username), NULL);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Password", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 34, 2);
    if (nk_button_label(ctx, ui->proxy_password[0] ? "Change password..."
                                                  : "Set password...")) {
      char *value = tinyfd_inputBox("Proxy password", "Enter proxy password",
                                   NULL); /* NULL requests a masked input box. */
      if (value)
        copy_text(ui->proxy_password, sizeof(ui->proxy_password), value);
    }
    if (nk_button_label(ctx, "Clear password"))
      ui->proxy_password[0] = '\0';
    nk_group_end(ctx);
  }
  nk_style_pop_style_item(ctx);
  text_at(ctx, 20, h - 80, w - 40, 24, ui->settings_message, 11,
          nk_rgb(224, 132, 136), SURFACE);
  fill(ctx, screen_rect(ctx, 0, h - 60, w, 1), 0, BORDER);
  if (button(ctx, w - 232, h - 46, 80, 32, "Cancel", true, false))
    ui->settings_open = false;
  if (button(ctx, w - 144, h - 46, 124, 32, "Save settings", true, true)) {
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
                "Invalid number. Check the allowed range in each field.");
    else if (!proxy_url_ok)
      copy_text(ui->settings_message, sizeof(ui->settings_message),
                "Proxy URL needs a scheme and host.");
    else if (config_save(&config) && gui_client_reload_config()) {
      ui->clipboard_monitor_enabled = config.clipboard_monitor;
      clipboard_baseline(ui);
      ui->clipboard_offer_open = false;
      copy_text(ui->folder, sizeof(ui->folder), ui->directory);
      ui->disk_checked_at = UINT32_MAX;
      ui->settings_open = false;
    } else
      copy_text(ui->settings_message, sizeof(ui->settings_message),
                "Could not save or apply settings");
  }
  modal_end(ctx);
}

static void input_field(struct nk_context *ctx, const char *name, char *buffer,
                        int size) {
  nk_layout_row_dynamic(ctx, 22, 1);
  nk_label(ctx, name, NK_TEXT_LEFT);
  nk_layout_row_dynamic(ctx, 34, 1);
  nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, buffer, size, NULL);
  nk_layout_row_dynamic(ctx, 10, 1);
  nk_label(ctx, "", NK_TEXT_LEFT);
}

static void draw_add(struct nk_context *ctx, UiState *ui, float width,
                     float height) {
  float w = 520, h = ui->advanced ? height * .85f : 350;
  if (h > 700)
    h = 700;
  if (!modal_start(ctx, "add-download", "New Download", (width - w) / 2,
                   (height - h) / 2, w, h, &ui->add_open)) {
    nk_end(ctx);
    return;
  }
  nk_layout_space_push(ctx, nk_rect(20, 72, w - 40, h - 146));
  nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                           nk_style_item_color(SURFACE));
  if (nk_group_begin(ctx, "add-fields", 0)) {
    input_field(ctx, "URL", ui->url, sizeof(ui->url));
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Save to", NK_TEXT_LEFT);
    nk_layout_row_begin(ctx, NK_STATIC, 34, 3);
    nk_layout_row_push(ctx, 342);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->folder,
                                   sizeof(ui->folder), NULL);
    nk_layout_row_push(ctx, 8);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, 120);
    if (nk_button_label(ctx, "Choose folder")) {
      char *chosen =
          tinyfd_selectFolderDialog("Choose download folder", ui->folder);
      if (chosen)
        copy_text(ui->folder, sizeof(ui->folder), chosen);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 12, 1);
    nk_label(ctx, "", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 28, 1);
    if (nk_button_label(ctx, ui->advanced ? "- Advanced options"
                                          : "+ Advanced options"))
      ui->advanced = !ui->advanced;
    if (ui->advanced) {
      input_field(ctx, "Cookie", ui->cookie, sizeof(ui->cookie));
      input_field(ctx, "Referrer", ui->referrer, sizeof(ui->referrer));
      input_field(ctx, "Extra headers", ui->headers, sizeof(ui->headers));
      input_field(ctx, "SHA-256", ui->sha256, sizeof(ui->sha256));
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
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, "Queue", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 32, 1);
        selected = nk_combo(ctx, names, queue_count, selected, 30,
                            nk_vec2(260, 240));
        ui->add_queue_id = queues[selected].id;
      }
      number_field(ctx, "Speed limit (bytes/sec, 0 = unlimited)",
                   ui->numbers[5]);
    }
    nk_group_end(ctx);
  }
  nk_style_pop_style_item(ctx);
  text_at(ctx, 20, h - 76, w - 40, 22, ui->error, 11, nk_rgb(224, 132, 136),
          SURFACE);
  fill(ctx, screen_rect(ctx, 0, h - 54, w, 1), 0, BORDER);
  if (button(ctx, w - 232, h - 41, 80, 30, "Cancel", true, false))
    ui->add_open = false;
  if (button(ctx, w - 144, h - 41, 124, 30, "Add download", true, true)) {
    if (!ui->url[0] || !ui->folder[0])
      copy_text(ui->error, sizeof(ui->error),
                "Enter a URL and destination folder.");
    else if (!parse_number(ui->numbers[5], 0, 1000000000, &ui->speed_limit))
      copy_text(ui->error, sizeof(ui->error),
                "Speed limit must be between 0 and 1000000000.");
    else if (add_download(ui->url, ui->folder, ui->cookie, ui->referrer,
                          ui->headers, ui->sha256, (uint64_t)ui->speed_limit,
                          ui->add_queue_id)) {
      ui->add_open = false;
      ui->error[0] = '\0';
      ui->url[0] = ui->folder[0] = ui->cookie[0] = ui->referrer[0] = '\0';
      ui->headers[0] = ui->sha256[0] = '\0';
      ui->speed_limit = 0;
      ui->add_queue_id = 0;
      copy_text(ui->numbers[5], sizeof(ui->numbers[5]), "0");
      ui->advanced = false;
    } else
      copy_text(ui->error, sizeof(ui->error),
                "Could not prepare or enqueue the download.");
  }
  modal_end(ctx);
}

static bool queue_draft_valid(UiState *ui) {
  int priority = 0, cap = 0;
  if (!ui->queue_draft.name[0] ||
      !parse_number(ui->queue_priority, 0, 1000, &priority) ||
      !parse_number(ui->queue_cap, 0, 64, &cap)) {
    copy_text(ui->error, sizeof(ui->error),
              "Enter a name, priority 0-1000, and max concurrent 0-64.");
    return false;
  }
  if (!gui_schedule_valid(ui->queue_draft.schedule_start,
                          ui->queue_draft.schedule_stop)) {
    copy_text(ui->error, sizeof(ui->error),
              "Enter two different HH:MM times, or leave both empty.");
    return false;
  }
  if (ui->queue_draft.post_action[0] &&
      strcmp(ui->queue_draft.post_action, "none") != 0 &&
      !config_post_action_enabled(ui->queue_draft.post_action)) {
    copy_text(ui->error, sizeof(ui->error),
              "Enable this post-action in [post_actions] before saving.");
    return false;
  }
  if (strcmp(ui->queue_draft.post_action, "command") == 0 &&
      !ui->queue_draft.post_action_arg[0]) {
    copy_text(ui->error, sizeof(ui->error), "Enter a command to run.");
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
  input_field(ctx, "Name", ui->queue_draft.name,
              sizeof(ui->queue_draft.name));
  number_field(ctx, "Priority (0-1000)", ui->queue_priority);
  number_field(ctx, "Max concurrent (0 = unlimited)", ui->queue_cap);
  input_field(ctx, "Schedule start (HH:MM)", ui->queue_draft.schedule_start,
              sizeof(ui->queue_draft.schedule_start));
  input_field(ctx, "Schedule stop (HH:MM)", ui->queue_draft.schedule_stop,
              sizeof(ui->queue_draft.schedule_stop));
  nk_layout_row_dynamic(ctx, 20, 1);
  if (!ui->queue_draft.schedule_start[0] &&
      !ui->queue_draft.schedule_stop[0])
    nk_label_colored(ctx, "Always", NK_TEXT_LEFT, MUTED);
  else if (!gui_schedule_valid(ui->queue_draft.schedule_start,
                               ui->queue_draft.schedule_stop))
    nk_label_colored(ctx, "Use two different HH:MM times (00:00-23:59).",
                     NK_TEXT_LEFT, RED);
  else
    nk_label_colored(ctx, "Active during this time window", NK_TEXT_LEFT,
                     MUTED);
  static const char *actions[] = {"none", "shutdown", "sleep", "command"};
  static const char *labels[] = {"None", "Shut down", "Sleep", "Run command"};
  int selected = 0;
  for (int i = 1; i < 4; ++i)
    if (strcmp(ui->queue_draft.post_action, actions[i]) == 0)
      selected = i;
  nk_layout_row_dynamic(ctx, 22, 1);
  nk_label(ctx, "Post action", NK_TEXT_LEFT);
  nk_layout_row_dynamic(ctx, 34, 1);
  if (nk_combo_begin_label(ctx, labels[selected], nk_vec2(400, 150))) {
    nk_layout_row_dynamic(ctx, 30, 1);
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
  nk_layout_row_dynamic(ctx, 20, 1);
  if (selected && !config_post_action_enabled(actions[selected]))
    nk_label_colored(ctx,
        "Enable this action under [post_actions] in config.toml.",
        NK_TEXT_LEFT, MUTED);
  else
    nk_label_colored(ctx,
        "Power and command actions need explicit config enablement.",
        NK_TEXT_LEFT, MUTED);
  if (strcmp(ui->queue_draft.post_action, "command") == 0)
    input_field(ctx, "Command (executable and arguments)",
                ui->queue_draft.post_action_arg,
                sizeof(ui->queue_draft.post_action_arg));
}

static void draw_queue_add(struct nk_context *ctx, UiState *ui, float width,
                           float height) {
  float w = 520, h = height * .82f;
  if (h > 660)
    h = 660;
  if (!modal_start(ctx, "queue-add", "Add queue", (width - w) / 2,
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
  if (button(ctx, w - 230, h - 41, 80, 30, "Cancel", true, false))
    ui->queue_add_open = false;
  if (button(ctx, w - 142, h - 41, 122, 30, "Add queue", true, true) &&
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
  nk_layout_space_begin(ctx, NK_STATIC, 32, 1);
  text_at(ctx, 14, 0, 48, 30, "Order", 11, MUTED, BG);
  text_at(ctx, 68, 0, priority_x - 72, 30, "Name", 11, MUTED, BG);
  text_at(ctx, priority_x, 0, cap_x - priority_x, 30, "Priority", 11, MUTED,
          BG);
  text_at(ctx, cap_x, 0, schedule_x - cap_x, 30, "Max", 11, MUTED, BG);
  text_at(ctx, schedule_x, 0, action_x - schedule_x, 30, "Schedule", 11,
          MUTED, BG);
  text_at(ctx, action_x, 0, content - action_x - 120, 30, "Post action", 11,
          MUTED, BG);
  nk_layout_space_end(ctx);
  for (int i = 0; i < count; ++i) {
    Queue *queue = &queues[i];
    nk_layout_space_begin(ctx, NK_STATIC, 56, 1);
    struct nk_rect row = screen_rect(ctx, 8, 1, content, 52);
    fill(ctx, row, 8, SURFACE);
    button(ctx, 14, 12, 44, 30, "Drag", true, false);
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
      copy_text(schedule, sizeof(schedule), "Always");
    label(ctx, screen_rect(ctx, schedule_x, 10, action_x - schedule_x, 32),
          schedule, 12, MUTED, SURFACE);
    label(ctx, screen_rect(ctx, action_x, 10, content - action_x - 124, 32),
          queue->post_action, 12, MUTED, SURFACE);
    if (button(ctx, content - 112, 12, 48, 30, "Edit", true, false)) {
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
    else if (button(ctx, content - 60, 12, 54, 30, "Delete", true, false)) {
      ui->queue_delete_id = queue->id;
      ui->queue_delete_open = true;
    }
    nk_layout_space_end(ctx);
    if (ui->queue_edit_id == queue->id) {
      nk_layout_space_begin(ctx, NK_STATIC, 500, 1);
      nk_layout_space_push(ctx, nk_rect(14, 0, content - 28, 490));
      if (nk_group_begin(ctx, "queue-inline-edit", 0)) {
        queue_fields(ctx, ui);
        nk_layout_row_dynamic(ctx, 32, 2);
        if (nk_button_label(ctx, "Cancel"))
          ui->queue_edit_id = 0;
        if (nk_button_label(ctx, "Save") && queue_draft_valid(ui)) {
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
  if (!modal_start(ctx, "queue-delete", "Delete queue", (width - w) / 2,
                   (height - h) / 2, w, h, &ui->queue_delete_open)) {
    nk_end(ctx);
    return;
  }
  text_at(ctx, 20, 78, w - 40, 48,
          "Downloads in this queue will move to Default.", 13, TEXT,
          SURFACE);
  if (button(ctx, w - 222, h - 43, 84, 30, "Cancel", true, false))
    ui->queue_delete_open = false;
  if (button(ctx, w - 132, h - 43, 112, 30, "Delete", true, true)) {
    bool queued = gui_controller_enqueue_queue_delete(ui->queue_delete_id);
    report_enqueue(ui, queued);
    if (queued)
      ui->queue_delete_open = false;
  }
  modal_end(ctx);
}

static void draw_details(struct nk_context *ctx, UiState *ui, float width,
                         float height) {
  float w = 580, h = 390;
  if (!modal_start(ctx, "details", "Download details", (width - w) / 2,
                   (height - h) / 2, w, h, &ui->details_open)) {
    nk_end(ctx);
    return;
  }
  nk_layout_space_push(ctx, nk_rect(20, 74, w - 40, h - 130));
  if (nk_group_begin(ctx, "details-fields", 0)) {
    nk_layout_row_dynamic(ctx, 26, 1);
    if (!ui->details_found)
      nk_label(ctx, "Loading details...", NK_TEXT_LEFT);
    else {
      nk_labelf(ctx, NK_TEXT_LEFT, "ID %u  |  Speed limit: %llu bytes/sec",
                ui->selected_id,
                (unsigned long long)ui->details.speed_limit_bps);
      nk_labelf_wrap(ctx, "Cookie: %s", ui->details.cookie);
      nk_labelf_wrap(ctx, "Referrer: %s", ui->details.referrer);
      nk_labelf_wrap(ctx, "Headers: %s", ui->details.extra_headers);
      nk_labelf_wrap(ctx, "SHA-256: %s", ui->details.expected_sha256);
    }
    nk_group_end(ctx);
  }
  text_at(ctx, 20, h - 56, w - 125, 36, ui->error, 11, nk_rgb(224, 132, 136),
          SURFACE);
  if (button(ctx, w - 100, h - 46, 80, 30, "Close", true, false))
    ui->details_open = false;
  modal_end(ctx);
}

static void draw_delete_confirmation(struct nk_context *ctx, UiState *ui,
                                     float width, float height) {
  float w = 520, h = 230;
  if (!modal_start(ctx, "delete-file", "Delete download file",
                   (width - w) / 2, (height - h) / 2, w, h,
                   &ui->delete_confirm_open)) {
    nk_end(ctx);
    return;
  }
  text_at(ctx, 20, 76, w - 40, 22,
          "Delete this download record and its file from disk?", 12, TEXT,
          SURFACE);
  text_at(ctx, 20, 111, w - 40, 22, ui->delete_confirm_path, 11, MUTED,
          SURFACE);
  fill(ctx, screen_rect(ctx, 0, h - 54, w, 1), 0, BORDER);
  if (button(ctx, w - 232, h - 41, 80, 30, "Cancel", true, false))
    ui->delete_confirm_open = false;
  if (button(ctx, w - 144, h - 41, 124, 30, "Delete file", true, true)) {
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
      bold_at(ctx, 16, 12, 308, 20, "URL copied to clipboard", 13,
              ACCENT, SURFACE2);
      text_at(ctx, 16, 38, 308, 20, ui->clipboard_offer, 11, TEXT,
              SURFACE2);
      if (button(ctx, 16, 74, 148, 30, "Review download", true, true)) {
        copy_text(ui->url, sizeof(ui->url), ui->clipboard_offer);
        ui->add_open = true;
        ui->clipboard_offer_open = false;
      }
      if (button(ctx, 176, 74, 148, 30, "Dismiss", true, false))
        ui->clipboard_offer_open = false;
      nk_layout_space_end(ctx);
    }
    nk_end(ctx);
  }
  if (ui->duplicate_toast_open) {
    if (nk_begin(ctx, "duplicate-toast",
                 nk_rect(width - 300, height - 100, 280, 80),
                 NK_WINDOW_NO_SCROLLBAR)) {
      nk_layout_row_dynamic(ctx, 24, 1);
      nk_labelf(ctx, NK_TEXT_LEFT, "Already downloading (ID %u)",
                ui->duplicate_id);
      nk_layout_row_dynamic(ctx, 28, 1);
      if (nk_button_label(ctx, "Close"))
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
    bold_at(ctx, 16, 14, 220, 20, "Download complete", 13, GREEN, SURFACE2);
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
    if (button(ctx, 16, 100, 120, 30, "Open", true, true))
      open_path(ui, ui->toast.dest_path, false);
    if (button(ctx, 144, 100, 120, 30, "Open folder", true, false))
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
    }
  }
}

int run_gui(void) {
  if (!gui_client_connect()) {
    fprintf(stderr, "Cannot connect to daemon.\n");
    return 1;
  }
  gui_model_init();
  GuiSdlBackendConfig config = {.width = 1100,
                                .height = 720,
                                .title = "Core Download Manager",
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
  apply_theme(ctx);
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
    int category_counts[CATEGORY_COUNT] = {0};
    category_counts[CATEGORY_ALL] = n;
    for (int i = 0; i < n; ++i) {
      GuiCategory category = category_for_row(&rows[i]);
      if (category != CATEGORY_ALL)
        ++category_counts[category];
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
    bool modal = ui.settings_open || ui.add_open || ui.details_open ||
                 ui.delete_confirm_open || ui.queue_add_open ||
                 ui.queue_delete_open;
    if (modal) {
      float mw = ui.settings_open ? 460
                 : ui.queue_delete_open ? 480
                 : ui.add_open || ui.delete_confirm_open || ui.queue_add_open
                     ? 520 : 580;
      float mh =
          ui.settings_open ? (height * .8f > 650 ? 650 : height * .8f)
          : ui.delete_confirm_open ? 230
          : ui.queue_delete_open ? 220
          : ui.queue_add_open ? (height * .82f > 660 ? 660 : height * .82f)
          : ui.add_open
              ? (ui.advanced ? (height * .85f > 700 ? 700 : height * .85f)
                             : 350)
              : 390;
      struct nk_rect modal_bounds =
          nk_rect((width - mw) / 2, (height - mh) / 2, mw, mh);
      if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_ESCAPE] ||
          (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
           !nk_input_is_mouse_hovering_rect(&ctx->input, modal_bounds))) {
        ui.settings_open = ui.add_open = ui.details_open =
            ui.delete_confirm_open = false;
        ui.queue_add_open = ui.queue_delete_open = false;
        modal = false;
      }
    }
    gui_sdl_backend_begin_frame(backend);
    if (nk_begin(ctx, "Download Manager", nk_rect(0, 0, width, height),
                 NK_WINDOW_NO_SCROLLBAR | (modal ? NK_WINDOW_NO_INPUT : 0))) {
      nk_layout_space_begin(ctx, NK_STATIC, height, 32);
      draw_chrome(ctx, &ui, category_counts, visible_count, width, height);
      if (ui.tab == TAB_QUEUES)
        draw_queues(ctx, &ui, width, height);
      else {
        draw_rows(ctx, &ui, visible, visible_count, width, height);
        draw_toolbar(ctx, &ui, visible, visible_count);
      }
      if (ui.error[0]) {
        text_at(ctx, 236, height - 34, width - 290, 32, ui.error, 12,
                nk_rgb(224, 132, 136), BG);
        if (close_button(ctx, width - 42, height - 31))
          ui.error[0] = '\0';
      }
      nk_layout_space_end(ctx);
      if (!modal)
        draw_menu(ctx, &ui, visible, visible_count, width, height);
    }
    nk_end(ctx);
    if (!modal)
      draw_toast(ctx, &ui, width, height);
    if (ui.settings_open || ui.add_open || ui.details_open ||
        ui.delete_confirm_open || ui.queue_add_open || ui.queue_delete_open) {
      modal_backdrop(ctx, width, height);
      if (ui.settings_open)
        draw_settings(ctx, &ui, width, height);
      else if (ui.add_open)
        draw_add(ctx, &ui, width, height);
      else if (ui.delete_confirm_open)
        draw_delete_confirmation(ctx, &ui, width, height);
      else if (ui.queue_add_open)
        draw_queue_add(ctx, &ui, width, height);
      else if (ui.queue_delete_open)
        draw_queue_delete(ctx, &ui, width, height);
      else
        draw_details(ctx, &ui, width, height);
    }
    gui_sdl_backend_end_frame(backend);
    SDL_Delay(8);
  }
  gui_controller_stop();
  gui_sdl_backend_destroy(backend);
  gui_client_disconnect();
  return 0;
}
