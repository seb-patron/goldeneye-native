/* Unit tests for the action binding layer -- ge_bindings.c.
 *
 * The unit is included directly, the way every test here does it, so the statics that
 * hold the resolved tables and the per-frame latch are reachable. That matters more
 * than usual for this one: geBindSrc() caches its whole table on first call and
 * geBindingsFrame() carries state between frames, and a test that could not reset
 * either would be able to check the first case only.
 *
 * No SDL and no window. ge_bindings.c includes neither <SDL.h> nor <PR/os.h> -- that
 * is the entire reason it exists as a separate unit -- so the binding logic, the
 * presets and the crouch hold/toggle state machine are all testable in a plain process.
 * The keyboard half (scancode names) needs SDL and lives in port_input.c; what is
 * checked here is everything downstream of a resolved binding.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_bindings.c"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void eq_int(int got, int want, const char *what)
{
    checks++;
    if (got == want) {
        printf("ok   %s (%d)\n", what, got);
    } else {
        failures++;
        printf("FAIL %s: expected %d, got %d\n", what, want, got);
    }
}

static void eq_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (got != NULL && strcmp(got, want) == 0) {
        printf("ok   %s (%s)\n", what, got);
    } else {
        failures++;
        printf("FAIL %s: expected \"%s\", got \"%s\"\n", what, want,
               (got != NULL) ? got : "(null)");
    }
}

/* geBindSrc() resolves once and caches, which is correct at runtime and useless in a
 * test that wants to see more than one configuration. Reaching into the static is the
 * reason the unit is #included rather than linked. */
static void forget_bindings(void)
{
    /* `resolved` and `src` are function-scope statics inside geBindSrc, so they cannot
     * be reached by name. Re-resolving is instead forced by running each configuration
     * in its own process -- see main(). This helper exists to document that, and to
     * clear the per-frame state, which IS file-scope. */
    geBindingsReset();
}

/* ---- names and parsing --------------------------------------------------- */

static void test_names(void)
{
    printf("# names generated from the X-macro lists\n");

    eq_str(geActionName(GE_ACT_FIRE),        "fire",        "action name fire");
    eq_str(geActionName(GE_ACT_RELOAD),      "reload",      "action name reload");
    eq_str(geActionName(GE_ACT_CROUCH),      "crouch",      "action name crouch");
    eq_str(geActionName(GE_ACT_WEAPON_PREV), "weapon_prev", "action name weapon_prev");
    eq_str(geActionEnvSuffix(GE_ACT_RELOAD), "RELOAD",      "env suffix reload");

    eq_str(geSourceName(GE_SRC_NONE),   "none",   "source name none");
    eq_str(geSourceName(GE_SRC_RT),     "rt",     "source name rt");
    eq_str(geSourceName(GE_SRC_RSTICK), "rstick", "source name rstick");
    eq_str(geSourceName(GE_SRC_DDOWN),  "ddown",  "source name ddown");

    eq_str(geAxisName(GE_AXIS_STRAFE_LEFT),      "strafe_left",  "axis name strafe_left");
    eq_str(geAxisEnvSuffix(GE_AXIS_STRAFE_LEFT), "STRAFE_LEFT",  "axis env suffix");

    /* Out of range must be inert, not a read past the end of the table. A trace line
     * printing a neighbouring name is worse than printing nothing, because it looks
     * authoritative -- the exact failure the old parallel tables in port_os.c warned
     * about. */
    eq_str(geActionName(-1),          "?", "action name below range");
    eq_str(geActionName(GE_ACT_MAX),  "?", "action name above range");
    eq_str(geSourceName(GE_SRC_MAX),  "?", "source name above range");
    eq_str(geAxisName(999),           "?", "axis name far above range");

    /* Every action and every source must have a non-empty, distinct name. A duplicate
     * would mean two X() lines collided and one binding silently shadows the other. */
    {
        int a, b, dup = 0, empty = 0;
        for (a = 0; a < GE_ACT_MAX; a++) {
            if (geActionName(a)[0] == '\0') { empty++; }
            for (b = a + 1; b < GE_ACT_MAX; b++) {
                if (strcmp(geActionName(a), geActionName(b)) == 0) { dup++; }
            }
        }
        eq_int(empty, 0, "no action has an empty name");
        eq_int(dup,   0, "no two actions share a name");

        dup = empty = 0;
        for (a = 0; a < GE_SRC_MAX; a++) {
            if (geSourceName(a)[0] == '\0') { empty++; }
            for (b = a + 1; b < GE_SRC_MAX; b++) {
                if (strcmp(geSourceName(a), geSourceName(b)) == 0) { dup++; }
            }
        }
        eq_int(empty, 0, "no source has an empty name");
        eq_int(dup,   0, "no two sources share a name");
    }
}

