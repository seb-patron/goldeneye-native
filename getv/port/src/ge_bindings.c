/* GoldenEye native port - action binding resolution. See ge_bindings.h for why this
 * unit exists and what may not be included here.
 *
 * <SDL.h> and <PR/os.h> are both forbidden in this file. The point of it is to be
 * callable from port_input.c (which has SDL) and port_os.c (which has PR/os.h), and
 * including either would immediately halve that. The keyboard defaults below are
 * therefore SDL scancode NAMES as strings, resolved to scancodes by port_input.c --
 * which also means every preset lives in one file and can be tested without a window.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_actions.h"
#include "ge_bindings.h"
#include "port_input.h"

/* ---- name tables ---------------------------------------------------------
 * Generated from the X-macro lists, so they cannot drift out of step with the enums
 * the way the four hand-maintained tables in port_os.c could. */

static const char *const ge_act_name[] = {
#define M(id, lo, up) lo,
    GE_ACTION_LIST(M)
#undef M
    NULL
};

static const char *const ge_act_env[] = {
#define M(id, lo, up) up,
    GE_ACTION_LIST(M)
#undef M
    NULL
};

static const char *const ge_src_name[] = {
#define M(id, lo) lo,
    GE_SOURCE_LIST(M)
#undef M
    NULL
};

static const char *const ge_axis_name[] = {
#define M(id, lo, up) lo,
    GE_AXIS_LIST(M)
#undef M
    NULL
};

static const char *const ge_axis_env[] = {
#define M(id, lo, up) up,
    GE_AXIS_LIST(M)
#undef M
    NULL
};

const char *geActionName(int a)      { return (a >= 0 && a < GE_ACT_MAX)  ? ge_act_name[a]  : "?"; }
const char *geActionEnvSuffix(int a) { return (a >= 0 && a < GE_ACT_MAX)  ? ge_act_env[a]   : "?"; }
const char *geSourceName(int s)      { return (s >= 0 && s < GE_SRC_MAX)  ? ge_src_name[s]  : "?"; }
const char *geAxisName(int x)        { return (x >= 0 && x < GE_AXIS_MAX) ? ge_axis_name[x] : "?"; }
const char *geAxisEnvSuffix(int x)   { return (x >= 0 && x < GE_AXIS_MAX) ? ge_axis_env[x]  : "?"; }

const char *gePresetName(int p)
{
    switch (p) {
        case GE_PRESET_MODERN: return "modern";
        case GE_PRESET_N64:    return "n64";
        default:               return "?";
    }
}

/* ---- parsing -------------------------------------------------------------
 *
 * An unrecognised value keeps the fallback and warns rather than unbinding. A typo in a
 * config file must not silently remove the fire button: "nothing happens when I press
 * it" is far harder to diagnose than a line on stdout, and "none" already exists for
 * anyone who genuinely wants an action unbound. */
int geParseSource(const char *v, int fallback)
{
    int i;

    if (v == NULL || *v == '\0') { return fallback; }
    for (i = 0; i < GE_SRC_MAX; i++) {
        if (strcmp(v, ge_src_name[i]) == 0) { return i; }
    }
    printf("[getv] input: pad binding \"%s\" not recognised -- expected one of "
           "a/b/x/y/lb/rb/lt/rt/start/back/dup/ddown/dleft/dright/lstick/rstick/none; "
           "keeping the default\n", v);
    fflush(stdout);
    return fallback;
}

int geParsePreset(const char *v, int fallback)
{
    if (v == NULL || *v == '\0')     { return fallback; }
    if (strcmp(v, "modern") == 0)    { return GE_PRESET_MODERN; }
    if (strcmp(v, "n64") == 0)       { return GE_PRESET_N64; }
    /* "classic" is accepted as a synonym for n64: the launcher and the docs both use
     * "classic" for the retail control STYLES (1.1 Honey and friends), and a player who
     * has just read that page reaches for the same word here. */
    if (strcmp(v, "classic") == 0)   { return GE_PRESET_N64; }
    printf("[getv] input: preset \"%s\" not recognised -- expected modern or n64; "
           "keeping %s\n", v, gePresetName(fallback));
    fflush(stdout);
    return fallback;
}

