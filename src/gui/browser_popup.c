// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Opu Hossain

#include "browser_popup.h"

#include "gui_backend_sdl.h"
#include "gui_model.h"
#include "../platform/file_io.h"
#include "../platform/ipc_socket.h"
#include "../platform/open_path.h"
#include "../utils/config.h"
#include "../utils/path.h"
#include "../vendor/tinyfiledialogs.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#include "nuklear.h"

static const struct nk_color BG = {11, 15, 20, 255};
static const struct nk_color SURFACE = {17, 23, 34, 255};
static const struct nk_color SURFACE2 = {22, 30, 44, 255};
static const struct nk_color BORDER = {35, 45, 61, 255};
static const struct nk_color TEXT = {232, 236, 241, 255};
static const struct nk_color MUTED = {124, 138, 157, 255};
static const struct nk_color ACCENT = {61, 157, 207, 255};
static const struct nk_color GREEN = {63, 143, 95, 255};

static void report_popup_ready(void) {
  const char *value = getenv("CDM_BROWSER_READY_FD");
  if (!value)
    return;
  char *end = NULL;
  long fd = strtol(value, &end, 10);
  if (end && !*end && fd >= 3 && fd <= 1024) {
    char marker = 'R';
    write((int)fd, &marker, 1);
    close((int)fd);
  }
  unsetenv("CDM_BROWSER_READY_FD");
}

typedef struct {
  IpcBrowserOffer offer;
  uint32_t download_id;
  uint16_t daemon_version;
  bool duplicate;
  char filename[IPC_BROWSER_FILENAME_MAX];
  char folder[IPC_MAX_PATH_LEN];
  char full_path[IPC_MAX_PATH_LEN];
  char error[256];
  IpcBrowserProgress progress;
  IpcProgressV2 rich_progress;
  bool has_v2;
  int progress_sock;
  unsigned char progress_frame[
      sizeof(MsgHeader) +
      (sizeof(IpcBrowserProgress) > sizeof(IpcProgressV2)
           ? sizeof(IpcBrowserProgress)
           : sizeof(IpcProgressV2))];
  size_t progress_frame_bytes;
  uint32_t last_sample_tick;
  uint32_t next_retry_tick;
  uint64_t last_sample_bytes;
  double speed_bps;
  bool close;
} PopupState;

static void popup_theme(struct nk_context *ctx) {
  struct nk_color colors[NK_COLOR_COUNT];
  for (int i = 0; i < NK_COLOR_COUNT; i++)
    colors[i] = SURFACE2;
  colors[NK_COLOR_TEXT] = TEXT;
  colors[NK_COLOR_WINDOW] = SURFACE;
  colors[NK_COLOR_HEADER] = SURFACE;
  colors[NK_COLOR_BORDER] = BORDER;
  colors[NK_COLOR_BUTTON] = SURFACE2;
  colors[NK_COLOR_BUTTON_HOVER] = BORDER;
  colors[NK_COLOR_BUTTON_ACTIVE] = ACCENT;
  colors[NK_COLOR_EDIT] = BG;
  colors[NK_COLOR_EDIT_CURSOR] = TEXT;
  nk_style_from_table(ctx, colors);
  ctx->style.progress.normal = nk_style_item_color(BG);
  ctx->style.progress.hover = nk_style_item_color(BG);
  ctx->style.progress.active = nk_style_item_color(BG);
  ctx->style.progress.cursor_normal = nk_style_item_color(ACCENT);
  ctx->style.progress.cursor_hover = nk_style_item_color(ACCENT);
  ctx->style.progress.cursor_active = nk_style_item_color(ACCENT);
  ctx->style.window.padding = nk_vec2(20, 16);
  ctx->style.window.spacing = nk_vec2(8, 6);
  ctx->style.window.border = 0;
  ctx->style.button.rounding = 7;
  ctx->style.button.padding = nk_vec2(7, 4);
  ctx->style.edit.rounding = 7;
  ctx->style.edit.padding = nk_vec2(10, 8);
}

