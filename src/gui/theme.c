// SPDX-License-Identifier: MIT
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#include "theme.h"
#ifdef __linux__
#include <gio/gio.h>
#endif
#include <math.h>

/* Keep this palette byte-for-byte equal to the former fixed GUI colors. */
static const ThemePalette dark = {
  .bg = {11, 15, 20, 255}, .surface = {17, 23, 34, 255},
  .surface2 = {22, 30, 44, 255}, .border = {35, 45, 61, 255},
  .text = {232, 236, 241, 255}, .muted = {124, 138, 157, 255},
  .accent = {61, 157, 207, 255}, .accent_dim = {31, 92, 128, 255},
  .green = {63, 143, 95, 255}, .red = {163, 68, 72, 255},
  .amber = {184, 130, 63, 255}, .white = {255, 255, 255, 255},
  .disabled = {80, 93, 111, 255},
  .accent_hover = {75, 171, 224, 255},
  .status_blue = {61, 127, 176, 255},
  .status_purple = {122, 95, 176, 255},
  .status_orange = {192, 85, 63, 255},
  .status_green = {63, 143, 111, 255},
  .status_gold = {176, 135, 61, 255},
  .status_done = {111, 189, 140, 255},
  .status_error = {224, 132, 136, 255},
  .modal_clear = {0, 0, 0, 0}, .modal_shade = {4, 6, 9, 140},
};

static const ThemePalette light = {
  .bg = {245, 247, 250, 255}, .surface = {255, 255, 255, 255},
  .surface2 = {237, 241, 245, 255}, .border = {185, 197, 211, 255},
  .text = {23, 36, 52, 255}, .muted = {82, 100, 122, 255},
  .accent = {18, 104, 147, 255}, .accent_dim = {9, 87, 120, 255},
  .green = {38, 114, 69, 255}, .red = {161, 63, 71, 255},
  .amber = {138, 92, 22, 255}, .white = {255, 255, 255, 255},
  .disabled = {101, 117, 138, 255},
  .accent_hover = {15, 91, 130, 255},
  .status_blue = {30, 91, 143, 255},
  .status_purple = {99, 70, 143, 255},
  .status_orange = {151, 73, 43, 255},
  .status_green = {35, 111, 71, 255},
  .status_gold = {130, 89, 20, 255},
  .status_done = {32, 104, 62, 255},
  .status_error = {150, 45, 52, 255},
  .modal_clear = {0, 0, 0, 0}, .modal_shade = {4, 6, 9, 140},
};

/* Single owner: the SDL/Nuklear UI thread calls theme_apply and draws widgets. */
static const ThemePalette *current = &dark;

const ThemePalette *theme_current_palette(void) { return current; }

static double linear_channel(unsigned char channel) {
  double srgb = (double)channel / 255.0;
  return srgb <= 0.04045 ? srgb / 12.92
                         : pow((srgb + 0.055) / 1.055, 2.4);
}

struct nk_color theme_contrast_ink(struct nk_color background) {
  double luminance = 0.2126 * linear_channel(background.r) +
                     0.7152 * linear_channel(background.g) +
                     0.0722 * linear_channel(background.b);
  /* WCAG AA: black or white against an opaque sRGB color is >= 4.5:1. */
  return luminance >= 0.179 ? (struct nk_color){0, 0, 0, 255}
                            : (struct nk_color){255, 255, 255, 255};
}

ThemeId theme_detect_desktop(void) {
#ifdef __linux__
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus) {
    g_clear_error(&error);
    return THEME_LIGHT;
  }
  GVariant *reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Settings", "ReadOne",
      g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, &error);
  g_object_unref(bus);
  if (!reply) {
    g_clear_error(&error);
    return THEME_LIGHT;
  }
  GVariant *value = NULL;
  g_variant_get(reply, "(v)", &value);
  ThemeId result = g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32) &&
                   g_variant_get_uint32(value) == 1 ? THEME_DARK : THEME_LIGHT;
  g_variant_unref(value);
  g_variant_unref(reply);
  return result;
#else
  /* TODO(platform): query the native desktop appearance preference. */
  return THEME_LIGHT;
#endif
}

void theme_apply(struct nk_context *ctx, ThemeId id) {
  if (id == THEME_SYSTEM)
    id = theme_detect_desktop();
  current = id == THEME_DARK ? &dark : &light;
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