int geParseHoldToggle(const char *v, int fallback)
{
    if (v == NULL || *v == '\0')   { return fallback; }
    if (strcmp(v, "hold") == 0)    { return GE_HOLD; }
    if (strcmp(v, "toggle") == 0)  { return GE_TOGGLE; }
    /* Numeric spelling so the older GETV_AIM_TOGGLE=1 keeps meaning what it meant. */
    if (strcmp(v, "1") == 0)       { return GE_TOGGLE; }
    if (strcmp(v, "0") == 0)       { return GE_HOLD; }
    printf("[getv] input: mode \"%s\" not recognised -- expected hold or toggle; "
           "keeping %s\n", v, (fallback == GE_TOGGLE) ? "toggle" : "hold");
    fflush(stdout);
    return fallback;
}

/* ---- presets -------------------------------------------------------------
 *
 * Indexed [preset][action]. Sized by GE_ACT_MAX, so a new action added to
 * GE_ACTION_LIST without a line here reads GE_SRC_NONE -- unbound, silently. The
 * designated initialisers make that visible: every action is named, so a missing one
 * is missing from a list you can read top to bottom.
 */
static const int ge_pad_preset[GE_PRESET_MAX][GE_ACT_MAX] = {
    /* MODERN -- the layout a player coming from any shooter of the last fifteen years
     * expects. Face buttons are POSITIONAL (see the profile note in port_input.h), so
     * "a" is Cross on a DualSense and B on a Switch Pro; the launcher prints the right
     * glyph for the attached pad.
     *
     *   RT fire / LT aim      the near-universal shooter convention
     *   south use             interact is the bottom face button everywhere
     *   west  reload          X / Square, likewise
     *   east  crouch          B / Circle
     *   north weapon_next     Y / Triangle, the "swap weapon" button
     *
     * This moves USE off the east face button, where every previous build of this port
     * had it, and moves WEAPON_NEXT off south. That is a deliberate default change and
     * the reason GE_PRESET_N64 exists unchanged below. */
    [GE_PRESET_MODERN] = {
        [GE_ACT_FIRE]        = GE_SRC_RT,
        [GE_ACT_AIM]         = GE_SRC_LT,
        [GE_ACT_USE]         = GE_SRC_A,
        [GE_ACT_RELOAD]      = GE_SRC_X,
        [GE_ACT_CROUCH]      = GE_SRC_B,
        [GE_ACT_WEAPON_NEXT] = GE_SRC_Y,
        /* Unbound by default even here: the back-cycle is synthesised from the retail
         * inventory+fire gesture rather than being a real engine input (see
         * gePortDecodePad), and it is unverified on hardware. The mouse wheel binds it
         * because a wheel notch is unambiguous; a face button is not worth the risk. */
        [GE_ACT_WEAPON_PREV] = GE_SRC_NONE,
        [GE_ACT_PAUSE]       = GE_SRC_START,
    },

    /* N64 -- byte for byte what this port defaulted to before remapping existed, so
     * `input_preset = n64` is a true no-op revert rather than an approximation.
     * Everything the modern preset added is unbound, because none of it existed. */
    [GE_PRESET_N64] = {
        [GE_ACT_FIRE]        = GE_SRC_RT,
        [GE_ACT_AIM]         = GE_SRC_LT,
        [GE_ACT_USE]         = GE_SRC_B,
        [GE_ACT_RELOAD]      = GE_SRC_NONE,
        [GE_ACT_CROUCH]      = GE_SRC_NONE,
        [GE_ACT_WEAPON_NEXT] = GE_SRC_A,
        [GE_ACT_WEAPON_PREV] = GE_SRC_NONE,
        [GE_ACT_PAUSE]       = GE_SRC_START,
    },
};

