/* GoldenEye tvOS port - the SDL side of input, kept away from <PR/os.h>.
 *
 * This split is required, not cosmetic. <PR/os.h> redeclares bcopy/bcmp/bzero with
 * `int` lengths, and <SDL.h> pulls in the system <string.h> which declares them with
 * `size_t`. Including both in one translation unit is a hard "conflicting types" error
 * whichever order they go in. port_input.c sees SDL and never sees PR/os.h; port_os.c
 * sees PR/os.h and never sees SDL. This neutral struct is the only thing that crosses.
 *
 * Raw device state only - no N64 mapping. The CONT_* bits belong in port_os.c, next
 * to the header that defines them.
 */
#ifndef GE_PORT_INPUT_H
#define GE_PORT_INPUT_H

/* Safe from every translation unit, including the ones that see <PR/os.h>: ge_actions.h
 * includes no headers at all. */
#include "ge_actions.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The N64 has four controller ports and the game's MAXCONTROLLERS is 4. Multiplayer
 * needs all four; solo only ever reads port 0. */
#define GE_PORT_MAX_PADS 4

struct GePadState {
    int present;        /* 0 = no pad on this port yet (tvOS delivers them late) */

    /* 1 = a real dual-stick gamepad (MFi/DualSense/Xbox). 0 = something that
     * enumerated as a controller but has no sticks/shoulders - in practice the Siri
     * Remote. The game is not playable from one, but it must not crash either, and
     * a real pad that arrives later must be able to take over. */
    int real_gamepad;

    /* 1 = this port's state was synthesised (GETV_PADS / GETV_PAD_SYNTH), not read
     * from any hardware. Printed in the trace so a headless run can never be mistaken
     * for one taken off a physical pad. */
    int synthetic;

    int a, b, x, y;
    int start, back;
    int lshoulder, rshoulder;
    int ltrigger, rtrigger;             /* already thresholded to 0/1 */
    int lt_raw, rt_raw;                 /* 0..32767, for GETV_INPUT_DEBUG */
    int dup, ddown, dleft, dright;
    int lstickbtn, rstickbtn;           /* stick CLICKS (L3/R3), not deflection */

    int lx, ly, rx, ry;                 /* -32768..32767, SDL sign convention (+Y down) */

    /* Actions asserted directly by the keyboard and mouse, indexed by GE_ACT_*.
     *
     * This is the second of the two binding layers and the reason keyboard remapping
     * works at all. The fields above are a DEVICE state -- "the right trigger is down"
     * -- and the pad binds them to actions in ge_bindings.c. The keyboard has no
     * triggers to speak of, so it used to fake them: pressing Q set `ltrigger`, purely
     * because aim happened to be bound there. Rebinding aim to a face button then broke
     * Q, and two keys could never drive one action independently.
     *
     * So the keyboard skips the device layer and names the action. geActionHeld() ORs
     * the two, and neither remap can disturb the other.
     *
     * Sized by GE_ACT_MAX. ge_actions.h is safe to pull in from anywhere -- it includes
     * nothing itself, which is the whole reason it exists. */
    unsigned char act[GE_ACT_MAX];
};

/* Number of N64 ports that should report as connected, 0..GE_PORT_MAX_PADS.
 *
 * Always contiguous from port 0. joy.c's joyGetControllerCount() returns the index of
 * the first clear bit of g_ConnectedControllers, so a hole (say ports 0 and 2 only)
 * makes the game count one controller, not two. Device assignment therefore compacts,
 * and a device is never parked in a port with an empty one below it.
 *
 * Brings the controller subsystem up on the first call and rescans on every call, so
 * either this or gePortInputPollPort() is enough to drive discovery. */
int gePortInputPadCount(void);

/* Fills `out` with the state of one N64 port (0..GE_PORT_MAX_PADS-1). Always writes a
 * fully-initialised struct, so a caller can ignore `present` and still read
 * well-defined zeroes. Out-of-range ports read as absent rather than faulting. */
void gePortInputPollPort(int port, struct GePadState *out);

/* Called by player one's game input consumer. Context 0 discards menu/cutscene
 * motion, 1 consumes on-foot angles once, 2 retains classic vehicle controls.
 * Returns whether modern mouse look owns pitch centering, even at rest. */
int gePortInputTakeMouseLook(int player, int context, float *yaw, float *pitch);

/* Desktop SDL window handoff. The event owner calls MouseClick only for left-button-down
 * in the game window after console capture, and FocusLost on keyboard-focus loss. */