static void test_parsing(void)
{
    printf("# parsing keeps the fallback rather than unbinding\n");

    eq_int(geParseSource("rt",     GE_SRC_NONE), GE_SRC_RT,     "parse rt");
    eq_int(geParseSource("lstick", GE_SRC_NONE), GE_SRC_LSTICK, "parse lstick");
    eq_int(geParseSource("none",   GE_SRC_RT),   GE_SRC_NONE,   "parse explicit none");

    /* The important one. A typo must not silently remove the fire button: "nothing
     * happens when I press it" is far harder to diagnose than a warning line, and
     * "none" already exists for anyone who wants an action genuinely unbound. */
    eq_int(geParseSource("rtt",  GE_SRC_RT), GE_SRC_RT, "typo keeps the fallback");
    eq_int(geParseSource("",     GE_SRC_RT), GE_SRC_RT, "empty keeps the fallback");
    eq_int(geParseSource(NULL,   GE_SRC_RT), GE_SRC_RT, "NULL keeps the fallback");

    eq_int(geParsePreset("modern",  GE_PRESET_N64),    GE_PRESET_MODERN, "parse modern");
    eq_int(geParsePreset("n64",     GE_PRESET_MODERN), GE_PRESET_N64,    "parse n64");
    eq_int(geParsePreset("classic", GE_PRESET_MODERN), GE_PRESET_N64,    "classic aliases n64");
    eq_int(geParsePreset("nope",    GE_PRESET_MODERN), GE_PRESET_MODERN, "bad preset keeps fallback");

    eq_int(geParseHoldToggle("hold",   GE_TOGGLE), GE_HOLD,   "parse hold");
    eq_int(geParseHoldToggle("toggle", GE_HOLD),   GE_TOGGLE, "parse toggle");
    /* GETV_AIM_TOGGLE=1 shipped before hold/toggle had names; a config written against
     * it must keep meaning what it meant. */
    eq_int(geParseHoldToggle("1", GE_HOLD),   GE_TOGGLE, "numeric 1 is toggle");
    eq_int(geParseHoldToggle("0", GE_TOGGLE), GE_HOLD,   "numeric 0 is hold");
}

/* ---- presets ------------------------------------------------------------- */

