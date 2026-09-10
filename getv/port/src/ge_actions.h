/* GoldenEye native port - the canonical input ACTION set.
 *
 * This header is the one place that names an action, a pad source or a movement axis.
 * It exists because the two halves of input cannot see each other: port_input.c sees
 * <SDL.h> and never <PR/os.h>, port_os.c sees <PR/os.h> and never <SDL.h> (the two
 * redeclare bcopy/bcmp/bzero with different length types and cannot coexist -- see the
 * note at the top of port_input.h). Both need the same action numbering, so it lives
 * in a header that includes NOTHING. Do not add an #include here.
 *
 * The lists are X-macros rather than four parallel arrays, and that is a bug fix rather
 * than a style choice. port_os.c used to carry the enum plus `nm[]`, `act_nm[]`,
 * `act_key[]` and `dflt[]` as separate positional tables, with a comment warning that
 * adding a member shifts every later ordinal and makes the resolved-bindings report
 * print the wrong name -- "worse than no line at all, because it looks authoritative".
 * Adding crouch, stand and reload would have meant editing four tables in two files and
 * a fifth in the launcher. Generating all of them from one list makes that class of
 * drift unrepresentable.
 *
 * Adding an action: add one X() line below, then give it a default in every preset in
 * ge_bindings.c (the compiler will not catch a missing one -- the preset tables are
 * sized by GE_ACT_MAX and a gap reads as GE_SRC_NONE, which is a silent unbound key).
 */
#ifndef GE_ACTIONS_H
#define GE_ACTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- actions -------------------------------------------------------------
 *
 * M(id, config-name, ENV-SUFFIX)
 *
 * config-name is what goes in goldeneye.cfg; ENV-SUFFIX composes GETV_BIND_<S> for the
 * pad and GETV_KEY_<S> for the keyboard.
 *
 * Order is the order the launcher lists them and the order the startup report prints,
 * so it is grouped by what a player thinks of together rather than by history.
 *
 * FIRE/AIM/USE/WEAPON_NEXT/PAUSE predate this header and keep their spellings; a config
 * written for the old build still resolves. CROUCH, STAND, RELOAD and WEAPON_PREV are
 * new here -- see ge_bindings.c for what each one can and cannot reach in the engine.
 */
#define GE_ACTION_LIST(M)                        \
    M(FIRE,        "fire",        "FIRE")        \
    M(AIM,         "aim",         "AIM")         \
    M(USE,         "use",         "USE")         \
    M(RELOAD,      "reload",      "RELOAD")      \
    M(CROUCH,      "crouch",      "CROUCH")      \
    M(STAND,       "stand",       "STAND")       \
    M(WEAPON_NEXT, "weapon_next", "WEAPON_NEXT") \
    M(WEAPON_PREV, "weapon_prev", "WEAPON_PREV") \
    M(PAUSE,       "pause",       "PAUSE")

enum {
#define GE_ACT_ENUM_(id, lo, up) GE_ACT_##id,
    GE_ACTION_LIST(GE_ACT_ENUM_)
#undef GE_ACT_ENUM_
    GE_ACT_MAX
};

/* ---- pad sources ---------------------------------------------------------
 *
 * M(id, config-name)
 *
 * Positional, matching SDL and `struct GePadState`: "a" is the bottom face button on
 * every pad SDL's database knows, including Nintendo's, where the physically-bottom
 * button is labelled B. A binding therefore never needs to know the pad type; only a
 * printed prompt does, which is what gePortPadGlyph() in port_input.h is for.
 *
 * DUP..DRIGHT and LSTICK/RSTICK are new. The d-pad was always read (it drives the N64
 * d-pad directly in gePortDecodePad) but was not bindable; the stick clicks were not
 * read at all until this change added them to GePadState. Both are here because a
 * modern pad layout wants crouch on a stick click or the d-pad and the six-source
 * table could not express it.
 */