static bool primary_button(struct nk_context *ctx, const char *label) {
  nk_style_push_style_item(ctx, &ctx->style.button.normal,
                           nk_style_item_color(ACCENT));
  nk_style_push_style_item(ctx, &ctx->style.button.hover,
                           nk_style_item_color(nk_rgb(83, 176, 222)));
  nk_style_push_style_item(ctx, &ctx->style.button.active,
                           nk_style_item_color(nk_rgb(31, 92, 128)));
  nk_style_push_color(ctx, &ctx->style.button.text_normal, BG);
  bool clicked = nk_button_label(ctx, label) != 0;
  nk_style_pop_color(ctx);
  nk_style_pop_style_item(ctx);
  nk_style_pop_style_item(ctx);
  nk_style_pop_style_item(ctx);
  return clicked;
}

static bool filename_valid(const char *filename) {
  return filename[0] && strcmp(filename, ".") != 0 &&
         strcmp(filename, "..") != 0 && !strchr(filename, '/') &&
         !strchr(filename, '\\');
}

static void draw_confirmation(struct nk_context *ctx, PopupState *state,
                              int daemon) {
  nk_layout_row_dynamic(ctx, 28, 1);
  nk_label(ctx, "Review download", NK_TEXT_LEFT);
  nk_layout_row_dynamic(ctx, 18, 1);
  nk_label_colored(ctx, "Download URL", NK_TEXT_LEFT, MUTED);
  nk_layout_row_dynamic(ctx, 43, 1);
  nk_label_wrap(ctx, state->offer.url);
  nk_layout_row_dynamic(ctx, 18, 1);
  nk_label_colored(ctx, "Save as", NK_TEXT_LEFT, MUTED);
  nk_layout_row_dynamic(ctx, 34, 1);
  nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, state->filename,
                                  sizeof(state->filename), NULL);
  nk_layout_row_dynamic(ctx, 18, 1);
  nk_label_colored(ctx, "Destination folder", NK_TEXT_LEFT, MUTED);
  nk_layout_row_begin(ctx, NK_STATIC, 34, 2);
  nk_layout_row_push(ctx, 325);
  nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, state->folder,
                                  sizeof(state->folder), NULL);
  nk_layout_row_push(ctx, 90);
  if (nk_button_label(ctx, "Browse...")) {
    char *chosen = tinyfd_selectFolderDialog("Choose download folder",
                                              state->folder);
    if (chosen)
      snprintf(state->folder, sizeof(state->folder), "%s", chosen);
  }
  nk_layout_row_end(ctx);
  char size_label[80];
  if (state->offer.total_bytes)
    snprintf(size_label, sizeof(size_label), "File size: %.1f MB",
             (double)state->offer.total_bytes / 1000000.0);
  else
    snprintf(size_label, sizeof(size_label), "File size: unknown");
  nk_layout_row_dynamic(ctx, 18, 1);
  nk_label_colored(ctx, size_label, NK_TEXT_LEFT, MUTED);
  nk_layout_row_dynamic(ctx, 20, 1);
  nk_label_colored(ctx, state->error, NK_TEXT_LEFT,
                   nk_rgb(224, 132, 136));
  nk_layout_row_begin(ctx, NK_STATIC, 34, 3);
  nk_layout_row_push(ctx, 230);
  nk_spacing(ctx, 1);
  nk_layout_row_push(ctx, 88);
  if (nk_button_label(ctx, "Cancel")) {
    ipc_browser_dismiss(daemon, state->offer.offer_id);
    state->close = true;
  }
  nk_layout_row_push(ctx, 97);
  if (primary_button(ctx, "Download")) {
    if (!filename_valid(state->filename) || !state->folder[0] ||
        !path_join(state->folder, state->filename, state->full_path,
                   sizeof(state->full_path))) {
      snprintf(state->error, sizeof(state->error),
               "Choose a valid folder and filename.");
    } else {
      IpcAddResponse response = {0};
      int result = state->daemon_version >= 3
          ? ipc_browser_confirm_v2(daemon, state->offer.offer_id,
                                   state->full_path, &response)
          : ipc_browser_confirm(daemon, state->offer.offer_id,
                                state->full_path, &response.id);
      state->download_id = response.id;
      state->duplicate = response.result == IPC_RESULT_REJECTED;
      if (result != 0)
      snprintf(state->error, sizeof(state->error),
               "Could not start download. Check the destination.");
      else
        state->error[0] = '\0';
    }
  }
  nk_layout_row_end(ctx);
}