static void test_presets(void)
{
    int a;

    printf("# presets\n");

    /* The N64 preset is a compatibility contract, not a preference. It must reproduce
     * what this port defaulted to before remapping existed, or `input_preset = n64` is
     * an approximation rather than a revert. These five values are that old table. */
    eq_int(gePresetSource(GE_PRESET_N64, GE_ACT_FIRE),        GE_SRC_RT,    "n64 fire=rt");
    eq_int(gePresetSource(GE_PRESET_N64, GE_ACT_AIM),         GE_SRC_LT,    "n64 aim=lt");
    eq_int(gePresetSource(GE_PRESET_N64, GE_ACT_USE),         GE_SRC_B,     "n64 use=b");
    eq_int(gePresetSource(GE_PRESET_N64, GE_ACT_WEAPON_NEXT), GE_SRC_A,     "n64 weapon_next=a");
    eq_int(gePresetSource(GE_PRESET_N64, GE_ACT_WEAPON_PREV), GE_SRC_NONE,  "n64 weapon_prev unbound");
    eq_int(gePresetSource(GE_PRESET_N64, GE_ACT_PAUSE),       GE_SRC_START, "n64 pause=start");
    /* Nothing the modern preset added may leak into it. */
    eq_int(gePresetSource(GE_PRESET_N64, GE_ACT_RELOAD), GE_SRC_NONE, "n64 reload unbound");
    eq_int(gePresetSource(GE_PRESET_N64, GE_ACT_CROUCH), GE_SRC_NONE, "n64 crouch unbound on pad");

    eq_int(gePresetSource(GE_PRESET_MODERN, GE_ACT_FIRE),        GE_SRC_RT, "modern fire=rt");
    eq_int(gePresetSource(GE_PRESET_MODERN, GE_ACT_AIM),         GE_SRC_LT, "modern aim=lt");
    eq_int(gePresetSource(GE_PRESET_MODERN, GE_ACT_USE),         GE_SRC_A,  "modern use=south");
    eq_int(gePresetSource(GE_PRESET_MODERN, GE_ACT_RELOAD),      GE_SRC_X,  "modern reload=west");
    eq_int(gePresetSource(GE_PRESET_MODERN, GE_ACT_CROUCH),      GE_SRC_B,  "modern crouch=east");
    eq_int(gePresetSource(GE_PRESET_MODERN, GE_ACT_WEAPON_NEXT), GE_SRC_Y,  "modern weapon_next=north");

    /* No two actions may share a pad source within one preset, or one press does two
     * things and the second is undiscoverable. GE_SRC_NONE is exempt: several actions
     * are deliberately unbound. */
    {
        int p;
        for (p = 0; p < GE_PRESET_MAX; p++) {
            int b, clash = 0;
            for (a = 0; a < GE_ACT_MAX; a++) {
                if (gePresetSource(p, a) == GE_SRC_NONE) { continue; }
                for (b = a + 1; b < GE_ACT_MAX; b++) {
                    if (gePresetSource(p, a) == gePresetSource(p, b)) { clash++; }
                }
            }
            printf("#   preset %s\n", gePresetName(p));
            eq_int(clash, 0, "no two actions share a pad button");
        }
    }

    /* Every preset must bind the actions without which the game cannot be finished.
     * An unbound `use` is not a controls preference, it is a soft lock: every
     * objective in the game runs through that button. */
    {
        int p;
        for (p = 0; p < GE_PRESET_MAX; p++) {
            ok(gePresetSource(p, GE_ACT_FIRE)  != GE_SRC_NONE, "preset binds fire on the pad");
            ok(gePresetSource(p, GE_ACT_USE)   != GE_SRC_NONE, "preset binds use on the pad");
            ok(gePresetSource(p, GE_ACT_PAUSE) != GE_SRC_NONE, "preset binds pause on the pad");
            ok(gePresetKeys(p, GE_ACT_USE)[0]  != '\0',        "preset binds use on the keyboard");
            /* weapon_next drives the N64 A button, which is what confirms a front.c
             * menu item. A keyboard player with it unbound cannot start a mission. */
            ok(gePresetKeys(p, GE_ACT_WEAPON_NEXT)[0] != '\0',
               "preset binds weapon_next on the keyboard (menu confirm)");
        }
    }

    /* Out-of-range indices fall back rather than reading off the end. */
    eq_int(gePresetSource(-1, GE_ACT_FIRE),      GE_SRC_RT,   "bad preset falls back to modern");
    eq_int(gePresetSource(GE_PRESET_MODERN, -1), GE_SRC_NONE, "bad action reads as unbound");
    eq_str(gePresetKeys(GE_PRESET_MODERN, GE_ACT_MAX), "",    "bad action has no keys");
    eq_str(gePresetAxisKeys(GE_PRESET_MODERN, GE_AXIS_MAX), "", "bad axis has no keys");

    /* The user-visible part of the modern keyboard layout. These are the exact
     * spellings the launcher writes and a hand-edited config may contain, so a change
     * to any of them is a change to what an existing config means. */
    /* The mouse buttons must be BOUND, in every preset.
     *
     * They used to be hard-wired onto the trigger fields in geMousePoll and were moved
     * into the binding table so they could be remapped -- at which point a preset that
     * forgets to mention them leaves left-click doing nothing at all. That is exactly
     * what happened while this change was being written, and it is invisible from any
     * test that only looks at keys. */
    {
        int p;
        for (p = 0; p < GE_PRESET_MAX; p++) {
            printf("#   preset %s\n", gePresetName(p));
            ok(strstr(gePresetKeys(p, GE_ACT_FIRE), "mouse1") != NULL,
               "left mouse button fires");
            ok(strstr(gePresetKeys(p, GE_ACT_AIM), "mouse2") != NULL,
               "right mouse button aims");
        }
    }

    /* The wheel changes weapon in the modern preset -- the feature this whole path
     * exists for. */
    ok(strstr(gePresetKeys(GE_PRESET_MODERN, GE_ACT_WEAPON_NEXT), "wheelup") != NULL,
       "wheel up is next weapon");
    ok(strstr(gePresetKeys(GE_PRESET_MODERN, GE_ACT_WEAPON_PREV), "wheeldown") != NULL,
       "wheel down is previous weapon");
    /* Q is free in modern because aim moved to the mouse, so it backs up the wheel. */
    ok(strstr(gePresetKeys(GE_PRESET_MODERN, GE_ACT_WEAPON_NEXT), "Q") != NULL,
       "Q also changes weapon in modern");
    ok(strstr(gePresetKeys(GE_PRESET_N64, GE_ACT_AIM), "Q") != NULL,
       "Q still aims in n64");

    eq_str(gePresetKeys(GE_PRESET_MODERN, GE_ACT_RELOAD), "R",           "modern reload=R");
    eq_str(gePresetKeys(GE_PRESET_MODERN, GE_ACT_CROUCH), "C,Left Ctrl", "modern crouch=C");
    eq_str(gePresetKeys(GE_PRESET_MODERN, GE_ACT_USE),    "E,F",         "modern use=E");
    eq_str(gePresetAxisKeys(GE_PRESET_MODERN, GE_AXIS_FORWARD), "W",     "modern forward=W");
}

