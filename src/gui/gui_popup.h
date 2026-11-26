#ifndef CDM_GUI_POPUP_H
#define CDM_GUI_POPUP_H

#include "nuklear.h"
#include <stdbool.h>

/* Leaves the popup open for nk_popup_end(), or closes and ends it now. */
bool gui_popup_begin_or_dismiss(struct nk_context *ctx, const char *title,
                                struct nk_rect bounds, bool dismiss);

#endif