static void update_progress(PopupState *state,
                            const IpcBrowserProgress *progress) {
  uint32_t now = SDL_GetTicks();
  if (!state->has_v2 && state->last_sample_tick &&
      now != state->last_sample_tick &&
      progress->bytes_received >= state->last_sample_bytes) {
    double elapsed = (double)(now - state->last_sample_tick) / 1000.0;
    if (elapsed >= 0.2)
      state->speed_bps =
          (double)(progress->bytes_received - state->last_sample_bytes) /
          elapsed;
  }
  state->last_sample_tick = now;
  state->last_sample_bytes = progress->bytes_received;
  state->progress = *progress;
  if (progress->dest_path[0]) {
    snprintf(state->full_path, sizeof(state->full_path), "%s",
             progress->dest_path);
    const char *base = strrchr(state->full_path, '/');
    const char *name = base ? base + 1 : state->full_path;
    size_t length = strnlen(name, sizeof(state->filename) - 1);
    memcpy(state->filename, name, length);
    state->filename[length] = '\0';
  }
}

static void connect_progress(PopupState *state) {
  uint16_t version = 1;
  int sock = ipc_client_connect_compatible(800, &version);
  if (sock < 0)
    goto retry;
  IpcBrowserProgress initial = {0};
  if (ipc_browser_subscribe_progress(sock, state->download_id, &initial) != 0) {
    ipc_client_disconnect(sock);
    goto retry;
  }
  if (version >= 2 && ipc_send_subscribe_v2(sock) != 0) {
    ipc_client_disconnect(sock);
    goto retry;
  }
  state->progress_sock = sock;
  state->progress_frame_bytes = 0;
  state->has_v2 = false;
  state->last_sample_tick = 0;
  update_progress(state, &initial);
  state->error[0] = '\0';
  return;
retry:
  state->progress_sock = -1;
  state->next_retry_tick = SDL_GetTicks() + 1000;
  snprintf(state->error, sizeof(state->error),
           "Progress connection lost; retrying...");
}

static void poll_progress(PopupState *state) {
  if (state->progress_sock < 0) {
    if ((int32_t)(SDL_GetTicks() - state->next_retry_tick) >= 0)
      connect_progress(state);
    return;
  }
  for (int i = 0; i < 16; i++) {
    size_t target = sizeof(MsgHeader);
    MsgHeader header = {0};
    if (state->progress_frame_bytes >= sizeof(header)) {
      memcpy(&header, state->progress_frame, sizeof(header));
      if ((header.type != MSG_BROWSER_PROGRESS_EVENT ||
           header.length != sizeof(IpcBrowserProgress)) &&
          (header.type != MSG_STATUS_EVENT_V2 ||
           header.length != sizeof(IpcProgressV2)))
        break;
      target += header.length;
    }
    size_t need = target - state->progress_frame_bytes;
    ssize_t n = recv(state->progress_sock,
                     state->progress_frame + state->progress_frame_bytes,
                     need, MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return;
      break;
    }
    if (n == 0)
      break;
    state->progress_frame_bytes += (size_t)n;
    if (state->progress_frame_bytes >= sizeof(header)) {
      memcpy(&header, state->progress_frame, sizeof(header));
      if ((header.type != MSG_BROWSER_PROGRESS_EVENT ||
           header.length != sizeof(IpcBrowserProgress)) &&
          (header.type != MSG_STATUS_EVENT_V2 ||
           header.length != sizeof(IpcProgressV2)))
        break;
      if (state->progress_frame_bytes == sizeof(header) + header.length) {
        if (header.type == MSG_STATUS_EVENT_V2) {
          IpcProgressV2 rich;
          memcpy(&rich, state->progress_frame + sizeof(header), sizeof(rich));
          if (rich.download_id == state->download_id) {
            state->rich_progress = rich;
            state->has_v2 = true;
          }
        } else {
          IpcBrowserProgress event;
          memcpy(&event, state->progress_frame + sizeof(header), sizeof(event));
          update_progress(state, &event);
        }
        state->progress_frame_bytes = 0;
      }
    }
  }
  ipc_client_disconnect(state->progress_sock);
  state->progress_sock = -1;
  state->next_retry_tick = SDL_GetTicks() + 1000;
  snprintf(state->error, sizeof(state->error),
           "Progress connection lost; retrying...");
}

