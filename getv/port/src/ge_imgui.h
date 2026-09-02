/* Dear ImGui overlay -- C entry points.
 *
 * The implementation is C++ (ge_imgui.cpp); everything that calls it -- gfx_sdl2.c and
 * anything a later launcher/dev UI hangs off it -- is C. This header is the whole of the
 * boundary and is deliberately free of both ImGui and SDL types, so including it costs a
 * C translation unit nothing and pulls in no C++.
 *
 * The parameters are void* rather than SDL_Window and SDL_Event pointers for that reason:
 * SDL_Window
 * is an opaque struct and could be forward-declared, but SDL_Event is a union and cannot,
 * so declaring one honestly and the other as void* would only look tidier. Both are void*
 * and both are documented here.
 *
 * Every function is safe to call unconditionally:
 *   - built without ImGui (no -DGE_WITH_IMGUI), they are empty;
 *   - built with it but not enabled (GETV_IMGUI unset), they return immediately;
 *   - called out of order or after a failed init, they return immediately.
 * That is the point. gfx_sdl2.c has no #ifdef in it.
 */
#ifndef GE_IMGUI_H
#define GE_IMGUI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called once from gfx_sdl_init() after the GL context exists.
 *   window -- SDL_Window*
 *   glctx  -- SDL_GLContext (itself a void*)
 * Reads GETV_IMGUI. Anything other than a set, non-empty, non-"0" value leaves the
 * overlay off and this call is then the only cost the feature has at runtime. */
void gePortImguiInit(void *window, void *glctx);

/* Called once per rendered frame from gfx_sdl_start_frame(), i.e. after the event pump
 * and before the display list is walked. Begins the ImGui frame and builds the overlay's
 * widgets; it does not touch GL. */
void gePortImguiNewFrame(void);

/* Called from gfx_sdl_swap_buffers_begin() after the scene is complete and before present.
 * Issues either the OpenGL draw or Metal overlay pass. */
void gePortImguiRender(void);

/* Called for every SDL event from gfx_sdl_handle_events(). Returns 1 when the console owns
 * this keyboard/mouse event and gameplay must not see it.
 * Window and quit events are never swallowed. */
int gePortImguiEvent(void *sdl_event);

/* Called from gfx_sdl_shutdown() before the GL context is destroyed. */
void gePortImguiShutdown(void);

/* 1 when the overlay is built in AND enabled AND initialised. Not used by the port itself;
 * it exists so a later launcher can ask rather than re-reading the environment. */
int  gePortImguiActive(void);

/* Used by the SDL bridge to clear legacy callback state on the closed-to-open edge. */
int gePortImguiConsoleOpen(void);

#ifdef __cplusplus
}
#endif

#endif /* GE_IMGUI_H */
