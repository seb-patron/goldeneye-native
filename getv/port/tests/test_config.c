/* Config key handling, without the game.
 *
 * This exists because the frame-rate key was got wrong three times in one day, and every
 * mistake was the kind a test catches in a second: a range that let through a value the
 * engine could not honour, a pairing that was set in one code path and not the other, and a
 * rejection message describing a limitation that had stopped being true. None of it needed a
 * window, a ROM or a level. It needed somebody to call apply() and look at the result.
 *
 * apply() is the whole surface: every key, from the file and from the command line, arrives
 * here as (key, value, overwrite) and the only observable effects are the environment
 * variables it sets and whether it counted an error. Both are readable from a test, so this
 * pins behaviour rather than implementation.
 *
 * The rule these tests encode, which is the one that keeps being broken: a setting the engine
 * cannot honour must be REFUSED, not accepted and quietly played wrong. Uncapped on the
 * synthetic clock ran the game thirteen times too fast and was reachable from the config.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The four externs ge_config.c expects from the rest of the port and the game. Filtering is
 * exercised below; the other three only have to exist for the unit to link. */
unsigned int  configFiltering = 2;
void set_debug_testingmanpos_flag(int flag) { (void) flag; }
unsigned char g_CheatPlayerTextRelated[256];
int           num_chars_selectable_mp = 8;

/* port_paths.c owns these. Only the config file's own load and save path touch them, and no
 * key tested here reads or writes a file, so returning failure is both sufficient and honest:
 * it is what a host with nowhere to put user data would report. */
int gePortUserDataDir(const char *org, const char *app, char *out, size_t outsz)
{ (void) org; (void) app; if (out && outsz) out[0] = '\0'; return 0; }
int gePortMakeDirTree(const char *path, unsigned mode)
{ (void) path; (void) mode; return -1; }

#include "ge_config.c"

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: got %d want %d\n", what, got, want);
        failures++;
    }
}

static void check_env(const char *what, const char *name, const char *want)
{
    const char *got = getenv(name);
    if (want == NULL) {
        if (got == NULL) { printf("  ok    %s\n", what); return; }
        printf("  FAIL  %s: %s is \"%s\", expected unset\n", what, name, got);
        failures++;
        return;
    }
    if (got != NULL && strcmp(got, want) == 0) { printf("  ok    %s\n", what); return; }
    printf("  FAIL  %s: %s is \"%s\" want \"%s\"\n", what, name, got ? got : "(unset)", want);
    failures++;
}

/* Every case starts from nothing set, because put() is setenv with overwrite disabled for
 * file values: a leftover from the previous case would silently make the next one pass. */
static void reset_preset(void)
{
    int i;
    for (i = 0; i < kPresetPlusCount; i++) {
        unsetenv(kPresetPlus[i].name);
        g_preset_env_had[i] = 0;
    }
    unsetenv("GETV_PRESET");
    unsetenv("GETV_PROFILE_PLUS");
    g_preset_plus = -1;
    g_errors = 0;
}

static void reset(void)
{
    unsetenv("GETV_FPS");
    unsetenv("GETV_REALCLOCK");
    unsetenv("GETV_TICKFIELDS");
    unsetenv("GETV_CROSSHAIR_COLOR");
    unsetenv("GETV_WIDESCREEN");
    unsetenv("GETV_WINDOW");
    unsetenv("GETV_SUPERSAMPLE");
    unsetenv("GETV_FILTERING");
    unsetenv("GETV_POINT_FILTER");
    configFiltering = 2;
    g_errors = 0;
}

/* apply() with overwrite on, which is what the command line does. The file path differs only
 * in that put() will not displace an existing value, and that is setenv's behaviour rather
 * than this file's. */
static int set(const char *key, const char *val) { return apply(key, val, 1); }

