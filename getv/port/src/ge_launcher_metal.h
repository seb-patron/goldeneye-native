#ifndef GE_LAUNCHER_METAL_H
#define GE_LAUNCHER_METAL_H

/* A standalone Metal context for the launcher's OWN short-lived window -- independent of
 * gfx_metal.mm's game-window state, which does not exist yet when the launcher runs (it
 * runs before gfx_init(): main()/SDL_main() calls gePortLauncherRun() first). Mirrors the
 * launcher's existing SDL_GLContext lifecycle: create once, draw+present per frame, destroy
 * once. See ge_launcher_metal.mm for the implementation and getv/port/fast3d/gfx_metal.mm
 * for the (separate) game-window equivalent this deliberately does not share state with. */

#ifdef __cplusplus
extern "C" {
#endif

int  geLauncherMetalCreate(void *sdl_window);   /* SDL_Window*; returns 1 on success */
/* draw_data is an ImDrawData*, opaque here so this header stays includable without pulling
 * in ObjC/ImGui types -- ge_launcher.cpp passes ImGui::GetDrawData() straight through. No
 * drawable-size parameter: the CAMetalLayer's own .drawableSize is already the physical
 * (HiDPI-correct) size, same as gfx_metal.mm reads for the game window, so there is no
 * separate query to get wrong the way the GL path's manual glViewport(dw,dh) call (see its
 * comment) once did. Clears to the launcher's own kBg background, draws, presents, commits. */
void geLauncherMetalRenderAndPresent(void *draw_data);
void geLauncherMetalDestroy(void);

#ifdef __cplusplus
}
#endif

#endif
