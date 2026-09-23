#ifndef GUI_GUI_BACKEND_SDL_H
#define GUI_GUI_BACKEND_SDL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct GuiSdlBackend GuiSdlBackend;

typedef struct {
  int width;
  int height;
  const char *title;
  float font_size;
} GuiSdlBackendConfig;

GuiSdlBackend *gui_sdl_backend_create(const GuiSdlBackendConfig *config);
bool gui_sdl_backend_poll(GuiSdlBackend *backend);
void gui_sdl_backend_begin_frame(GuiSdlBackend *backend);
void gui_sdl_backend_end_frame(GuiSdlBackend *backend);
void gui_sdl_backend_destroy(GuiSdlBackend *backend);

struct nk_user_font;
const struct nk_user_font *gui_sdl_backend_font(GuiSdlBackend *backend,
                                                int size);

const struct nk_user_font *gui_sdl_backend_bold_font(GuiSdlBackend *backend,
                                                     int size);

struct nk_context *gui_sdl_backend_context(GuiSdlBackend *backend);

#endif