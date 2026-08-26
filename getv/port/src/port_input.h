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

    int lx, ly, rx, ry;                 /* -32768..32767, SDL sign convention (+Y down) */
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
 * where SDL maps the physically-bottom button (labelled B) to `_BUTTON_A`.
 * `struct GePadState` inherits that, so `st->a` already means "bottom face button" on an
 * Xbox pad, a DualSense and a Switch Pro alike.
 *
 * Gameplay bindings are therefore already pad-agnostic and need no profile at all. A
 * profile is needed for exactly one thing: deciding what to print. "Press A" is correct
 * on Xbox, wrong on a DualSense (Cross) and misleading on a Switch Pro, where the button
 * labelled A is the one to its right.
 *
 * This API is a labelling service, not a remapping one, so that a future
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

#endif /* GE_PORT_INPUT_H */
