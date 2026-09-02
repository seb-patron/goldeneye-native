#ifndef GFX_METAL_H
#define GFX_METAL_H

#include <stdint.h>

#include "gfx_rendering_api.h"

/* GfxRenderingAPI rectangles use OpenGL's lower-left origin. Metal's viewport and
 * scissor origins are upper-left, so the backend must translate Y at its boundary. */
static inline int gfx_metal_upper_left_y(int lower_left_y, int height, int target_height)
{
    return target_height - lower_left_y - height;
}

/* Fast3D's per-tile flag says whether this draw wants filtering at all; configFiltering
 * distinguishes ordinary bilinear (1) from the N64's three-point filter (2). Keep that
 * combination at the Metal boundary so point-sampled HUD/text never enters the shader
 * filter and mode 1 remains the hardware bilinear path. */
static inline int gfx_metal_three_point_active(unsigned int filtering, int linear_filter)
{
    return filtering == 2 && linear_filter;
}

/* The shared depth-test toggle follows Fast3D/OpenGL's inclusive comparison: a fragment
 * at the stored depth passes. Decal mode still controls Metal's depth bias, but must not
 * change that comparison policy. Keep the mapping in plain C so it can be tested without
 * a Metal device, window, ROM, or extracted assets. */
enum GfxMetalDepthCompare {
    GFX_METAL_DEPTH_COMPARE_ALWAYS,
    GFX_METAL_DEPTH_COMPARE_LESS_EQUAL,
};

static inline enum GfxMetalDepthCompare gfx_metal_depth_compare(bool depth_test, bool zmode_decal)
{
    (void)zmode_decal;
    return depth_test ? GFX_METAL_DEPTH_COMPARE_LESS_EQUAL : GFX_METAL_DEPTH_COMPARE_ALWAYS;
}

/* GoldenEye uses the RDP's translucent coverage-save ("cloud surface") Z mode for blob
 * shadows that sit almost on the surface receiving them. Metal's float-depth rasterization
 * otherwise rejects parts that OpenGL keeps, so give exactly that state a modest
 * slope-aware offset. Ordinary translucent draws stay untouched; decals retain their
 * existing, smaller offset. */
static inline float gfx_metal_depth_slope_bias(bool depth_test, bool depth_mask, bool zmode_decal,
                                                bool zmode_cloud)
{
    if (zmode_decal) {
        return -2.0f;
    }
    return depth_test && !depth_mask && zmode_cloud ? -8.0f : 0.0f;
}

/* Shared byte layout for the CPU buffer and generated MSL DrawUniforms. Metal aligns a
 * float2 to eight bytes and therefore rounds the whole structure to eight as well; the
 * explicit final word keeps sizeof() at 32 instead of relying on compiler tail padding. */
struct GfxMetalDrawUniforms {
    float tex0_size[2];
    float tex1_size[2];
    int32_t has_height;
    int32_t tex0_filter;
    int32_t tex1_filter;
    int32_t alignment_pad;
};

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
int  gePortMetalImguiRenderDrawData(void *draw_data);
void gePortMetalImguiEndPass(void);
void gePortMetalImguiShutdown(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