#define GE_SOURCE_LIST(M)  \
    M(NONE,   "none")      \
    M(A,      "a")         \
    M(B,      "b")         \
    M(X,      "x")         \
    M(Y,      "y")         \
    M(LB,     "lb")        \
    M(RB,     "rb")        \
    M(LT,     "lt")        \
    M(RT,     "rt")        \
    M(START,  "start")     \
    M(BACK,   "back")      \
    M(DUP,    "dup")       \
    M(DDOWN,  "ddown")     \
    M(DLEFT,  "dleft")     \
    M(DRIGHT, "dright")    \
    M(LSTICK, "lstick")    \
    M(RSTICK, "rstick")

enum {
#define GE_SRC_ENUM_(id, lo) GE_SRC_##id,
    GE_SOURCE_LIST(GE_SRC_ENUM_)
#undef GE_SRC_ENUM_
    GE_SRC_MAX
};

/* ---- keyboard movement axes ----------------------------------------------
 *
 * M(id, config-name, ENV-SUFFIX)
 *
 * Separate from actions because they are not buttons: each one deflects a virtual stick
 * rather than asserting a game action, and on a pad they come from a real analogue axis
 * and are not bindable at all. Keyboard only.
 *
 * LOOK_* drive the right stick, which gePortDecodePad thresholds into C-buttons. They
 * remain useful with the mouse enabled -- the C-buttons are how the front-end menus are
 * driven -- so they are bound by default in every preset.
 */
#define GE_AXIS_LIST(M)                              \
    M(FORWARD,      "forward",      "FORWARD")       \
    M(BACKWARD,     "backward",     "BACKWARD")      \
    M(STRAFE_LEFT,  "strafe_left",  "STRAFE_LEFT")   \
    M(STRAFE_RIGHT, "strafe_right", "STRAFE_RIGHT")  \
    M(LOOK_UP,      "look_up",      "LOOK_UP")       \
    M(LOOK_DOWN,    "look_down",    "LOOK_DOWN")     \
    M(LOOK_LEFT,    "look_left",    "LOOK_LEFT")     \
    M(LOOK_RIGHT,   "look_right",   "LOOK_RIGHT")

enum {
#define GE_AXIS_ENUM_(id, lo, up) GE_AXIS_##id,
    GE_AXIS_LIST(GE_AXIS_ENUM_)
#undef GE_AXIS_ENUM_
    GE_AXIS_MAX
};

/* ---- presets -------------------------------------------------------------
 *
 * A preset is a complete set of defaults for BOTH the keyboard and the pad. It is only
 * ever a starting point: an explicit binding, from the environment or the config file,
 * always wins over whatever the preset says (see geBindSrc/geKeyBinding).
 *
 * GE_PRESET_MODERN is the default. GE_PRESET_N64 reproduces the bindings this port
 * shipped with before remapping existed, byte for byte, so a player who preferred them
 * -- or a test that depends on them -- can ask for them by name rather than by
 * reconstructing nine keys by hand.
 */
enum {
    GE_PRESET_MODERN = 0,
    GE_PRESET_N64,
    GE_PRESET_MAX
};

/* ---- hold vs toggle ------------------------------------------------------
 *
 * Applies to AIM and CROUCH. HOLD is momentary: the action is live exactly while the
 * input is down. TOGGLE latches on the press and stays until the next press.
 *
 * The two are implemented in different places and cannot share code. Crouch latches in
 * the port layer (ge_bindings.c), because the engine has no crouch button to latch --
 * crouch reaches the game as a bool the port asserts. Aim latches in the ENGINE: the
 * retail options menu already has a per-player aim-control setting that bondview2.c
 * reads as either a level or a rising edge, so aim=toggle sets that option rather than
 * second-guessing it from outside. See docs/CONTROLS.md.
 */
enum {
    GE_HOLD = 0,
    GE_TOGGLE
};

#ifdef __cplusplus
}
#endif

#endif /* GE_ACTIONS_H */
