#define NK_INCLUDE_FIXED_TYPES
#include "gui_popup.h"

bool gui_popup_begin_or_dismiss(struct nk_context *ctx, const char *title,
                                struct nk_rect bounds, bool dismiss) {
  if (!nk_popup_begin(ctx, NK_POPUP_STATIC, title, NK_WINDOW_NO_SCROLLBAR,
                      bounds))
    return false;
  if (dismiss) {
    nk_popup_close(ctx);
    nk_popup_end(ctx);
    return false;
  }
  return true;
}