static void draw_progress(struct nk_context *ctx, PopupState *state,
                          int daemon) {
  bool done = strcmp(state->progress.status, "DONE") == 0;
  bool stopped = strcmp(state->progress.status, "ERROR") == 0 ||
                 strcmp(state->progress.status, "CANCELED") == 0;
  nk_layout_row_dynamic(ctx, 30, 1);
  nk_label(ctx, done ? "Download complete" : "Downloading", NK_TEXT_LEFT);
  if (state->duplicate) {
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_labelf_colored(ctx, NK_TEXT_LEFT, ACCENT,
                      "Already downloading (ID %u)", state->download_id);
  }
  nk_layout_row_dynamic(ctx, 24, 1);
  nk_label(ctx, state->filename, NK_TEXT_LEFT);
  nk_layout_row_dynamic(ctx, 30, 1);
  char amount[128];
  if (state->progress.total_bytes)
    snprintf(amount, sizeof(amount), "%.1f MB of %.1f MB (%.0f%%)",
             (double)state->progress.bytes_received / 1000000.0,
             (double)state->progress.total_bytes / 1000000.0,
             state->progress.progress * 100.0);
  else
    snprintf(amount, sizeof(amount), "%.1f MB downloaded",
             (double)state->progress.bytes_received / 1000000.0);
  nk_label(ctx, amount, NK_TEXT_LEFT);
  nk_size amount_progress =
      (nk_size)(state->progress.progress * 1000.0f);
  if (amount_progress > 1000) amount_progress = 1000;
  nk_layout_row_dynamic(ctx, 12, 1);
  nk_progress(ctx, &amount_progress, 1000, nk_false);
  char speed[80], eta[80];
  if (state->has_v2) {
    char amount_per_second[32], formatted_eta[32];
    gui_format_bytes(state->rich_progress.speed_bps, amount_per_second,
                     sizeof(amount_per_second));
    snprintf(speed, sizeof(speed), "Speed: %s/s", amount_per_second);
    gui_format_eta(state->rich_progress.eta_seconds, formatted_eta,
                   sizeof(formatted_eta));
    snprintf(eta, sizeof(eta), "Time remaining: %s",
             state->rich_progress.eta_seconds == UINT64_MAX
                 ? "unknown"
                 : formatted_eta);
  } else {
    snprintf(speed, sizeof(speed), "Speed: %.1f MB/s",
             state->speed_bps / 1000000.0);
    if (!done && state->speed_bps > 0 && state->progress.total_bytes >
                                                state->progress.bytes_received) {
      double seconds = (double)(state->progress.total_bytes -
                                state->progress.bytes_received) /
                       state->speed_bps;
      snprintf(eta, sizeof(eta), "Time remaining: %.0f sec", seconds);
    } else {
      snprintf(eta, sizeof(eta), "Time remaining: unknown");
    }
  }
  nk_layout_row_dynamic(ctx, 20, 2);
  nk_label_colored(ctx, speed, NK_TEXT_LEFT, MUTED);
  nk_label_colored(ctx, eta, NK_TEXT_LEFT, MUTED);
  nk_layout_row_dynamic(ctx, 24, 1);
  if (done)
    nk_label_colored(ctx, "Download complete", NK_TEXT_LEFT, GREEN);
  else if (stopped)
    nk_label_colored(ctx, state->progress.error[0]
                             ? state->progress.error
                             : state->progress.status,
                     NK_TEXT_LEFT, nk_rgb(224, 132, 136));
  else
    nk_label_colored(ctx, state->error, NK_TEXT_LEFT,
                     nk_rgb(224, 132, 136));
  nk_layout_row_dynamic(ctx, 34, done ? 3 : 2);
  if (done) {
    if (primary_button(ctx, "Open file") &&
        platform_open_path(state->full_path) != 0)
      snprintf(state->error, sizeof(state->error), "Could not open file.");
    if (nk_button_label(ctx, "Open folder")) {
      char folder[IPC_MAX_PATH_LEN];
      snprintf(folder, sizeof(folder), "%s", state->full_path);
      char *slash = strrchr(folder, '/');
      if (!slash || slash == folder)
        snprintf(folder, sizeof(folder), "/");
      else
        *slash = '\0';
      if (platform_open_path(folder) != 0)
        snprintf(state->error, sizeof(state->error), "Could not open folder.");
    }
    if (nk_button_label(ctx, "Close")) state->close = true;
  } else {
    if (!stopped && nk_button_label(ctx, "Cancel download"))
      ipc_send_cancel(daemon, state->download_id);
    if (nk_button_label(ctx, "Close")) state->close = true;
  }
  if (state->error[0] && done) {
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, state->error, NK_TEXT_LEFT,
                     nk_rgb(224, 132, 136));
  }
}