/* ---- geSourceHeld / geActionHeld ----------------------------------------- */

static void test_source_held(void)
{
    struct GePadState st;

    printf("# geSourceHeld reads the right field\n");
    memset(&st, 0, sizeof st);

    eq_int(geSourceHeld(&st, GE_SRC_RT), 0, "nothing held on an idle pad");
    eq_int(geSourceHeld(NULL, GE_SRC_RT), 0, "NULL pad is not a crash");

    st.rtrigger = 1;  eq_int(geSourceHeld(&st, GE_SRC_RT),     1, "rt");
    st.ltrigger = 1;  eq_int(geSourceHeld(&st, GE_SRC_LT),     1, "lt");
    st.a = 1;         eq_int(geSourceHeld(&st, GE_SRC_A),      1, "a");
    st.x = 1;         eq_int(geSourceHeld(&st, GE_SRC_X),      1, "x");
    st.dleft = 1;     eq_int(geSourceHeld(&st, GE_SRC_DLEFT),  1, "dleft");
    st.rstickbtn = 1; eq_int(geSourceHeld(&st, GE_SRC_RSTICK), 1, "rstick click");
    st.lstickbtn = 1; eq_int(geSourceHeld(&st, GE_SRC_LSTICK), 1, "lstick click");

    eq_int(geSourceHeld(&st, GE_SRC_NONE), 0, "none is never held");
    eq_int(geSourceHeld(&st, GE_SRC_Y),    0, "an unpressed button stays unpressed");

    /* START accepts BACK. The N64 has no fifth face bit for Back to map to and every
     * front.c menu branch accepts START_BUTTON, so aliasing costs nothing and saves a
     * player whose pad labels the button the other way. */
    memset(&st, 0, sizeof st);
    st.back = 1;
    eq_int(geSourceHeld(&st, GE_SRC_START), 1, "back also satisfies start");
    eq_int(geSourceHeld(&st, GE_SRC_BACK),  1, "back satisfies back");
    st.back = 0; st.start = 1;
    eq_int(geSourceHeld(&st, GE_SRC_START), 1, "start satisfies start");
    eq_int(geSourceHeld(&st, GE_SRC_BACK),  0, "start does not satisfy back");
}

