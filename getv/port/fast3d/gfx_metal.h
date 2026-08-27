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

/* Same idea, the SDL_Window* itself -- needed by ge_imgui.cpp's ImGui_ImplSDL2_InitForMetal
 * call, made from gfx_metal_init() (see the long comment in gfx_metal.mm on why ImGui init
 * cannot happen at the same point gfx_sdl2.c calls it for the GL backend). */
extern void *gePortMetalWindow;

/* Always present, regardless of GE_WITH_IMGUI: Metal has no "swap" call the way GL does.
 * gfx_metal_end_frame() only ends the GAME's render encoder now; this presents the drawable
 * and commits the command buffer. Called from gfx_sdl2.c's RAPI_METAL swap_buffers_begin,
 * after the ImGui overlay (if any) has drawn -- see the frame-lifecycle note in gfx_metal.mm. */
void gePortMetalFinishFrame(void);

#ifdef GE_WITH_IMGUI
/* ImGui-on-Metal glue, called from ge_imgui.cpp and ge_launcher.cpp. Every Metal/ObjC type
 * (id<MTLDevice>, the render pass descriptor, the command encoder) stays inside gfx_metal.mm
 * -- these signatures are plain C/void* so callers never need to be Objective-C++
 * themselves. draw_data is an ImDrawData*, opaque here for the same reason. */
int  gePortMetalImguiInit(void);
void gePortMetalImguiBeginPass(void);
void gePortMetalImguiRenderDrawData(void *draw_data);
void gePortMetalImguiEndPass(void);
void gePortMetalImguiShutdown(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
