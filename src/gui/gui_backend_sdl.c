#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_GL3_IMPLEMENTATION

#include "gui_font_data.h"
// Epoxy must precede SDL_opengl.h; their GL declarations conflict otherwise.
// clang-format off
#include <epoxy/gl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
// clang-format on
#include <stdlib.h>

#include "nuklear.h"
#include "nuklear_sdl_gl3.h"

#include "../utils/log.h"
#include "gui_backend_sdl.h"

#define GUI_SDL_MAX_VERTEX_MEMORY (512 * 1024)
#define GUI_SDL_MAX_ELEMENT_MEMORY (128 * 1024)

struct GuiSdlBackend {
  SDL_Window *window;
  SDL_GLContext gl_context;
  struct nk_context *context;
  struct nk_font *fonts[6];
  struct nk_font *bold_fonts[6];
};

GuiSdlBackend *gui_sdl_backend_create(const GuiSdlBackendConfig *config) {
  if (!config ||
      SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
    LOG_ERROR("gui_sdl: SDL initialization failed: %s", SDL_GetError());
    return NULL;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                      SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  GuiSdlBackend *backend = calloc(1, sizeof(*backend));
  if (!backend)
    goto fail;

  backend->window = SDL_CreateWindow(
      config->title ? config->title : "Core Download Manager",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      config->width > 0 ? config->width : 1100,
      config->height > 0 ? config->height : 720,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!backend->window) {
    LOG_ERROR("gui_sdl: window creation failed: %s", SDL_GetError());
    free(backend);
    goto fail;
  }

  backend->gl_context = SDL_GL_CreateContext(backend->window);
  if (!backend->gl_context) {
    LOG_ERROR("gui_sdl: OpenGL context creation failed: %s", SDL_GetError());
    SDL_DestroyWindow(backend->window);
    free(backend);
    goto fail;
  }

  SDL_GL_MakeCurrent(backend->window, backend->gl_context);
  SDL_GL_SetSwapInterval(1);
  backend->context = nk_sdl_init(backend->window);
  if (!backend->context) {
    LOG_ERROR("gui_sdl: Nuklear initialization failed");
    gui_sdl_backend_destroy(backend);
    return NULL;
  }

  struct nk_font_atlas *atlas = NULL;
  nk_sdl_font_stash_begin(&atlas);
  /* Nuklear copies these static bytes into its owned atlas allocation. */
  bool fonts_ok = true;
  for (int i = 0; i < 6; ++i) {
    /* stb_truetype uses ascender-to-descender pixel height; CSS uses em.
       Both bundled faces have hhea span 2288 and unitsPerEm 2048. */
    float pixel_height = (float)(11 + i) * (2288.0f / 2048.0f);
    struct nk_font_config font_config = nk_font_config(pixel_height);
    font_config.oversample_h = 4;
    font_config.oversample_v = 4;
    font_config.ttf_data_owned_by_atlas = nk_false;
    backend->fonts[i] = nk_font_atlas_add_from_memory(
        atlas, (void *)gui_font_data, sizeof(gui_font_data), pixel_height,
        &font_config);
    backend->bold_fonts[i] = nk_font_atlas_add_from_memory(
        atlas, (void *)gui_font_bold_data, sizeof(gui_font_bold_data),
        pixel_height, &font_config);
    if (!backend->fonts[i] || !backend->bold_fonts[i])
      fonts_ok = false;
  }
  if (!fonts_ok) {
    LOG_ERROR("gui_sdl: could not load embedded Liberation Sans; refusing font "
              "fallback");
    gui_sdl_backend_destroy(backend);
    return NULL;
  }
  nk_sdl_font_stash_end();
  for (int i = 0; i < 6; ++i) {
    if (!backend->fonts[i]->glyphs || !backend->fonts[i]->handle.width ||
        !backend->bold_fonts[i]->glyphs ||
        !backend->bold_fonts[i]->handle.width) {
      LOG_ERROR("gui_sdl: embedded font atlas bake failed at %dpx", 11 + i);
      gui_sdl_backend_destroy(backend);
      return NULL;
    }
  }
  int size = config->font_size >= 11 && config->font_size <= 16
                 ? (int)config->font_size
                 : 13;
  nk_style_set_font(backend->context, &backend->fonts[size - 11]->handle);
  LOG_INFO("gui_sdl: embedded Liberation Sans regular/bold (%zu bytes), 11-16 "
           "CSS px, oversample=4x4",
           sizeof(gui_font_data) + sizeof(gui_font_bold_data));
  return backend;

fail:
  SDL_Quit();
  return NULL;
}

bool gui_sdl_backend_poll(GuiSdlBackend *backend) {
  if (!backend)
    return false;

  nk_input_begin(backend->context);
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      nk_input_end(backend->context);
      return false;
    }
    nk_sdl_handle_event(&event);
  }
  nk_sdl_handle_grab();
  nk_input_end(backend->context);
  return true;
}

void gui_sdl_backend_begin_frame(GuiSdlBackend *backend) {
  (void)backend;
  glClearColor(0.055f, 0.065f, 0.08f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

void gui_sdl_backend_end_frame(GuiSdlBackend *backend) {
  if (!backend)
    return;
  nk_sdl_render(NK_ANTI_ALIASING_ON, GUI_SDL_MAX_VERTEX_MEMORY,
                GUI_SDL_MAX_ELEMENT_MEMORY);
  SDL_GL_SwapWindow(backend->window);
}

void gui_sdl_backend_destroy(GuiSdlBackend *backend) {
  if (!backend)
    return;
  nk_sdl_shutdown();
  if (backend->gl_context)
    SDL_GL_DeleteContext(backend->gl_context);
  if (backend->window)
    SDL_DestroyWindow(backend->window);
  free(backend);
  SDL_Quit();
}

struct nk_context *gui_sdl_backend_context(GuiSdlBackend *backend) {
  return backend ? backend->context : NULL;
}
const struct nk_user_font *gui_sdl_backend_font(GuiSdlBackend *backend,
                                                int size) {
  if (!backend || size < 11 || size > 16)
    return NULL;
  return &backend->fonts[size - 11]->handle;
}

const struct nk_user_font *gui_sdl_backend_bold_font(GuiSdlBackend *backend,
                                                     int size) {
  if (!backend || size < 11 || size > 16)
    return NULL;
  return &backend->bold_fonts[size - 11]->handle;
}