static void test_action_held(void)
{
    struct GePadState st;

    printf("# geActionHeld ORs the keyboard and the pad\n");
    memset(&st, 0, sizeof st);

    /* Under the default preset FIRE is on the right trigger. */
    eq_int(geActionHeld(&st, 0, GE_ACT_FIRE), 0, "fire idle");
    st.rtrigger = 1;
    eq_int(geActionHeld(&st, 0, GE_ACT_FIRE), 1, "fire from the pad");

    /* The whole point of the split: the keyboard names the action directly, so it does
     * not matter which pad button fire is bound to. Previously the keyboard set
     * `rtrigger` itself and rebinding fire elsewhere broke the key. */
    memset(&st, 0, sizeof st);
    st.act[GE_ACT_FIRE] = 1;
    eq_int(geActionHeld(&st, 0, GE_ACT_FIRE), 1, "fire from the keyboard");
    eq_int(st.rtrigger, 0, "the keyboard did not have to fake a trigger");

    /* An action with no pad binding at all is still reachable from the keyboard. Under
     * the modern preset weapon_prev is exactly that -- it is what the mouse wheel
     * binds to. */
    memset(&st, 0, sizeof st);
    st.act[GE_ACT_WEAPON_PREV] = 1;
    eq_int(geActionHeld(&st, 0, GE_ACT_WEAPON_PREV), 1, "unbound-on-pad action still works from a key");

    eq_int(geActionHeld(NULL, 0, GE_ACT_FIRE),        0, "NULL pad is not a crash");
    eq_int(geActionHeld(&st, 0, -1),                  0, "action below range");
    eq_int(geActionHeld(&st, 0, GE_ACT_MAX),          0, "action above range");
}

/* ---- crouch hold vs toggle, and the reload edge -------------------------- */

/* Drive one frame with a given set of raw actions held. */
static void frame(int crouch, int stand, int reload)
{
    struct GePadState st;
    memset(&st, 0, sizeof st);
    st.act[GE_ACT_CROUCH] = (unsigned char) (crouch != 0);
    st.act[GE_ACT_STAND]  = (unsigned char) (stand  != 0);
    st.act[GE_ACT_RELOAD] = (unsigned char) (reload != 0);
    geBindingsFrame(0, &st);
}

static void test_crouch_hold(void)
{
    printf("# crouch, hold mode\n");
    forget_bindings();

    frame(0, 0, 0);
    eq_int(geCrouchActive(0), 0, "idle");

    frame(1, 0, 0);
    eq_int(geCrouchActive(0), 1, "held down");
    frame(1, 0, 0);
    eq_int(geCrouchActive(0), 1, "still held");

    /* Releasing crouch must stand Bond up. This is the behaviour change: crouch was
     * momentary before, but nothing watched the release, so a player who tapped C
     * stayed squatting until they found the separate stand key. */
    frame(0, 0, 0);
    eq_int(geCrouchActive(0), 0, "released");
    eq_int(geStandActive(0),  1, "release pulses stand");

    /* The pulse is short and self-terminating. A permanently-true stand would cancel
     * the retail aim-stick crouch the instant the stick recentred, because bondview2.c
     * reads `if (crouchDown) ... else if (crouchUp)`. */
    frame(0, 0, 0);
    eq_int(geStandActive(0), 1, "pulse spans a second frame (render-only ticks)");
    frame(0, 0, 0);
    eq_int(geStandActive(0), 0, "pulse has ended");
    frame(0, 0, 0);
    eq_int(geStandActive(0), 0, "and stays ended");

    /* An explicit stand key is momentary and needs no edge. */
    frame(0, 1, 0);
    eq_int(geStandActive(0), 1, "explicit stand key");
    frame(0, 0, 0);
    eq_int(geStandActive(0), 0, "explicit stand released");
}

