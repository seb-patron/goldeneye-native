/* On-screen touch controls for Android.
 *
 * iOS gets this for free: ge_virtual_controller.mm asks Apple for a GCVirtualController and
 * the OS presents it to SDL as an ordinary MFi pad, which is why that file needs no SDL-side
 * plumbing and no touch code at all. Android has no system equivalent, so the same result has
 * to be built -- but it is built at the SAME SEAM, deliberately. Touches drive an SDL virtual
 * joystick; SDL presents that as a game controller; port_input.c opens it exactly as it opens
 * a real pad and does not know the difference. No game code and no mapping code changes.
 *
 * No-op stub everywhere except Android.
 */
#ifndef GE_ANDROID_TOUCH_H
#define GE_ANDROID_TOUCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Attaches the virtual pad. Safe to call more than once; only the first call attaches.
 * GETV_TOUCH=0 turns the whole thing off, for the case of a real Bluetooth pad paired to
 * the device, where an invisible second controller would otherwise take player 1. */
void gePortAndroidTouchInit(void);

/* Samples the fingers currently down and writes the virtual pad's axes and buttons.
 * Called once per frame, before SDL_GameControllerUpdate(). */
void gePortAndroidTouchUpdate(void);

void gePortAndroidTouchShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GE_ANDROID_TOUCH_H */