/* Keyboard defaults, as SDL scancode names. Comma-separated means "any of these";
 * port_input.c resolves each name with SDL_GetScancodeFromName, which is the same
 * mechanism GETV_CONSOLE_KEY already uses, so the spellings here are exactly the
 * spellings a player may write in goldeneye.cfg.
 *
 * An empty string is unbound. */
static const char *const ge_key_preset[GE_PRESET_MAX][GE_ACT_MAX] = {
    /* MODERN. Fire and aim are the mouse buttons; the keys below are the keyboard-only
     * fallbacks for someone playing without one. */
    [GE_PRESET_MODERN] = {
        /* The mouse buttons are BINDINGS now, not a hard-wired special case. They used
         * to be wired straight onto the trigger fields in geMousePoll, which meant they
         * were not bound to fire and aim so much as bound to whatever fire and aim
         * happened to sit on -- rebinding aim to a face button took it off the right
         * mouse button too. Naming them here is what separates the two. */
        [GE_ACT_FIRE]        = "mouse1,Space",
        [GE_ACT_AIM]         = "mouse2",
        [GE_ACT_USE]         = "E,F",
        [GE_ACT_RELOAD]      = "R",
        [GE_ACT_CROUCH]      = "C,Left Ctrl",
        /* Q is free in this preset -- aim moved to the right mouse button -- so it
         * takes weapon-next, alongside the wheel. Return is a third alternative only.
         * It used to be load-bearing -- weapon_next was the keyboard's N64 A, the
         * menu confirm -- but menus now read fixed keys through geMenuButtons, so no
         * binding here can strand a player on a menu screen. */
        [GE_ACT_WEAPON_NEXT] = "wheelup,Q,Return",
        [GE_ACT_WEAPON_PREV] = "wheeldown",
        [GE_ACT_PAUSE]       = "Tab,Keypad Enter",
    },

    /* N64 -- the pre-remap keyboard map exactly: Space/LCtrl fire, Q aim, E/F use,
     * Return/R weapon, Tab start, C/LShift crouch, V stand. R is weapon_next here, not
     * reload, which is the one difference a player switching presets will feel. */
    [GE_PRESET_N64] = {
        /* mouse1/mouse2 reproduce the hard-wired left-fires / right-aims behaviour this
         * port had before mouse buttons became bindable. */
        [GE_ACT_FIRE]        = "Space,Left Ctrl,mouse1",
        [GE_ACT_AIM]         = "Q,mouse2",
        [GE_ACT_USE]         = "E,F",
        [GE_ACT_RELOAD]      = "",
        [GE_ACT_CROUCH]      = "C,Left Shift",
        [GE_ACT_WEAPON_NEXT] = "Return,R",
        [GE_ACT_WEAPON_PREV] = "",
        [GE_ACT_PAUSE]       = "Tab,Keypad Enter",
    },
};

/* Movement and look. Identical in both presets -- WASD plus arrows was already the
 * layout and there is no second convention worth offering. Kept in the preset table
 * anyway so a future preset can differ without a new mechanism. */
static const char *const ge_axis_preset[GE_PRESET_MAX][GE_AXIS_MAX] = {
    [GE_PRESET_MODERN] = {
        [GE_AXIS_FORWARD]      = "W",
        [GE_AXIS_BACKWARD]     = "S",
        [GE_AXIS_STRAFE_LEFT]  = "A",
        [GE_AXIS_STRAFE_RIGHT] = "D",
        [GE_AXIS_LOOK_UP]      = "Up",
        [GE_AXIS_LOOK_DOWN]    = "Down",
        [GE_AXIS_LOOK_LEFT]    = "Left",
        [GE_AXIS_LOOK_RIGHT]   = "Right",
    },
    [GE_PRESET_N64] = {
        [GE_AXIS_FORWARD]      = "W",
        [GE_AXIS_BACKWARD]     = "S",
        [GE_AXIS_STRAFE_LEFT]  = "A",
        [GE_AXIS_STRAFE_RIGHT] = "D",
        [GE_AXIS_LOOK_UP]      = "Up",
        [GE_AXIS_LOOK_DOWN]    = "Down",
        [GE_AXIS_LOOK_LEFT]    = "Left",
        [GE_AXIS_LOOK_RIGHT]   = "Right",
    },
};

