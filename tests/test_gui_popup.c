#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../src/gui/gui_popup.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <criterion/criterion.h>

static float font_width(nk_handle handle, float height, const char *text,
                        int length) {
  (void)handle;
  (void)text;
  return height * length / 2;
}

Test(gui_popup, dismissal_releases_parent_input) {
  struct nk_context ctx;
  struct nk_user_font font = {.height = 12, .width = font_width};
  cr_assert(nk_init_default(&ctx, &font));
  struct nk_rect bounds = nk_rect(40, 40, 180, 100);

  nk_input_begin(&ctx);
  nk_input_end(&ctx);
  cr_assert(nk_begin(&ctx, "main", nk_rect(0, 0, 400, 300), 0));
  cr_assert(gui_popup_begin_or_dismiss(&ctx, "row-menu", bounds, false));
  nk_popup_end(&ctx);
  nk_end(&ctx);
  nk_clear(&ctx);

  nk_input_begin(&ctx);
  nk_input_end(&ctx);
  cr_assert(nk_begin(&ctx, "main", nk_rect(0, 0, 400, 300), 0));
  cr_assert(ctx.current->flags & NK_WINDOW_ROM);
  cr_assert_not(gui_popup_begin_or_dismiss(&ctx, "row-menu", bounds, true));
  nk_end(&ctx);
  nk_clear(&ctx);

  nk_input_begin(&ctx);
  nk_input_end(&ctx);
  cr_assert(nk_begin(&ctx, "main", nk_rect(0, 0, 400, 300), 0));
  cr_assert_not(ctx.current->flags & NK_WINDOW_ROM);
  cr_assert(gui_popup_begin_or_dismiss(&ctx, "row-menu", bounds, false));
  nk_popup_end(&ctx);
  nk_end(&ctx);
  nk_clear(&ctx);
  nk_free(&ctx);
}