int run_browser_popup(uint32_t offer_id) {
  uint16_t daemon_version = 1;
  int daemon = ipc_client_connect_compatible(1500, &daemon_version);
  if (daemon < 0)
    return 1;
  PopupState state = {.progress_sock = -1};
  state.daemon_version = daemon_version;
  if (ipc_browser_get_offer(daemon, offer_id, &state.offer) != 0 ||
      state.offer.state != IPC_BROWSER_WAITING) {
    ipc_client_disconnect(daemon);
    return 1;
  }
  snprintf(state.filename, sizeof(state.filename), "%s",
           state.offer.filename);
  snprintf(state.folder, sizeof(state.folder), "%s",
           config_get_default_download_dir());
  file_ensure_directory(state.folder);
  GuiSdlBackendConfig settings = {.width = 480, .height = 370,
                                  .title = "cdm — Browser download",
                                  .font_size = 13};
  GuiSdlBackend *backend = gui_sdl_backend_create(&settings);
  if (!backend) {
    ipc_client_disconnect(daemon);
    return 1;
  }
  struct nk_context *ctx = gui_sdl_backend_context(backend);
  popup_theme(ctx);
  report_popup_ready();
  while (!state.close && gui_sdl_backend_poll(backend)) {
    if (state.download_id)
      poll_progress(&state);
    gui_sdl_backend_begin_frame(backend);
    if (nk_begin(ctx, "browser-popup", nk_rect(0, 0, 480, 370),
                 NK_WINDOW_NO_SCROLLBAR)) {
      if (state.download_id)
        draw_progress(ctx, &state, daemon);
      else
        draw_confirmation(ctx, &state, daemon);
    }
    nk_end(ctx);
    gui_sdl_backend_end_frame(backend);
    SDL_Delay(8);
  }
  if (!state.download_id && !state.close)
    ipc_browser_dismiss(daemon, state.offer.offer_id);
  gui_sdl_backend_destroy(backend);
  if (state.progress_sock >= 0)
    ipc_client_disconnect(state.progress_sock);
  ipc_client_disconnect(daemon);
  return 0;
}
