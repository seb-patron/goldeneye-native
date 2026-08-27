// Standalone Metal context for the launcher window. See ge_launcher_metal.h for why this
// cannot reuse gfx_metal.mm's game-window MTLDevice/CAMetalLayer/command queue: none of it
// exists yet at the point the launcher runs (before gfx_init()).
//
// Compiled unconditionally (like gfx_metal.mm) so a non-Metal build gets an empty, harmless
// translation unit rather than needing its own build-script exclusion.
#if defined(RAPI_METAL) && defined(GE_WITH_IMGUI)

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <SDL2/SDL.h>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "ge_launcher_metal.h"

namespace {
id<MTLDevice> g_device;
id<MTLCommandQueue> g_queue;
CAMetalLayer *g_layer;
SDL_MetalView g_view;
} // namespace

int geLauncherMetalCreate(void *sdl_window) {
    @autoreleasepool {
        g_view = SDL_Metal_CreateView((SDL_Window *)sdl_window);
        if (!g_view) {
            printf("[getv][launcher] SDL_Metal_CreateView failed: %s\n", SDL_GetError());
            return 0;
        }
        g_layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(g_view);
        if (!g_layer) {
            printf("[getv][launcher] SDL_Metal_GetLayer returned NULL\n");
            SDL_Metal_DestroyView(g_view);
            g_view = nil;
            return 0;
        }
        g_device = MTLCreateSystemDefaultDevice();
        g_layer.device = g_device;
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.framebufferOnly = YES;
        g_queue = [g_device newCommandQueue];

        if (!ImGui_ImplMetal_Init(g_device)) {
            printf("[getv][launcher] ImGui_ImplMetal_Init failed\n");
            return 0;
        }
        /* Build every device object -- depth-stencil state AND the font atlas texture -- NOW,
         * not lazily inside the first ImGui_ImplMetal_NewFrame() call.
         *
         * First bug this fixes: ImGui::NewFrame() (called once per loop iteration, before
         * widgets are built) asserts g.IO.Fonts->IsBuilt() before ANY renderer backend call
         * runs at all -- so the atlas has to exist before the loop's first NewFrame(), not
         * merely before the first draw.
         *
         * Second, subtler bug this fixes, found AFTER the first one was patched (a plain
         * ImGui_ImplMetal_CreateFontsTexture() call here): ImGui_ImplMetal_NewFrame() itself
         * does `if (depthStencilState == nil) CreateDeviceObjects(device)`, and
         * CreateDeviceObjects() unconditionally calls CreateFontsTexture() AGAIN --
         * regardless of whether a texture already exists. geLauncherMetalRenderAndPresent()
         * defers the NewFrame() call to right before drawing (see its own comment for why),
         * so on the very first frame that lazy path fired AFTER ImGui::Render() had already
         * recorded draw commands referencing the FIRST texture's pointer -- rebuilding the
         * atlas replaced ctx.fontTexture with a second, different id<MTLTexture>, and ARC
         * deallocated the first one out from under those already-recorded commands. Verified
         * with an instrumented build: FontsTexID at draw time did not match the TexID actually
         * embedded in the draw commands, off by exactly one rebuild. Calling
         * CreateDeviceObjects() (not just CreateFontsTexture()) here makes depthStencilState
         * non-nil before the loop starts, so that lazy branch never fires at all. */
        if (!ImGui_ImplMetal_CreateDeviceObjects(g_device)) {
            printf("[getv][launcher] ImGui_ImplMetal_CreateDeviceObjects failed\n");
            return 0;
        }
        return 1;
    }
}

void geLauncherMetalRenderAndPresent(void *draw_data) {
    if (!g_device) return;
    @autoreleasepool {
        if (g_layer.drawableSize.width < 1 || g_layer.drawableSize.height < 1) return;
        id<CAMetalDrawable> drawable = [g_layer nextDrawable];
        if (!drawable) return;

        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        /* kBg, the same constant ge_launcher.cpp's GL path clears to (0.031, 0.035, 0.043). */
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.031, 0.035, 0.043, 1.0);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cmdbuf = [g_queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [cmdbuf renderCommandEncoderWithDescriptor:pass];

        ImGui_ImplMetal_NewFrame(pass);
        ImGui_ImplMetal_RenderDrawData((ImDrawData *)draw_data, cmdbuf, encoder);

        [encoder endEncoding];
        [cmdbuf presentDrawable:drawable];
        [cmdbuf commit];
    }
}

void geLauncherMetalDestroy(void) {
    ImGui_ImplMetal_Shutdown();
    if (g_view) { SDL_Metal_DestroyView(g_view); g_view = nil; }
    g_device = nil;
    g_queue = nil;
    g_layer = nil;
}

#endif // RAPI_METAL && GE_WITH_IMGUI