int main(void)
{
    printf("test_config\n");

    /* ---- framerate: the key that keeps going wrong ---------------------------------- */

    reset();
    check("framerate=60 accepted",        set("framerate", "60"), 1);
    check("framerate=60 no error",        g_errors, 0);
    check_env("framerate=60 sets the cap", "GETV_FPS", "60");
    check_env("framerate=60 leaves the clock alone", "GETV_REALCLOCK", NULL);

    reset();
    check("framerate=30 accepted",        set("framerate", "30"), 1);
    /* 30 without this reports one elapsed field per update and runs the world at half speed.
     * The pairing is the fix; losing it is a silent halving, not an error. */
    check_env("framerate=30 pairs TICKFIELDS", "GETV_TICKFIELDS", "2");

    reset();
    check("framerate=50 accepted",        set("framerate", "50"), 1);
    check("framerate=50 no error",        g_errors, 0);

    /* off has to bring the real clock with it. Uncapped on the synthetic counter is one
     * rendered frame per video field by construction, measured at 811.9 fields a second
     * against the correct 60. Accepting the cap without the clock is the worst state the
     * config can reach, so this pairing is the point of the key. */
    reset();
    check("framerate=off accepted",       set("framerate", "off"), 1);
    check_env("framerate=off uncaps",     "GETV_FPS", "0");
    check_env("framerate=off takes the real clock", "GETV_REALCLOCK", "1");

    reset();
    check("framerate=uncapped accepted",  set("framerate", "uncapped"), 1);
    check_env("uncapped is the same key", "GETV_FPS", "0");
    check_env("uncapped takes the clock too", "GETV_REALCLOCK", "1");

    /* A cap above 60 is wrong on the synthetic clock and ignored on the real one, because
     * waitForNextFrame only free-runs when the cap is off. There is no third case, so it is
     * refused rather than accepted. Leaving GETV_FPS unset matters as much as the error: a
     * rejected value must not half-apply. */
    reset();
    check("framerate=120 refused",        set("framerate", "120"), 1);
    check("framerate=120 counts an error", g_errors, 1);
    check_env("framerate=120 sets nothing", "GETV_FPS", NULL);

    reset();
    check("framerate=144 refused",        set("framerate", "144"), 1);
    check("framerate=144 counts an error", g_errors, 1);

    reset();
    check("framerate=45 refused",         set("framerate", "45"), 1);
    check("framerate=45 counts an error",  g_errors, 1);
    check_env("framerate=45 sets nothing", "GETV_FPS", NULL);

    reset();
    check("framerate=nonsense refused",   set("framerate", "banana"), 1);
    check("framerate=nonsense counts an error", g_errors, 1);

    /* ---- reticle scale ----------------------------------------------------------------
     *
     * Range-checked here as well as in port_support.c so a rejected value is reported where
     * the user can see it rather than accepted and then silently clamped back to 1.0 at load.
     * Same rule as crosshair_color below. */

    reset();
    unsetenv("GETV_CROSSHAIR_SCALE");
    check("crosshair_scale=0.6 accepted",  set("crosshair_scale", "0.6"), 1);
    check("crosshair_scale no error",      g_errors, 0);
    check_env("crosshair_scale set",       "GETV_CROSSHAIR_SCALE", "0.6");

    reset();
    unsetenv("GETV_CROSSHAIR_SCALE");
    check("reticle_scale is the same key", set("reticle_scale", "1.0"), 1);
    check_env("reticle_scale set",         "GETV_CROSSHAIR_SCALE", "1.0");

    /* Below a quarter the sight has too few texels left to read as a shape; above 2.0 it
     * stops being a sight and becomes an obstruction. Both ends refuse rather than clamp. */
    reset();
    unsetenv("GETV_CROSSHAIR_SCALE");
    check("crosshair_scale=0.1 refused",   set("crosshair_scale", "0.1"), 1);
    check("too small counts an error",     g_errors, 1);
    check_env("too small sets nothing",    "GETV_CROSSHAIR_SCALE", NULL);

    reset();
    unsetenv("GETV_CROSSHAIR_SCALE");
    check("crosshair_scale=5 refused",     set("crosshair_scale", "5"), 1);
    check("too large counts an error",     g_errors, 1);

    /* ---- crosshair colour ------------------------------------------------------------ */

    reset();
    check("crosshair_color accepted",     set("crosshair_color", "FF0000"), 1);
    check("crosshair_color no error",     g_errors, 0);
    check_env("crosshair_color set",      "GETV_CROSSHAIR_COLOR", "FF0000");

    /* Validated here as well as in port_support.c, so a typo is reported where the user can
     * see it rather than silently falling back to white at load. */
    reset();
    check("crosshair_color short refused", set("crosshair_color", "F00"), 1);
    check("short counts an error",         g_errors, 1);
    check_env("short sets nothing",        "GETV_CROSSHAIR_COLOR", NULL);

    reset();
    check("crosshair_color non-hex refused", set("crosshair_color", "GGHHII"), 1);
    check("non-hex counts an error",         g_errors, 1);

    /* ---- a few neighbours, so a change here does not go unnoticed --------------------- */

    reset();
    check("widescreen=0 accepted",        set("widescreen", "0"), 1);
    check_env("widescreen=0 set",         "GETV_WIDESCREEN", "0");

    reset();
    check("widescreen=on accepted",       set("widescreen", "on"), 1);
    check_env("widescreen=on is 1",       "GETV_WIDESCREEN", "1");

    reset();
    check("resolution accepted",          set("resolution", "1600x900"), 1);
    check_env("resolution sets the window", "GETV_WINDOW", "1600x900");

    /* Below the floor the renderer supports. Rejected rather than clamped, so the user is
     * told rather than quietly given something else. */
    reset();
    check("resolution too small refused", set("resolution", "160x120"), 1);
    check("too small counts an error",    g_errors, 1);
    check_env("too small sets nothing",   "GETV_WINDOW", NULL);

    reset();
    check("supersample=2 accepted",       set("supersample", "2"), 1);
    check_env("supersample=2 set",        "GETV_SUPERSAMPLE", "2");

    reset();
    check("supersample=3 refused",        set("supersample", "3"), 1);
    check("supersample=3 counts an error", g_errors, 1);

    /* Filtering has one consumer that cannot use getenv(): configFiltering is read directly
     * by Fast3D. The shell environment is therefore loaded into that global by a constructor
     * before main(), and the file pass must not overwrite it by assigning the global directly.
     * This used to make the one key resolve as CLI > file > environment while every other key
     * followed the documented CLI > environment > file order. */
    reset();
    setenv("GETV_FILTERING", "0", 1);
    configFiltering = 0;   /* the constructor's result before geConfigInit() */
    check("filtering file value accepted", apply("filtering", "three-point", 0), 1);
    check("environment filtering beats the file", (int)configFiltering, 0);
    check_env("environment filtering remains canonical", "GETV_FILTERING", "0");
    check_env("point companion follows effective environment", "GETV_POINT_FILTER", "1");

    check("filtering CLI value accepted", set("filtering", "bilinear"), 1);
    check("CLI filtering beats the environment", (int)configFiltering, 1);
    check_env("CLI filtering becomes canonical", "GETV_FILTERING", "1");
    check_env("bilinear clears the point companion", "GETV_POINT_FILTER", "0");

    reset();
    check("filtering file value accepted without environment",
          apply("filtering", "three-point", 0), 1);
    check("file filtering reaches the global", (int)configFiltering, 2);
    check_env("file filtering is visible to the launcher", "GETV_FILTERING", "2");
    check_env("three-point clears the point companion", "GETV_POINT_FILTER", "0");

    /* An unknown key is reported rather than ignored: a typo in a config file that silently
     * does nothing is indistinguishable from a setting that does not work. */
    reset();
    check("unknown key not claimed",      set("no_such_key", "1"), 0);

    /* ---- the GoldenEye+ preset -------------------------------------------------------
     *
     * This key used to accept `enhanced` and then print that no enhancement was implemented,
     * while the launcher had a working profile of the same name under a different variable.
     * The two are one thing now, and these pin the part that is easy to get subtly wrong: not
     * what the preset turns on, but what it is allowed to overwrite. */

    reset_preset();
    check("preset=plus claimed",      set("preset", "plus"), 1);
    check("preset=plus no error",     g_errors, 0);
    check("preset=plus recorded",     g_preset_plus, 1);
    /* Nothing is applied at parse time. Doing it here would tie the result to where the line
     * sits in the file. */
    check_env("preset does not act at parse time", "GETV_FPS", NULL);

    reset_preset();
    check("preset=goldeneye+ is the same", set("preset", "goldeneye+"), 1);
    check("goldeneye+ recorded",      g_preset_plus, 1);

    reset_preset();
    check("preset=faithful claimed",  set("preset", "faithful"), 1);
    check("faithful recorded",        g_preset_plus, 0);

    reset_preset();
    check("preset=nonsense refused",  set("preset", "banana"), 1);
    check("nonsense counts an error", g_errors, 1);

    /* The rule the whole thing exists for: the preset FILLS GAPS and displaces nothing.
     *
     * Somebody who writes `preset = plus` and then `fxaa = 0` means both lines, and handing
     * them FXAA anyway would be the config layer overruling them. The first version of this
     * had the preset win instead, which read as reasonable until you wrote the two lines out
     * and saw what it did.
     *
     * The gap it leaves is real and is handled by reporting rather than by overriding: the
     * template now ships `supersample` and `framerate` commented out so the profile can reach
     * them, and an older config with those as live lines gets told which settings it is
     * holding back. */
    reset_preset();
    (void) set("supersample", "1");                 /* stand in for the file's own line */
    (void) set("framerate", "60");
    check_env("file value is in place first", "GETV_SUPERSAMPLE", "1");
    (void) set("preset", "plus");
    ge_preset_apply();
    check_env("the file's supersample survives the preset", "GETV_SUPERSAMPLE", "1");
    check_env("the file's framerate survives it too",       "GETV_FPS", "60");
    /* And everything the file did NOT mention still arrives. A preset that backed off
     * entirely because one key was set would be worse than no preset. */
    check_env("but the rest of the profile still applies",  "GETV_REALCLOCK", "1");
    check_env("HD textures on",                          "GETV_HD_TEXTURES", "1");
    check_env("FXAA on",                                 "GETV_FXAA", "1");
    check_env("MSAA on",                                 "GETV_MSAA", "4");
    check_env("anisotropic on",                          "GETV_ANISO", "8");
    check_env("mipmaps on",                              "GETV_MIPMAPS", "1");
    check_env("parallax on",                             "GETV_PARALLAX", "1");
    check_env("and a smaller reticle",                   "GETV_CROSSHAIR_SCALE", "0.6");

    /* And the other half of that rule, which matters more. The launcher writes every setting
     * explicitly into the environment and then execs, so a preset that overwrote the
     * environment would discard the choices the player just made in the UI. */
    reset_preset();
    setenv("GETV_SUPERSAMPLE", "1", 1);
    setenv("GETV_FXAA", "0", 1);
    ge_preset_snapshot();                            /* what geConfigInit does at entry */
    (void) set("preset", "plus");
    ge_preset_apply();
    check_env("environment keeps its supersample", "GETV_SUPERSAMPLE", "1");
    check_env("environment keeps its FXAA",        "GETV_FXAA", "0");
    check_env("everything else still applies",     "GETV_MSAA", "4");

    /* GETV_PROFILE_PLUS is what the launcher writes. A run with that set and no preset line
     * anywhere should still come up as GoldenEye+. */
    reset_preset();
    setenv("GETV_PROFILE_PLUS", "1", 1);
    ge_preset_apply();
    check_env("the launcher's own flag selects the preset", "GETV_FXAA", "1");

    reset_preset();
    ge_preset_apply();
    check_env("no preset asked for, nothing applied", "GETV_FXAA", NULL);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