static void test_crouch_toggle(void)
{
    printf("# crouch, toggle mode\n");
    forget_bindings();

    frame(0, 0, 0);
    eq_int(geCrouchActive(0), 0, "idle");

    /* Latches on the press, not on the hold. */
    frame(1, 0, 0);
    eq_int(geCrouchActive(0), 1, "first press latches");
    frame(1, 0, 0);
    eq_int(geCrouchActive(0), 1, "still latched while held");
    frame(0, 0, 0);
    eq_int(geCrouchActive(0), 1, "still latched after release");
    frame(0, 0, 0);
    eq_int(geCrouchActive(0), 1, "and stays latched");
    eq_int(geStandActive(0),  0, "no stand pulse while latched");

    /* Second press unlatches and pulses stand. Asserting crouchDown continuously is
     * safe -- currentPlayerAdjustCrouchPos(-2) clamps at CROUCH_SQUAT -- but coming
     * back up needs crouchUp for at least one frame. */
    frame(1, 0, 0);
    eq_int(geCrouchActive(0), 0, "second press unlatches");
    eq_int(geStandActive(0),  1, "unlatch pulses stand");
    frame(1, 0, 0);
    eq_int(geCrouchActive(0), 0, "holding does not re-latch");
    frame(0, 0, 0);
    eq_int(geCrouchActive(0), 0, "released, still standing");
    eq_int(geStandActive(0),  0, "pulse finished");

    /* An explicit stand press clears the latch. Without this the two inputs disagree:
     * V stands Bond up while the latch still says crouched, and the next crouch press
     * toggles it OFF and leaves him standing. */
    frame(1, 0, 0);
    eq_int(geCrouchActive(0), 1, "latched again");
    frame(0, 1, 0);
    eq_int(geCrouchActive(0), 0, "stand key clears the latch");
    frame(0, 0, 0);
    frame(1, 0, 0);
    eq_int(geCrouchActive(0), 1, "next press crouches rather than un-crouching");
}

static void test_reload_edge(void)
{
    printf("# reload is one frame per press\n");
    forget_bindings();

    frame(0, 0, 0);
    eq_int(geReloadEdge(0), 0, "idle");

    frame(0, 0, 1);
    eq_int(geReloadEdge(0), 1, "press");
    /* A level would restart the reload animation every frame the key was down, which
     * spends the whole magazine's worth of animation going nowhere. */
    frame(0, 0, 1);
    eq_int(geReloadEdge(0), 0, "held is not a second reload");
    frame(0, 0, 1);
    eq_int(geReloadEdge(0), 0, "still held");
    frame(0, 0, 0);
    eq_int(geReloadEdge(0), 0, "release");
    frame(0, 0, 1);
    eq_int(geReloadEdge(0), 1, "next press reloads again");
}

static void test_per_player_isolation(void)
{
    struct GePadState st;

    printf("# per-player state does not bleed\n");
    forget_bindings();

    memset(&st, 0, sizeof st);
    st.act[GE_ACT_CROUCH] = 1;
    geBindingsFrame(1, &st);

    eq_int(geCrouchActive(1), 1, "player 2 crouched");
    eq_int(geCrouchActive(0), 0, "player 1 did not");
    eq_int(geCrouchActive(2), 0, "nor player 3");

    /* Out-of-range ports read as idle rather than faulting, the same rule the rest of
     * the input layer follows. */
    eq_int(geCrouchActive(-1),               0, "port below range");
    eq_int(geCrouchActive(GE_PORT_MAX_PADS), 0, "port above range");
    eq_int(geStandActive(GE_PORT_MAX_PADS),  0, "stand, port above range");
    eq_int(geReloadEdge(GE_PORT_MAX_PADS),   0, "reload, port above range");
    geBindingsFrame(GE_PORT_MAX_PADS, &st);   /* must not write past the array */
    geBindingsFrame(-1, &st);
    ok(1, "out-of-range frame updates are ignored");

    geBindingsReset();
    eq_int(geCrouchActive(1), 0, "reset clears every player");
}

