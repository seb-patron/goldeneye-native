#ifndef GE_VIRTUAL_CONTROLLER_H
#define GE_VIRTUAL_CONTROLLER_H

#ifdef __cplusplus
extern "C" {
#endif

/* On-screen touch controls for iOS, via Apple's GCVirtualController -- see the .mm file's
 * header comment for why this needs no SDL-side plumbing at all. No-op stub on every
 * other platform (tvOS has a real remote; desktop has a keyboard/mouse and real pads). */
void gePortVirtualControllerInit(void);
void gePortVirtualControllerShutdown(void);

/* Force an immediate rotation to landscape. Info.plist's UISupportedInterfaceOrientations
 * (project_ios.yml) and the SDL_HINT_ORIENTATIONS hint (ge_launcher.cpp / gfx_sdl2.c) both
 * restrict which orientations the app CAN present, but neither one makes the OS actually
 * rotate an already-portrait launch into landscape -- see the .mm file's own comment. Call
 * this once an SDL window/UIWindow exists (there is no scene/key window before then). */
void gePortForceLandscapeOrientation(void);

#ifdef __cplusplus
}
#endif

#endif /* GE_VIRTUAL_CONTROLLER_H */
