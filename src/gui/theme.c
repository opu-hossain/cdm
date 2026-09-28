// SPDX-License-Identifier: MIT
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#include "theme.h"

const struct nk_color BG = {11, 15, 20, 255};
const struct nk_color SURFACE = {17, 23, 34, 255};
const struct nk_color SURFACE2 = {22, 30, 44, 255};
const struct nk_color BORDER = {35, 45, 61, 255};
const struct nk_color TEXT = {232, 236, 241, 255};
const struct nk_color MUTED = {124, 138, 157, 255};
const struct nk_color ACCENT = {61, 157, 207, 255};
const struct nk_color ACCENT_DIM = {31, 92, 128, 255};
const struct nk_color GREEN = {63, 143, 95, 255};
const struct nk_color RED = {163, 68, 72, 255};
const struct nk_color AMBER = {184, 130, 63, 255};
const struct nk_color WHITE = {255, 255, 255, 255};
const struct nk_color DISABLED = {80, 93, 111, 255};
const struct nk_color ACCENT_HOVER = {75, 171, 224, 255};
const struct nk_color STATUS_BLUE = {61, 127, 176, 255};
const struct nk_color STATUS_PURPLE = {122, 95, 176, 255};
const struct nk_color STATUS_ORANGE = {192, 85, 63, 255};
const struct nk_color STATUS_GREEN = {63, 143, 111, 255};
const struct nk_color STATUS_GOLD = {176, 135, 61, 255};
const struct nk_color STATUS_DONE = {111, 189, 140, 255};
const struct nk_color STATUS_ERROR = {224, 132, 136, 255};
const struct nk_color MODAL_CLEAR = {0, 0, 0, 0};
const struct nk_color MODAL_SHADE = {4, 6, 9, 140};

void theme_apply(struct nk_context *ctx, ThemeId id) {
  (void)id;
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
