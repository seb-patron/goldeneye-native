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
static void frame(int crouch, int reload)
{
    struct GePadState st;
    memset(&st, 0, sizeof st);
    st.act[GE_ACT_CROUCH] = (unsigned char) (crouch != 0);
    st.act[GE_ACT_RELOAD] = (unsigned char) (reload != 0);
    geBindingsFrame(0, &st);
}

static void test_crouch_hold(void)
{
    printf("# crouch, hold mode\n");
    forget_bindings();

    frame(0, 0);
    eq_int(geCrouchActive(0), 0, "idle");

    frame(1, 0);
    eq_int(geCrouchActive(0), 1, "held down");
    frame(1, 0);
    eq_int(geCrouchActive(0), 1, "still held");

    /* Releasing crouch must stand Bond up. There is no stand key to press: crouch was
     * momentary before and nothing watched the release, so a tap left you squatting
     * until you found a second key that most players never did. */
    frame(0, 0);
    eq_int(geCrouchActive(0), 0, "released");
    eq_int(geStandActive(0),  1, "release stands you up");

    /* The pulse is short and self-terminating. A permanently-true stand would cancel
     * the retail aim-stick crouch the instant the stick recentred, because bondview2.c
     * reads `if (crouchDown) ... else if (crouchUp)`. */
    frame(0, 0);
    eq_int(geStandActive(0), 1, "pulse spans a second frame (render-only ticks)");
    frame(0, 0);
    eq_int(geStandActive(0), 0, "pulse has ended");
    frame(0, 0);
    eq_int(geStandActive(0), 0, "and stays ended");
}

static void test_crouch_toggle(void)
{
    printf("# crouch, toggle mode -- the default\n");
    forget_bindings();

    frame(0, 0);
    eq_int(geCrouchActive(0), 0, "idle");

    /* Latches on the press, not on the hold. */
    frame(1, 0);
    eq_int(geCrouchActive(0), 1, "first press crouches");
    frame(1, 0);
    eq_int(geCrouchActive(0), 1, "still crouched while held");
    frame(0, 0);
    eq_int(geCrouchActive(0), 1, "still crouched after release");
    frame(0, 0);
    eq_int(geCrouchActive(0), 1, "and stays crouched");
    eq_int(geStandActive(0),  0, "no stand pulse while crouched");

    /* The whole point: the SAME key stands you back up. Asserting crouchDown
     * continuously is safe -- currentPlayerAdjustCrouchPos(-2) clamps at CROUCH_SQUAT --
     * but coming back up needs crouchUp for at least one frame. */
    frame(1, 0);
    eq_int(geCrouchActive(0), 0, "second press stands up");
    eq_int(geStandActive(0),  1, "and pulses stand");
    frame(1, 0);
    eq_int(geCrouchActive(0), 0, "holding does not re-crouch");
    frame(0, 0);
    eq_int(geCrouchActive(0), 0, "released, still standing");
    eq_int(geStandActive(0),  0, "pulse finished");

    /* And round again, so the toggle is not one-shot. */
    frame(1, 0);
    eq_int(geCrouchActive(0), 1, "third press crouches again");
    frame(0, 0);
    frame(1, 0);
    eq_int(geCrouchActive(0), 0, "fourth press stands again");
}

static void test_crouch_default_is_toggle(void)
{
    printf("# crouch defaults to toggle with nothing configured\n");
    forget_bindings();

    /* Run with GETV_CROUCH_MODE unset. Toggle is the default because it is the only
     * mode where pressing crouch again stands you up, and there is no second key. */
    eq_int(geCrouchMode(), GE_TOGGLE, "default crouch mode");

    frame(1, 0);
    eq_int(geCrouchActive(0), 1, "press crouches");
    frame(0, 0);
    eq_int(geCrouchActive(0), 1, "stays crouched on release");
    frame(1, 0);
    eq_int(geCrouchActive(0), 0, "press again stands up");
}

static void test_no_stand_action(void)
{
    printf("# there is no bindable stand action\n");
    {
        int a, found = 0;
        for (a = 0; a < GE_ACT_MAX; a++) {
            if (strcmp(geActionName(a), "stand") == 0) { found = 1; }
        }
        /* A stand key does nothing except while already crouched, so nobody finds it
         * and everybody reports the crouch as broken instead. Crouch toggling is the
         * replacement, and this pins that the key does not come back by accident. */
        eq_int(found, 0, "no action is named \"stand\"");
    }
}

static void test_reload_edge(void)
{
    printf("# reload is one frame per press\n");
    forget_bindings();

    frame(0, 0);
    eq_int(geReloadEdge(0), 0, "idle");

    frame(0, 1);
    eq_int(geReloadEdge(0), 1, "press");
    /* A level would restart the reload animation every frame the key was down, which
     * spends the whole magazine's worth of animation going nowhere. */
    frame(0, 1);
    eq_int(geReloadEdge(0), 0, "held is not a second reload");
    frame(0, 1);
    eq_int(geReloadEdge(0), 0, "still held");
    frame(0, 0);
    eq_int(geReloadEdge(0), 0, "release");
    frame(0, 1);
    eq_int(geReloadEdge(0), 1, "next press reloads again");
}

/* ---- does USE still reload? ---------------------------------------------- */