int geInputPreset(void)
{
    static int p = -1;
    if (p < 0) {
        p = geParsePreset(getenv("GETV_INPUT_PRESET"), GE_PRESET_MODERN);
        printf("[getv] input: preset \"%s\"\n", gePresetName(p));
        fflush(stdout);
    }
    return p;
}

int gePresetSource(int preset, int act)
{
    if (preset < 0 || preset >= GE_PRESET_MAX) { preset = GE_PRESET_MODERN; }
    if (act < 0 || act >= GE_ACT_MAX)          { return GE_SRC_NONE; }
    return ge_pad_preset[preset][act];
}

const char *gePresetKeys(int preset, int act)
{
    const char *s;
    if (preset < 0 || preset >= GE_PRESET_MAX) { preset = GE_PRESET_MODERN; }
    if (act < 0 || act >= GE_ACT_MAX)          { return ""; }
    s = ge_key_preset[preset][act];
    return (s != NULL) ? s : "";
}

const char *gePresetAxisKeys(int preset, int axis)
{
    const char *s;
    if (preset < 0 || preset >= GE_PRESET_MAX) { preset = GE_PRESET_MODERN; }
    if (axis < 0 || axis >= GE_AXIS_MAX)       { return ""; }
    s = ge_axis_preset[preset][axis];
    return (s != NULL) ? s : "";
}

/* ---- pad binding resolution ---------------------------------------------- */

/* Is `v` a recognised source name? Silent, unlike geParseSource, because the resolver
 * asks this about every key and a typo has already been reported once by the parse. */
static int geSourceNameValid(const char *v)
{
    int i;
    if (v == NULL || *v == '\0') { return 0; }
    for (i = 0; i < GE_SRC_MAX; i++) {
        if (strcmp(v, ge_src_name[i]) == 0) { return 1; }
    }
    return 0;
}

/* Compose "GETV_BIND_<ACT>" (player < 1) or "GETV_P<n>_BIND_<ACT>". */
static void geBindKey(char *dst, size_t cap, int player, const char *suffix)
{
    if (player >= 1) {
        snprintf(dst, cap, "GETV_P%d_BIND_%s", player, suffix);
    } else {
        snprintf(dst, cap, "GETV_BIND_%s", suffix);
    }
}

/* Bindings are PER PLAYER, resolved most-specific first: GETV_P2_BIND_FIRE, then the
 * global GETV_BIND_FIRE, then the preset. Split-screen is the reason -- with one global
 * table, moving fire off the right trigger for a player on a Nintendo pad moved it for
 * everyone, so a mixed set of controllers could not be accommodated at all. */