/* ---- the binding table itself -------------------------------------------- */

static void test_bind_resolution(void)
{
    printf("# geBindSrc: per-player over global over preset\n");

    /* Resolved once and cached, so this process gets exactly one configuration. main()
     * re-executes itself to cover the others. */
    eq_int(geBindSrc(0, GE_ACT_FIRE), geParseSource(getenv("EXPECT_P1_FIRE"), GE_SRC_RT),
           "player 1 fire");
    eq_int(geBindSrc(1, GE_ACT_FIRE), geParseSource(getenv("EXPECT_P2_FIRE"), GE_SRC_RT),
           "player 2 fire");

    eq_int(geBindSrc(0, -1),                 GE_SRC_NONE, "action below range");
    eq_int(geBindSrc(0, GE_ACT_MAX),         GE_SRC_NONE, "action above range");
    /* An out-of-range player clamps to player 1 rather than reading off the end. */
    eq_int(geBindSrc(99, GE_ACT_FIRE), geBindSrc(0, GE_ACT_FIRE), "player above range clamps");
}

int main(int argc, char **argv)
{
    const char *phase = (argc > 1) ? argv[1] : "";

    /* geBindSrc(), geInputPreset(), geCrouchMode() and geAimMode() all cache on first
     * call, which is right at runtime and means one process can only observe one
     * configuration. Rather than adding reset hooks that exist solely for the test --
     * and that would then be a second code path nothing in the game uses -- each
     * configuration runs as a child process with its own environment. */
    if (phase[0] == '\0') {
        static const char *const phases[] = {
            "pure", "hold", "toggle", "bind-default", "bind-global", "bind-player", NULL
        };
        int i, rc = 0;

        for (i = 0; phases[i] != NULL; i++) {
            char cmd[1024];
            snprintf(cmd, sizeof cmd, "\"%s\" %s", argv[0], phases[i]);
            if (system(cmd) != 0) { rc = 1; }
        }
        return rc;
    }

    if (strcmp(phase, "pure") == 0) {
        /* Everything that does not depend on a cached setting. */
        test_names();
        test_parsing();
        test_presets();
        test_source_held();
        test_action_held();
        test_reload_edge();
        test_per_player_isolation();
    } else if (strcmp(phase, "hold") == 0) {
        setenv("GETV_CROUCH_MODE", "hold", 1);
        test_crouch_hold();
    } else if (strcmp(phase, "toggle") == 0) {
        setenv("GETV_CROUCH_MODE", "toggle", 1);
        test_crouch_toggle();
    } else if (strcmp(phase, "bind-default") == 0) {
        setenv("EXPECT_P1_FIRE", "rt", 1);
        setenv("EXPECT_P2_FIRE", "rt", 1);
        test_bind_resolution();
    } else if (strcmp(phase, "bind-global") == 0) {
        /* A global key still means "all four players", so nothing configured against
         * the pre-split-screen build changes meaning. */
        setenv("GETV_BIND_FIRE", "lt", 1);
        setenv("EXPECT_P1_FIRE", "lt", 1);
        setenv("EXPECT_P2_FIRE", "lt", 1);
        test_bind_resolution();
    } else if (strcmp(phase, "bind-player") == 0) {
        /* Split-screen is the whole reason bindings are per player: with one global
         * table, moving fire for a player on a Nintendo pad moved it for everyone. */
        setenv("GETV_BIND_FIRE",    "lt", 1);
        setenv("GETV_P2_BIND_FIRE", "rb", 1);
        setenv("EXPECT_P1_FIRE", "lt", 1);
        setenv("EXPECT_P2_FIRE", "rb", 1);
        test_bind_resolution();
    } else {
        printf("FAIL unknown phase \"%s\"\n", phase);
        return 1;
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
