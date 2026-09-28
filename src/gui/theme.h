// SPDX-License-Identifier: MIT
#ifndef CDM_GUI_THEME_H
#define CDM_GUI_THEME_H

#include "nuklear.h"

typedef enum {
  THEME_SYSTEM = 0,
  THEME_LIGHT,
  THEME_DARK,
  THEME_DEFAULT = THEME_SYSTEM,
} ThemeId;

typedef struct {
  struct nk_color bg, surface, surface2, border, text, muted;
  struct nk_color accent, accent_dim, green, red, amber, white;
  struct nk_color disabled, accent_hover, status_blue, status_purple;
  struct nk_color status_orange, status_green, status_gold, status_done;
  struct nk_color status_error, modal_clear, modal_shade;
} ThemePalette;

/* GUI main thread alone calls theme_apply and reads the current palette. */
const ThemePalette *theme_current_palette(void);
ThemeId theme_detect_desktop(void); // light when no portal preference is available
struct nk_color theme_contrast_ink(struct nk_color background);

#define BG (theme_current_palette()->bg)
#define SURFACE (theme_current_palette()->surface)
#define SURFACE2 (theme_current_palette()->surface2)
#define BORDER (theme_current_palette()->border)
#define TEXT (theme_current_palette()->text)
#define MUTED (theme_current_palette()->muted)
#define ACCENT (theme_current_palette()->accent)
#define ACCENT_DIM (theme_current_palette()->accent_dim)
#define GREEN (theme_current_palette()->green)
#define RED (theme_current_palette()->red)
#define AMBER (theme_current_palette()->amber)
#define WHITE (theme_current_palette()->white)
#define DISABLED (theme_current_palette()->disabled)
#define ACCENT_HOVER (theme_current_palette()->accent_hover)
#define STATUS_BLUE (theme_current_palette()->status_blue)
#define STATUS_PURPLE (theme_current_palette()->status_purple)
#define STATUS_ORANGE (theme_current_palette()->status_orange)
#define STATUS_GREEN (theme_current_palette()->status_green)
#define STATUS_GOLD (theme_current_palette()->status_gold)
#define STATUS_DONE (theme_current_palette()->status_done)
#define STATUS_ERROR (theme_current_palette()->status_error)
#define MODAL_CLEAR (theme_current_palette()->modal_clear)
#define MODAL_SHADE (theme_current_palette()->modal_shade)

enum {
  THEME_ROW_TIGHT_SPACER = 8,
  THEME_ROW_SPACER = 10,
  THEME_ROW_WIDE_SPACER = 12,
  THEME_ROW_CAPTION = 20,
  THEME_ROW_LABEL = 22,
  THEME_ROW_TEXT = 24,
  THEME_ROW_CHOICE = 26,
  THEME_ROW_SECTION = 28,
  THEME_ROW_ACTION = 30,
  THEME_ROW_BUTTON = 32,
  THEME_ROW_INPUT = 34,
  THEME_ROW_DETAIL = 36,
  THEME_ROW_PREVIEW = 100,
  THEME_COLUMN_GAP = 8,
};

void theme_apply(struct nk_context *ctx, ThemeId id);

#endif