#ifdef GE_PLATFORM_DESKTOP
void gePortInputMouseClick(unsigned int window_id, int x, int y);
void gePortInputMouseFocusLost(void);

/* Every SDL_MOUSEWHEEL, with `y` already un-flipped by the caller so that positive is
 * always "away from the user".
 *
 * An event rather than a poll because SDL offers no wheel STATE to read -- motion
 * exists only as an event, and the rest of this file's input is polled once a frame.
 * The count waits here until the next pad read, which releases it one notch at a time:
 * the engine cycles weapons on a rising edge, so three notches flicked inside one frame
 * would otherwise raise a single edge and advance one weapon instead of three. */
void gePortInputMouseWheel(int y);

/* The name of an input code, for the launcher's binding capture -- "Left Ctrl",
 * "mouse2", "wheelup", "none". Never NULL, and it round-trips: what this returns can be
 * written straight into goldeneye.cfg and parsed back. */
const char *gePortInputCodeName(int code);
#endif

/* Crouch, stand and reload, read back out of the port by the game -- bondview2.c and
 * lv.c call these directly, because the N64 controller has no button for any of them.
 * All three are resolved once per frame during the input poll; these are pure reads.
 * gePortReloadPressed() is one frame per press, not a level. */
int gePortCrouchHeld(void);
int gePortStandHeld(void);   /* the pulse that ends a crouch; not a bindable key */
int gePortReloadPressed(void);
int gePortUseAlsoReloads(void);

/* GETV_INPUT_DEBUG, read once and cached.
 *   0 = silent (default)
 *   1 = one line whenever the decoded N64 pad changes, plus a heartbeat
 *   2 = one line every frame the game reads the pad
 * Lives here rather than in port_os.c only because port_os.c cannot include a system
 * <stdlib.h> safely next to <PR/os.h>. */
int gePortInputDebugLevel(void);

/* Name of the controller bound to `port`, or "none"/"synthetic". Never NULL. */
const char *gePortInputPadName(int port);

/* ---- gamepad PROFILE ------------------------------------------------------
 *
 * SDL's face-button constants are positional, not label-based. `SDL_CONTROLLER_BUTTON_A`
 * is the bottom face button on every pad SDL's database knows, including Nintendo's,
 * where SDL deliberately maps the physically-bottom button (labelled B) to `_BUTTON_A`.
 * `struct GePadState` inherits that, so `st->a` already means "bottom face button" on an
 * Xbox pad, a DualSense and a Switch Pro alike.
 *
 * Gameplay bindings are therefore already pad-agnostic and need no profile at all. A
 * profile is needed for exactly one thing: deciding what to print. "Press A" is correct
 * on Xbox, wrong on a DualSense (Cross) and misleading on a Switch Pro, where the button
 * labelled A is the one to its right.
 *
 * This API is deliberately a labelling service, not a remapping one, so that a future
 * HUD or button-prompt layer is a lookup rather than a refactor. */
enum {
    GE_PAD_GENERIC = 0,
    GE_PAD_XBOX,
    GE_PAD_PLAYSTATION,
    GE_PAD_SWITCH
};

/* Positional glyph ids. SOUTH/EAST/WEST/NORTH are physical positions on the face
 * diamond, which is the only pad-independent way to name them. */
enum {
    GE_GLYPH_SOUTH = 0, GE_GLYPH_EAST, GE_GLYPH_WEST, GE_GLYPH_NORTH,
    GE_GLYPH_LB, GE_GLYPH_RB, GE_GLYPH_LT, GE_GLYPH_RT,
    GE_GLYPH_START, GE_GLYPH_BACK, GE_GLYPH_LSTICK, GE_GLYPH_RSTICK,
    GE_GLYPH_MAX
};

/* Which profile `port` resolves to. Auto-detected from SDL_GameControllerGetType()
 * and overridden by GETV_GAMEPAD=xbox|playstation|switch|generic|auto. */
int gePortPadProfile(int port);

/* The printable label for a positional glyph under `port`'s profile -- e.g.
 * GE_GLYPH_SOUTH is "A" on Xbox, "Cross" on PlayStation, "B" on Switch. Never NULL. */
const char *gePortPadGlyph(int port, int glyph);

/* The developer console releases relative mouse mode while it owns keyboard/mouse input and
 * restores the player's prior capture intent when it closes. */
void gePortInputConsoleCapture(int capture);

#ifdef __cplusplus
}
#endif

#endif /* GE_PORT_INPUT_H */
