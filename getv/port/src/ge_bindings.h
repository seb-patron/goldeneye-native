/* GoldenEye native port - action binding resolution.
 *
 * The neutral middle of the input system. This header and its .c include neither
 * <SDL.h> nor <PR/os.h>, which is what lets both halves of the port call into it:
 * port_input.c resolves crouch/stand/reload here, port_os.c resolves every action here
 * when it decodes an OSContPad. Before this existed the binding table lived inside
 * port_os.c, so anything the SDL side needed (crouch, and later reload) had to be
 * hard-coded against raw scancodes instead of going through a binding at all.
 *
 * Two independent binding layers feed one action set:
 *
 *   keyboard/mouse  scancode -> GE_ACT_*   resolved in port_input.c, written into
 *                                          GePadState::act[] (it needs SDL to name a key)
 *   gamepad         GE_SRC_* -> GE_ACT_*   resolved here, read out of GePadState's
 *                                          button fields
 *
 * geActionHeld() is the OR of the two, so a player can drive the same action from
 * either device without the two remaps interfering. That is the substantive change:
 * previously the keyboard synthesised a virtual pad, so rebinding `fire` moved BOTH the
 * pad trigger and whatever key had been hard-wired to it.
 */
#ifndef GE_BINDINGS_H
#define GE_BINDINGS_H

#include "ge_actions.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GePadState;

/* ---- names ---------------------------------------------------------------
 * All four never return NULL; an out-of-range id reads as "?" so a trace line can
 * never crash and can never silently print a neighbouring name. */
const char *geActionName(int act);      /* GE_ACT_FIRE -> "fire"        */
const char *geActionEnvSuffix(int act); /* GE_ACT_FIRE -> "FIRE"        */
const char *geSourceName(int src);      /* GE_SRC_RT   -> "rt"          */
const char *geAxisName(int axis);       /* GE_AXIS_FORWARD -> "forward" */
const char *geAxisEnvSuffix(int axis);
const char *gePresetName(int preset);   /* GE_PRESET_MODERN -> "modern" */

/* Parse a config value. Returns `fallback` for NULL/empty and, after printing one
 * warning, for anything unrecognised -- a typo must not silently unbind an action. */
int geParseSource(const char *v, int fallback);
int geParsePreset(const char *v, int fallback);
int geParseHoldToggle(const char *v, int fallback);   /* "hold"|"toggle" -> GE_HOLD/GE_TOGGLE */

/* ---- resolution ----------------------------------------------------------
 *
 * The active preset. GETV_INPUT_PRESET, read once and cached. Every default below is
 * taken from it. */
int geInputPreset(void);

/* The pad source bound to `act` for `player` (0..GE_PORT_MAX_PADS-1).
 *
 * Three steps, most specific first: GETV_P<n>_BIND_<ACT>, then the global
 * GETV_BIND_<ACT>, then the preset's default. Resolved once on first call and cached,
 * and the whole table is printed at that point so a binding that failed to arrive is
 * distinguishable from one that worked. */
int geBindSrc(int player, int act);

/* The preset's default pad source for `act` -- what geBindSrc would return with nothing
 * configured. Exported for the launcher's "reset to default" and for tests. */
int gePresetSource(int preset, int act);

/* The preset's default KEYS for an action or a movement axis, as a comma-separated list
 * of SDL scancode names ("C,Left Ctrl"). Empty string means unbound, never NULL.
 *
 * Strings rather than scancodes because this file may not include <SDL.h>; port_input.c
 * resolves them with SDL_GetScancodeFromName. The upside is that the spellings here are
 * exactly what a player may write in goldeneye.cfg, and the presets stay testable
 * without a window. */
const char *gePresetKeys(int preset, int act);
const char *gePresetAxisKeys(int preset, int axis);

/* Is `act` live for `player` this frame? True if EITHER the keyboard/mouse asserted it
 * into st->act[] or the bound pad source is held. */
int geActionHeld(const struct GePadState *st, int player, int act);

/* Is the raw pad source `src` held in `st`? Exported so the launcher's binding capture
 * can ask "is anything pressed right now" without duplicating the field switch. */
int geSourceHeld(const struct GePadState *st, int src);

/* The N64 buttons (GE_N64_*) a front-end MENU sees, from fixed positions and fixed keys,
 * ignoring every gameplay binding: bottom face / Return / Space / left click select,
 * right face / Backspace / right click go back, Start / Back / Tab / Keypad Enter are
 * START. port_os.c uses this instead of the bindings whenever a front.c menu is up. */
unsigned geMenuButtons(const struct GePadState *st);

/* ---- hold vs toggle ------------------------------------------------------
 *
 * GETV_AIM_MODE / GETV_CROUCH_MODE, each GE_HOLD or GE_TOGGLE, read once and cached.
 *
 * geAimMode() is reported for completeness but is NOT enforced here: aim latching is
 * the engine's own per-player aim-control option, which bondview2.c reads as a rising
 * edge instead of a level. ge_config.c turns aim_mode=toggle into GETV_AIM_TOGGLE=1 and
 * the 0015-aim-toggle patch does the rest. Latching aim in the port as well would give
 * two latches fighting over one flag. */
int geAimMode(void);
int geCrouchMode(void);

/* Whether the USE button keeps its retail second job of reloading when there is nothing
 * in reach. False once RELOAD is bound to anything, so a dedicated reload key replaces
 * the double duty rather than sitting alongside it. GETV_USE_RELOADS forces either way;
 * `input_preset = n64` leaves reload unbound and so keeps the retail behaviour. */
int geUseAlsoReloads(void);

/* ---- per-frame state -----------------------------------------------------
 *
 * Crouch and reload need memory between frames -- a toggle needs the previous press, a
 * reload pulse needs the rising edge -- so they cannot be pure functions of `st` the
 * way every other action is. geBindingsFrame() advances that memory.
 *
 * Call EXACTLY ONCE PER PORT PER FRAME, from the input poll. Calling it twice in one
 * frame eats an edge (the second call sees the key still down and reports no press);
 * not calling it freezes the toggle. The accessors below are pure reads and may be
 * called any number of times, which matters because the game asks for crouch from two
 * different branches of bondviewProcessInput. */
void geBindingsFrame(int player, const struct GePadState *st);

int geCrouchActive(int player);   /* after hold/toggle resolution */
int geStandActive(int player);    /* momentary always: "stand up" has no toggle meaning */
int geReloadEdge(int player);     /* 1 on the frame RELOAD was pressed, else 0 */

/* Drop all remembered state. For tests, and for a level/session boundary that should
 * not inherit a latched crouch. */
void geBindingsReset(void);

#ifdef __cplusplus
}
#endif

#endif /* GE_BINDINGS_H */
