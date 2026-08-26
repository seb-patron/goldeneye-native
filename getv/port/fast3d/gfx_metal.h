#ifndef GFX_METAL_H
#define GFX_METAL_H

#include "gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

extern struct GfxRenderingAPI gfx_metal_api;

/* Set by gfx_sdl2.c's RAPI_METAL window-creation path, right after
 * SDL_Metal_CreateView()/SDL_Metal_GetLayer() -- a CAMetalLayer*, opaque here so this
 * header stays includable from plain C (ge_tvos_main.c). Read once, in gfx_metal_init(),
 * which gfx_init() (gfx_pc.c) guarantees runs after gfx_wapi->init() has set this. */
extern void *gePortMetalLayer;

#ifdef __cplusplus
}
#endif

#endif
