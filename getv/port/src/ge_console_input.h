/* Renderer-independent input ownership for the developer console.
 *
 * SDL events and polled keyboard/mouse state are two different input paths in this port.  The
 * console must own both while it is open, then keep owning them until every physical key and
 * mouse button has been released.  That release quarantine prevents a key held while closing
 * the console from appearing to gameplay as a fresh press on the next poll.
 */
#ifndef GE_CONSOLE_INPUT_H
#define GE_CONSOLE_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

void geConsoleInputReset(void);
void geConsoleInputSetOpen(int open);
int  geConsoleInputOpen(void);

/* True while the console is open or its post-close release quarantine is active. */
int geConsoleInputCaptureActive(void);

/* Called from the polled input path with normalized physical state.  Returns one only when
 * gameplay may observe keyboard/mouse input.  The first all-released poll ends quarantine. */
int geConsoleInputGameplayAllowed(int any_keyboard_key, int any_mouse_button);

#ifdef __cplusplus
}
#endif

#endif /* GE_CONSOLE_INPUT_H */