static void test_use_also_reloads(void)
{
    printf("# use stops doubling as reload once reload is bound\n");
    /* Cached on first call, so main() runs each configuration in its own process. */
    eq_int(geUseAlsoReloads(), atoi(getenv("EXPECT_USE_RELOADS")),
           "use doubles as reload");
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

static void test_bind_migration(void)
{
    int a, b, shared = 0;

    printf("# a pre-remap config does not put two actions on one pad button\n");
    /* The exact lines every config written by the old template contains. Under the
     * modern preset, crouch defaults to b -- the button this config explicitly gave to
     * use -- which is how pressing B to open a door also crouched on the first launch. */
    eq_int(geBindSrc(0, GE_ACT_USE),         GE_SRC_B,    "explicit use=b is kept");
    eq_int(geBindSrc(0, GE_ACT_WEAPON_NEXT), GE_SRC_A,    "explicit weapon_next=a is kept");
    eq_int(geBindSrc(0, GE_ACT_CROUCH),      GE_SRC_NONE, "preset crouch yields its b to use");
    eq_int(geBindSrc(0, GE_ACT_RELOAD),      GE_SRC_X,    "a preset default with no clash survives");

    for (a = 0; a < GE_ACT_MAX; a++) {
        for (b = a + 1; b < GE_ACT_MAX; b++) {
            if (geBindSrc(0, a) != GE_SRC_NONE && geBindSrc(0, a) == geBindSrc(0, b)) { shared++; }
        }
    }
    eq_int(shared, 0, "no two actions share a pad button");
}

static void test_bind_explicit_clash(void)
{
    printf("# two actions the player bound to one button are both kept\n");
    /* A deliberate choice, not a migration accident -- only preset defaults yield. */
    eq_int(geBindSrc(0, GE_ACT_USE),    GE_SRC_B, "explicit use=b");
    eq_int(geBindSrc(0, GE_ACT_CROUCH), GE_SRC_B, "explicit crouch=b is not dropped");
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
            "pure", "hold", "toggle", "crouch-default",
            "use-reload-modern", "use-reload-n64", "use-reload-unbound",
            "use-reload-forced",
            "bind-default", "bind-global", "bind-player",
            "bind-migration", "bind-explicit-clash", NULL
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
        test_no_stand_action();
        test_per_player_isolation();
    } else if (strcmp(phase, "hold") == 0) {
        setenv("GETV_CROUCH_MODE", "hold", 1);
        test_crouch_hold();
    } else if (strcmp(phase, "toggle") == 0) {
        setenv("GETV_CROUCH_MODE", "toggle", 1);
        test_crouch_toggle();
    } else if (strcmp(phase, "crouch-default") == 0) {
        unsetenv("GETV_CROUCH_MODE");
        test_crouch_default_is_toggle();
    } else if (strcmp(phase, "use-reload-modern") == 0) {
        /* Modern binds reload to R and to the west face button, so the use button
         * should stop reloading -- otherwise the same key opens a door or reloads
         * depending on where you stand, which is what a dedicated key replaces. */
        setenv("GETV_INPUT_PRESET", "modern", 1);
        setenv("EXPECT_USE_RELOADS", "0", 1);
        test_use_also_reloads();
    } else if (strcmp(phase, "use-reload-n64") == 0) {
        /* n64 leaves reload unbound, so retail behaviour must survive: use with
         * nothing in reach is the ONLY way to reload there. */
        setenv("GETV_INPUT_PRESET", "n64", 1);
        setenv("EXPECT_USE_RELOADS", "1", 1);
        test_use_also_reloads();
    } else if (strcmp(phase, "use-reload-unbound") == 0) {
        /* Explicitly unbinding reload under modern must also restore it. */
        setenv("GETV_INPUT_PRESET", "modern", 1);
        setenv("GETV_KEY_RELOAD", "none", 1);
        setenv("GETV_BIND_RELOAD", "none", 1);
        setenv("EXPECT_USE_RELOADS", "1", 1);
        test_use_also_reloads();
    } else if (strcmp(phase, "use-reload-forced") == 0) {
        /* GETV_USE_RELOADS overrides the inference either way. */
        setenv("GETV_INPUT_PRESET", "modern", 1);
        setenv("GETV_USE_RELOADS", "1", 1);
        setenv("EXPECT_USE_RELOADS", "1", 1);
        test_use_also_reloads();
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
    } else if (strcmp(phase, "bind-migration") == 0) {
        setenv("GETV_INPUT_PRESET", "modern", 1);
        setenv("GETV_BIND_FIRE", "rt", 1);
        setenv("GETV_BIND_AIM", "lt", 1);
        setenv("GETV_BIND_USE", "b", 1);
        setenv("GETV_BIND_WEAPON_NEXT", "a", 1);
        setenv("GETV_BIND_WEAPON_PREV", "none", 1);
        setenv("GETV_BIND_PAUSE", "start", 1);
        test_bind_migration();
    } else if (strcmp(phase, "bind-explicit-clash") == 0) {
        setenv("GETV_INPUT_PRESET", "modern", 1);
        setenv("GETV_BIND_USE", "b", 1);
        setenv("GETV_BIND_CROUCH", "b", 1);
        test_bind_explicit_clash();
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