int geBindSrc(int player, int act)
{
    static int resolved = 0;
    static int src[GE_PORT_MAX_PADS][GE_ACT_MAX];

    if (!resolved) {
        const int preset = geInputPreset();
        int p, a;

        /* 1 where the value came from the environment/config rather than the preset. */
        int explicit_bind[GE_PORT_MAX_PADS][GE_ACT_MAX];

        for (a = 0; a < GE_ACT_MAX; a++) {
            char key[64];
            int g, g_explicit;

            geBindKey(key, sizeof key, 0, ge_act_env[a]);
            g = geParseSource(getenv(key), ge_pad_preset[preset][a]);
            g_explicit = geSourceNameValid(getenv(key));

            for (p = 0; p < GE_PORT_MAX_PADS; p++) {
                geBindKey(key, sizeof key, p + 1, ge_act_env[a]);
                src[p][a] = geParseSource(getenv(key), g);
                explicit_bind[p][a] = g_explicit || geSourceNameValid(getenv(key));
            }
        }

        /* An explicit binding claims its button; a preset default yields rather than
         * doubling up on it.
         *
         * Found on the first real launch. Every config written by the pre-remap template
         * carries explicit `use = b` and `weapon_next = a` lines, and the modern preset puts
         * crouch on b -- so an existing install came up with B doing use AND crouch, and a
         * player who pressed it to open a door would also drop to a squat. "Explicit beats
         * the preset" already governs a single action; this extends it to the button the
         * explicit binding sits on.
         *
         * Only a PRESET default is ever dropped. Two actions the player explicitly put on
         * one button are both kept: that is a choice, not a migration accident. */
        for (p = 0; p < GE_PORT_MAX_PADS; p++) {
            for (a = 0; a < GE_ACT_MAX; a++) {
                int b;
                if (explicit_bind[p][a] || src[p][a] == GE_SRC_NONE) { continue; }
                for (b = 0; b < GE_ACT_MAX; b++) {
                    if (b == a || !explicit_bind[p][b] || src[p][b] != src[p][a]) { continue; }
                    if (p == 0) {
                        printf("[getv] input: pad %s default (%s) dropped -- %s is explicitly bound "
                               "to %s; set %s yourself to keep both\n",
                               ge_act_name[a], ge_src_name[src[p][a]], ge_src_name[src[p][a]],
                               ge_act_name[b], ge_act_name[a]);
                    }
                    src[p][a] = GE_SRC_NONE;
                    break;
                }
            }
        }
        resolved = 1;

        /* Positive confirmation of what each action resolved to. With only the
         * "not recognised" warning, a correctly-applied binding produced no evidence at
         * all, so a config key that silently failed to reach here was indistinguishable
         * from one that worked.
         *
         * Player 1 is always printed; the others only when they differ, so the common
         * case stays one line and a per-player override is impossible to miss. */
        for (p = 0; p < GE_PORT_MAX_PADS; p++) {
            int differs = 0;
            for (a = 0; a < GE_ACT_MAX; a++) {
                if (src[p][a] != src[0][a]) { differs = 1; }
            }
            if (p > 0 && !differs) { continue; }

            printf("[getv] input: pad bindings, player %d --", p + 1);
            for (a = 0; a < GE_ACT_MAX; a++) {
                printf(" %s=%s", ge_act_name[a], ge_src_name[src[p][a]]);
            }
            printf("\n");
        }
        fflush(stdout);
    }

    if (player < 0 || player >= GE_PORT_MAX_PADS) { player = 0; }
    return (act >= 0 && act < GE_ACT_MAX) ? src[player][act] : GE_SRC_NONE;
}

int geSourceHeld(const struct GePadState *st, int src)
{
    if (st == NULL) { return 0; }
    switch (src) {
        case GE_SRC_A:      return st->a;
        case GE_SRC_B:      return st->b;
        case GE_SRC_X:      return st->x;
        case GE_SRC_Y:      return st->y;
        case GE_SRC_LB:     return st->lshoulder;
        case GE_SRC_RB:     return st->rshoulder;
        case GE_SRC_LT:     return st->ltrigger;
        case GE_SRC_RT:     return st->rtrigger;
        /* START also accepts BACK. The N64 has no fifth face bit for Back to map to and
         * every front.c menu branch accepts START_BUTTON, so aliasing costs nothing and
         * saves a player whose pad labels the button the other way. */
        case GE_SRC_START:  return st->start || st->back;
        case GE_SRC_BACK:   return st->back;
        case GE_SRC_DUP:    return st->dup;
        case GE_SRC_DDOWN:  return st->ddown;
        case GE_SRC_DLEFT:  return st->dleft;
        case GE_SRC_DRIGHT: return st->dright;
        case GE_SRC_LSTICK: return st->lstickbtn;
        case GE_SRC_RSTICK: return st->rstickbtn;
        default:            return 0;
    }
}

int geActionHeld(const struct GePadState *st, int player, int act)
{
    if (st == NULL || act < 0 || act >= GE_ACT_MAX) { return 0; }
    /* Keyboard/mouse first: it is a direct assertion and needs no lookup. The OR is the
     * whole point of splitting the two layers -- either device can drive the action and
     * neither remap disturbs the other. */
    if (st->act[act]) { return 1; }
    return geSourceHeld(st, geBindSrc(player, act));
}

