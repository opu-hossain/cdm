// SPDX-License-Identifier: MIT
#ifndef CDM_GUI_THEME_H
#define CDM_GUI_THEME_H

#include "nuklear.h"

typedef enum { THEME_DEFAULT = 0 } ThemeId;

/* The original GUI palette. Keep these values stable for the default theme. */
extern const struct nk_color BG, SURFACE, SURFACE2, BORDER, TEXT, MUTED;
extern const struct nk_color ACCENT, ACCENT_DIM, GREEN, RED, AMBER, WHITE;
extern const struct nk_color DISABLED, ACCENT_HOVER, STATUS_BLUE;
extern const struct nk_color STATUS_PURPLE, STATUS_ORANGE, STATUS_GREEN;
extern const struct nk_color STATUS_GOLD, STATUS_DONE, STATUS_ERROR;
extern const struct nk_color MODAL_CLEAR, MODAL_SHADE;

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