/* ---- menus -------------------------------------------------------------------
 *
 * The N64 buttons a front-end menu sees, taken from fixed positions and ignoring every
 * gameplay binding. See GePadState::menu_confirm in port_input.h for why: bindings that
 * make sense in a level put confirm on "back", turned the wheel into "select", and let a
 * launcher rebind remove the keyboard's only way past the mission report.
 *
 *   A (select)  pad bottom face button, Return, Space, left click
 *   B (back)    pad right face button, Backspace, right click
 *   START       pad Start or Back, Tab, Keypad Enter
 *   Z           right trigger     (every front.c select also accepts Z)
 *   L / R       shoulders, left trigger as L   (folder deletion reads L/R)
 *
 * Left click is A, not START, deliberately. front.c treats them differently: on the
 * mission briefing START launches the mission from any page while A acts on the
 * highlighted tab (front.c:7265-7292), in the 007 and multiplayer options START is a
 * separate shortcut (:4098, :4968), and the cheat menu does not accept START at all
 * (:8180). A picks the thing under the cursor on every one of them.
 *
 * Positional like every other pad source: "bottom face button" is Cross on a DualSense
 * and B on a Switch Pro. */
unsigned geMenuButtons(const struct GePadState *st)
{
    unsigned n = 0;

    if (st == NULL) { return 0; }
    if (st->a || st->menu_confirm)                  { n |= GE_N64_A; }
    if (st->b || st->menu_back)                     { n |= GE_N64_B; }
    if (st->start || st->back || st->menu_start)    { n |= GE_N64_START; }
    if (st->rtrigger)                               { n |= GE_N64_Z; }
    if (st->lshoulder || st->ltrigger)              { n |= GE_N64_L; }
    if (st->rshoulder)                              { n |= GE_N64_R; }
    return n | (st->n64 & GE_N64_ALL);
}

/* ---- hold vs toggle ------------------------------------------------------ */

int geAimMode(void)
{
    static int m = -1;
    if (m < 0) {
        /* GETV_AIM_MODE is the new spelling; GETV_AIM_TOGGLE is what shipped and is
         * still honoured, so an existing config keeps working. */
        const char *s = getenv("GETV_AIM_MODE");
        if (s == NULL || *s == '\0') { s = getenv("GETV_AIM_TOGGLE"); }
        m = geParseHoldToggle(s, GE_AIM_MODE_DEFAULT);
    }
    return m;
}

int geCrouchMode(void)
{
    static int m = -1;
    if (m < 0) {
        m = geParseHoldToggle(getenv("GETV_CROUCH_MODE"), GE_CROUCH_MODE_DEFAULT);
    }
    return m;
}

/* Is RELOAD bound to anything at all, on either device?
 *
 * Drives whether USE keeps its retail double duty. Retail reload is the use button with
 * nothing to use -- bond_interact_object() returns true only when no prop is in range --
 * and that is fine when it is the ONLY way to reload. Once R exists it is just a second,
 * unpredictable trigger: the same key reloads or opens a door depending on where you
 * happen to be standing, which is exactly the behaviour a dedicated key is meant to
 * replace.
 *
 * Checked rather than assumed, because `input_preset = n64` leaves reload unbound and
 * must keep the retail behaviour it describes. GETV_USE_RELOADS forces either way. */
int geUseAlsoReloads(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GETV_USE_RELOADS");
        if (e != NULL && *e != '\0') {
            v = (atoi(e) != 0);
        } else {
            int p = geInputPreset();
            int pad_bound = (geBindSrc(0, GE_ACT_RELOAD) != GE_SRC_NONE);
            int key_bound = 0;
            char key[64];
            const char *kv;
            snprintf(key, sizeof key, "GETV_KEY_%s", geActionEnvSuffix(GE_ACT_RELOAD));
            kv = getenv(key);
            if (kv == NULL) { kv = gePresetKeys(p, GE_ACT_RELOAD); }
            key_bound = (kv != NULL && *kv != '\0' && strcmp(kv, "none") != 0);
            v = !(pad_bound || key_bound);
        }
        printf("[getv] input: use button %s reload when nothing is in reach\n",
               v ? "DOES" : "does NOT");
        fflush(stdout);
    }
    return v;
}

/* ---- per-frame state -----------------------------------------------------
 *
 * Crouch reaches the game as `moveData.crouchDown`, which calls
 * currentPlayerAdjustCrouchPos(-2) -- a clamped step from CROUCH_STAND straight to
 * CROUCH_SQUAT. Two consequences shape everything below:
 *
 *   - Holding crouchDown is harmless. The value saturates at CROUCH_SQUAT and further
 *     calls do nothing, so a latched crouch can simply assert it every frame.
 *   - Standing back up needs crouchUp asserted for at least one frame, and must NOT be
 *     asserted continuously. bondview2.c reads `if (crouchDown) ... else if (crouchUp)`,
 *     so a permanently-true crouchUp would cancel the retail aim-stick crouch the
 *     instant the stick returned to centre.
 *
 * So standing up is a PULSE, fired on the edge that ends a crouch. Two frames rather
 * than one: the game runs render-only ticks (see the 0030 autocrouch patch) and a
 * single-frame pulse aimed at a tick that does no movement processing would be lost.
 */
#define GE_STAND_PULSE_FRAMES 2

struct GeFrameState {
    int prev_crouch;
    int prev_reload;
    int latched;        /* toggle mode only */
    int stand_pulse;    /* frames of crouchUp still owed */
    int reload_edge;
    int crouch_active;
    int stand_active;
};

static struct GeFrameState ge_frame[GE_PORT_MAX_PADS];

void geBindingsReset(void)
{
    memset(ge_frame, 0, sizeof ge_frame);
}

void geBindingsFrame(int player, const struct GePadState *st)
{
    struct GeFrameState *f;
    int raw_crouch, raw_reload;

    if (player < 0 || player >= GE_PORT_MAX_PADS) { return; }
    f = &ge_frame[player];

    raw_crouch = geActionHeld(st, player, GE_ACT_CROUCH);
    raw_reload = geActionHeld(st, player, GE_ACT_RELOAD);

    f->reload_edge = (raw_reload && !f->prev_reload);

    if (geCrouchMode() == GE_TOGGLE) {
        /* Press to crouch, press again to stand. There is no separate stand input and
         * deliberately so: a key that does nothing except when you are already crouched
         * is a key nobody finds, and the first version of this shipped one. */
        if (raw_crouch && !f->prev_crouch) {
            f->latched = !f->latched;
            if (!f->latched) { f->stand_pulse = GE_STAND_PULSE_FRAMES; }
        }
        f->crouch_active = f->latched;
    } else {
        f->crouch_active = raw_crouch;
        /* Releasing crouch stands you up. Before remapping, crouch was momentary but
         * nothing watched the release, so a tap left Bond squatting indefinitely. */
        if (!raw_crouch && f->prev_crouch) { f->stand_pulse = GE_STAND_PULSE_FRAMES; }
        f->latched = 0;
    }

    /* Standing is only ever the pulse now. It must not be a level: bondview2.c reads
     * `if (crouchDown) ... else if (crouchUp)`, so a permanently-true crouchUp would
     * cancel the retail aim-stick crouch the instant the stick recentred. */
    f->stand_active = 0;
    if (f->stand_pulse > 0) {
        f->stand_pulse--;
        f->stand_active = 1;
    }

    f->prev_crouch = raw_crouch;
    f->prev_reload = raw_reload;
}

int geCrouchActive(int player)
{
    if (player < 0 || player >= GE_PORT_MAX_PADS) { return 0; }
    return ge_frame[player].crouch_active;
}

int geStandActive(int player)
{
    if (player < 0 || player >= GE_PORT_MAX_PADS) { return 0; }
    return ge_frame[player].stand_active;
}

int geReloadEdge(int player)
{
    if (player < 0 || player >= GE_PORT_MAX_PADS) { return 0; }
    return ge_frame[player].reload_edge;
}
