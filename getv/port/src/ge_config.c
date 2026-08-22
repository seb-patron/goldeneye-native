/* ge_config.c — the user configuration layer.
 *
 * Why this exists, and why it looks like this
 * -------------------------------------------
 * This port already has around 100 `GETV_*` environment gates. They were never
 * designed as user settings; they are A/B development switches, and the project's
 * reproducibility rules (PORTING_PLAYBOOK.md §2.10-§2.12, every `level_sweep.sh` arm)
 * depend on them continuing to behave exactly as they do today. A public release still
 * needs a config file.
 *
 * The whole design is one line: setenv(key, value, OVERWRITE=0).
 *
 * POSIX setenv's third argument is "overwrite". Passing 0 means "set this only if it is * not already set". So:
 *
 * config file  -> setenv(..., 0) loses to anything already in the environment
 * CLI flag     -> setenv(..., 1) overwrites, so it beats the environment
 *
 * which is the required precedence, CLI > env > config file > default, with no changes
 * to any consumer. Every existing `getenv("GETV_...")` call site in port_render.c,
 * port_input.c, port_audio.c, port_save.c, port_support.c, gfx_sdl2.c, gfx_opengl.c,
 * gfx_pc.c and front.c keeps working unmodified. A harness that exports
 * GETV_EXIT_FRAME=61 gets 61 no matter what a user's goldeneye.cfg says.
 *
 * The consequence is that this file must run before anything reads a gate. It is called
 * from the first statement of main() in port/mac/ge_mac_main.c, before SDL_main(), and
 * therefore before gfx_init(), before osGetCount()'s first call, and before front.c
 * ever runs. That ordering is the only invariant here.
 *
 * Where the file is looked for (first hit wins, all others ignored)
 * -----------------------------------------------------------------
 *   1. $GETV_CONFIG                                    (explicit override)
 *   2. --config=<path>                                 (explicit override)
 *   3. <dir of argv[0]>/goldeneye.cfg                  (beside the binary)
 *   4. ~/Library/Application Support/GoldenEye/goldeneye.cfg   (macOS user config)
 *
 * argv[0] is used rather than _NSGetExecutablePath() deliberately: this file is globbed
 * into the tvOS port layer too (build_sim.sh / build.sh compile port/src/*.c), and
 * <mach-o/dyld.h> is a macOS-only header. argv[0] is portable C.
 *
 * File format
 * -----------
 *   # comment              ; also a comment
 * key = value            (whitespace around either side is trimmed)
 * GETV_ANYTHING = value  (raw escape hatch — sets that gate directly)
 *
 * No sections, no quoting, no line continuation. A config file that users hand-edit
 * should be hard to get subtly wrong; an unknown key is reported on stdout rather than
 * ignored.
 */

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ge_config.h"
/* The user-data directory and mkdir -p, factored out of the
 * two places below that open-coded "$HOME/Library/Application Support". The macOS
 * paths this file produces are unchanged, character for character. */
#include "port_paths.h"
int ge_config_loaded  = 0;
int ge_config_controls = -1;

/* port/configfile.h's globals. Declared by hand rather than #included: configfile.h
 * drags in the Fast3D config surface, and this is all we touch. It is defined in
 * port/src/port_support.c; this file only assigns to it. */
extern unsigned int configFiltering;   /* 0 = nearest, 1 = bilinear, 2 = three-point */

/* Rare's own leftover position readout. src/game/debugmenu_handler.c:1018 — a
 * three-line exported setter for `g_DebugManPos` (a plain s32 in BSS at :266),
 * whose only other writers are the debug menu's own toggle (:628) and this setter.
 * Setting it once here therefore sticks. bondview2.c:10367 then draws room id,
 * collision X/Y/Z and a compass heading every frame.
 *
 * This works in a stock build: the flag is gated on LEFTOVERDEBUG, which
 * build_mac.sh:87 already defines unconditionally. It does not need DEBUGMENU and
 * therefore does not repurpose START or change codegen. */
extern void set_debug_testingmanpos_flag(int flag);

/* ------------------------------------------------------------------------- *
 * Named cheats — the game's own cheat system, exposed by name.
 *
 * These are not GameShark codes, and the difference matters.
 *
 * A GameShark code is a raw N64 RDRAM address. This port has no RDRAM; it has native
 * pointers at ASLR'd locations, so the roughly 1,900 published GoldenEye codes cannot be
 * applied here the way a recompilation applies them. Mapping them back to symbols
 * resolves only 1.3% against this decomp (there is no .map file, and ge007.ld fixes only
 * three addresses), so the address route is a dead end.
 *
 * The well-known codes do not need it. Twenty-four of them cluster one byte apart
 * starting at 0x80069652, and
 *
 * gameshark_address - 0x80069650  == the CHEAT_ID enum ordinal
 *
 * exactly, gaps included: 0x80069650 is the retail base of `g_CheatPlayerTextRelated[]`
 * (src/game/cheat.c:26). This checks out against src/bondconstants.h:1249 on nine values
 * — Invincibility=2, AllGuns=3, LineMode=7, 2xHealth=8, Invisibility=0xA,
 * InfiniteAmmo=0xB, DKMode=0xC, TinyBond=0xE, Paintball=0xF — and every skipped address
 * lands on an enum member the published code lists do not name (CHEAT_MAXAMMO,
 * CHEAT_DEBUG_UNK5, CHEAT_DEACTIVATE_INVINCIBILITY, CHEAT_2X_ARMOR,
 * CHEAT_EXTRA_WEAPONS), which a coincidence would not reproduce.
 *
 * So the useful cheats on that list are the game's own cheat flags, and they can be set
 * by name. That is layout-independent, ASLR-proof, survives every relink and recompile,
 * and stays correct under mods that move the array — none of which an address list can
 * do.
 *
 * Why a direct array write and not cheatButtonTurnOnCheatForPlayers(): that function
 * (cheat.c:952) reads g_CheatInfo, calls getPlayerCount() and set_cur_player(), and
 * dispatches a per-cheat switch. None of that is safe from main(), which runs before any
 * player exists. It is also unnecessary, because the consumers do not read a cached copy
 * — they call cheatIsActive() live, per use (explosion.c:2025 for paintball; chr.c:2188,
 * 2208, 2825 and chr_b.c:41 for DK mode). cheatIsActive() (cheat.c:1677) is nothing but
 * `(g_CheatPlayerTextRelated[cheat] >> get_cur_playernum()) & 1`. Setting the bits
 * directly at startup therefore reaches every one of those call sites, and for anything
 * spawned after startup it is more complete than the turn-on path, whose
 * cheatButtonSetDkMode() only rescales guards that already exist.
 *
 * What this does not do. A flag write alone is not enough for most cheats: `line_mode`
 * set this way produces a frame byte-identical to baseline. The rule comes from
 * enumerating every live consumer in the tree — `grep -rE "cheatIsActive\(CHEAT_" src` —
 * which returns exactly five:
 *
 * CHEAT_DK_MODE chr.c:2188,2208,2825 chr_b.c:41,43
 * CHEAT_INFINITE_AMMO lv.c  (x2)
 * CHEAT_PAINTBALL explosion.c:2025
 * CHEAT_NO_RADAR_MP radar.c
 * CHEAT_ENEMY_ROCKETS prop.c
 *
 * (CHEAT_MARQUIS and CHEAT_ENEMYSHIELDS also appear in that grep. Both are inside
 * commented-out Perfect Dark leftovers in chrai.c:2981 and :3517, and neither exists in
 * the CHEAT_IDS enum at all. They are not cheats and are not exposed.)
 *
 * Those five, plus CHEAT_EXTRA_MP_CHARS (whose entire switch arm is one assignment this
 * file can make itself), are marked `live = 1` and take effect from the config file
 * immediately. Every other cheat's effect lives in the turn-on switch
 * (cheat.c:1084-1445) — granting weapons, multiplying health — which needs a player
 * context that does not exist at main() time. For those, the flag is set, which is real
 * and which the game's own UI honours, and the log says so. Nothing is silently
 * half-applied.
 *
 * cheatDisableAllCheats() (cheat.c:1625) is called from lvlUnloadStageTextData()
 * (lv.c:1745), i.e. on stage unload, and clears every cheat carrying CHEAT_MASK_TOGGLE.
 * Config cheats therefore apply to the session you boot into and are cleared when you
 * leave the stage. That is the retail lifetime, not a bug.
 *
 * The ordinals below are transcribed from src/bondconstants.h:1249-1284, which is the
 * only source of truth. They are hard-coded rather than #included because this file is
 * port-layer code and must not pull a game header (and therefore <ultra64.h>) into the
 * port build. If that enum gains or loses a member, every ordinal after it shifts and
 * this table silently applies the wrong cheat. Re-check the table against the enum after
 * any decomp bump. */

extern unsigned char g_CheatPlayerTextRelated[];  /* cheat.c:26, u8[CHEAT_INVALID+1] */
extern int num_chars_selectable_mp;               /* front.c:573, s32, initialised to 8 */

#define GE_CHEAT_MAX_ID 34   /* CHEAT_2X_LASER — the last gameplay cheat we expose */

static const struct { const char *name; unsigned char id; unsigned char live; }
GE_CHEATS[] = {
    /* name id live = has a real cheatIsActive() consumer, so a
 flag write alone is enough (see above) */
    { "extra_mp_chars",          1, 1 },   /* handled specially -> roster, below */
    { "invincibility",           2, 0 },
    { "all_guns",                3, 0 },
    { "max_ammo",                4, 0 },
    { "line_mode",               7, 0 },   /* no live consumer; front.c:1015 only */
    { "2x_health",               8, 0 },
    { "2x_armor",                9, 0 },
    { "invisibility",           10, 0 },
    { "infinite_ammo",          11, 1 },   /* lv.c */
    { "dk_mode",                12, 1 },   /* chr.c, chr_b.c */
    { "extra_weapons",          13, 0 },
    { "tiny_bond",              14, 0 },
    { "paintball",              15, 1 },   /* explosion.c:2025 */
    { "10x_health",             16, 0 },
    { "magnum",                 17, 0 },
    { "laser",                  18, 0 },
    { "golden_gun",             19, 0 },
    { "silver_pp7",             20, 0 },
    { "gold_pp7",               21, 0 },
    { "bond_phase",             22, 0 },
    { "no_radar",               23, 1 },   /* radar.c */
    { "turbo_mode",             24, 0 },
    { "debug_pos",              25, 0 },
    { "fast_animation",         26, 0 },
    { "slow_animation",         27, 0 },
    { "enemy_rockets",          28, 1 },   /* prop.c */
    { "2x_rocket_launcher",     29, 0 },
    { "2x_grenade_launcher",    30, 0 },
    { "2x_rcp90",               31, 0 },
    { "2x_throwing_knife",      32, 0 },
    { "2x_hunting_knife",       33, 0 },
    { "2x_laser",               34, 0 },
};
#define GE_CHEAT_COUNT ((int)(sizeof GE_CHEATS / sizeof GE_CHEATS[0]))

/* All four player bits, mirroring what cheatButtonHandleCheatsTurnedOn() writes for a
 * CHEAT_MASK_GLOBAL cheat: `(1 << player_count) - 1`. Using 0xF makes the cheat active
 * whatever get_cur_playernum() turns out to be, in SP and in 1-4P MP alike. */
#define GE_CHEAT_ALL_PLAYERS 0x0F


#define GE_CFG_BASENAME "goldeneye.cfg"
static int g_errors = 0;
static char g_cfgpath[1024] = "";

/* ------------------------------------------------------------------ helpers */

static void ge_err(const char *fmt, const char *a, const char *b)
{
 printf("[getv][config] ERROR: ");
 printf(fmt, a, b);
 printf("\n");
 g_errors++;
}

static char *trim(char *s)
{
 char *e;
 while (*s != '\0' && isspace((unsigned char)*s)) { s++; }
 e = s + strlen(s);
 while (e > s && isspace((unsigned char)e[-1])) { e--; }
    *e = '\0';
 return s;
}

static void lower(char *s)
{
 for (; *s != '\0'; s++) { *s = (char)tolower((unsigned char)*s); }
}

/* The single choke point. `over` is setenv's overwrite flag and is the only thing that
 * distinguishes a CLI source from a file source. */
static void put(const char *name, const char *value, int over)
{
 setenv(name, value, over);
}

static int is_true(const char *v)
{
 return (strcmp(v, "1") == 0 || strcmp(v, "on") == 0 ||
 strcmp(v, "true") == 0 || strcmp(v, "yes") == 0);
}

static int is_false(const char *v)
{
 return (strcmp(v, "0") == 0 || strcmp(v, "off") == 0 ||
 strcmp(v, "false") == 0 || strcmp(v, "no") == 0);
}

/* ------------------------------------------------------- the day-0 key table */

/* Every key here is a friendly name for an existing gate, never a new mechanism.
 * Anything that cannot be expressed as "set this GETV_* variable" does not belong in
 * this table. */

static void key_resolution(const char *v, int over)
{
 unsigned w = 0, h = 0;
    /* Mirrors port_support.c:79's own parse and its own 320x240 floor exactly, so a
     * value this layer accepts is a value that layer will honour. Accepting
     * something it silently drops would be worse than rejecting it here. */
 if (strcmp(v, "fullscreen") == 0 || strcmp(v, "native") == 0) {
 put("GETV_FULLSCREEN", "1", over);
 return;
    }
 if (sscanf(v, "%ux%u", &w, &h) != 2 || w < 320 || h < 240) {
 ge_err("resolution=\"%s\" is not WIDTHxHEIGHT with width>=320 and height>=240 ""(e.g. 1280x960, 1920x1080, or \"fullscreen\")%s", v, "");
 return;
    }
    {
 char buf[64];
 snprintf(buf, sizeof buf, "%ux%u", w, h);
 put("GETV_WINDOW", buf, over);
    }
}

static void key_aspect(const char *v, int over)
{
    /* gfx_pc.c:1468 derives the aspect from the actual framebuffer dimensions
     * (`gfx_adjust_x_for_aspect_ratio`), so the renderer needs no aspect setting — it
     * needs a correctly-shaped window. This key therefore picks a default window shape
     * and validates any explicit `resolution` against it. It does not introduce a
     * second, independent aspect control; two of those would aspect-correct twice,
     * which gfx_pc.c:3478 warns about explicitly. */
 const char *win = getenv("GETV_WINDOW");
 int aw = 0, ah = 0;

 if (strcmp(v, "4:3") == 0 || strcmp(v, "43") == 0)        { aw = 4; ah = 3;  }
 else if (strcmp(v, "16:9") == 0 || strcmp(v, "169") == 0) { aw = 16; ah = 9;  }
 else if (strcmp(v, "auto") == 0)                           { return; }
 else {
 ge_err("aspect=\"%s\" — only 4:3, 16:9 and auto are supported%s", v, "");
 return;
    }
 put("GETV_ASPECT", (aw == 4) ? "4:3" : "16:9", over);

 if (win != NULL && *win != '\0') {
 unsigned w = 0, h = 0;
 if (sscanf(win, "%ux%u", &w, &h) == 2 && h != 0) {
 double want = (double)aw / (double)ah;
 double got  = (double)w / (double)h;
 if (got < want * 0.97 || got > want * 1.03) {
 printf("[getv][config] note: resolution %ux%u is not %d:%d ""(%.3f vs %.3f). The renderer follows the WINDOW, so the ""window shape wins and `aspect` is advisory here.\n",
 w, h, aw, ah, got, want);
            }
        }
 return;   /* an explicit resolution always wins over an implied one */
    }
    /* No explicit resolution: give the aspect a sane default window.
     * 4:3 = 1280x960 is port_support.c's own declared default. */
 put("GETV_WINDOW", (aw == 4) ? "1280x960" : "1600x900", over);
}

static void key_framerate(const char *v, int over)
{
 int n;

 if (is_false(v) || strcmp(v, "uncapped") == 0 || strcmp(v, "unlimited") == 0) {
 put("GETV_FPS", "0", over);
 return;
    }
 n = atoi(v);

    /* Rates above 60 are rejected on arithmetic grounds, not out of conservatism.
     *
     * GoldenEye's timestep is whole video frames, not seconds. gfx_sdl2.c:60-80 and
     * port_os.c:175-200 spell it out: osGetCount() advances a synthetic +1000 per call
     * so that `(delta + 387937) / 775875` is exactly 1 every frame, and that 1 is
     * `speedgraphframes`, which feeds g_ClockTimer (lv.c:1040-1047) and every
     * `for (i = 0; i < speedgraphframes; i++)` integrator in the game. Fire rates,
     * reload times, animation, physics and the mission clock are all counted in game
     * frames. Render twice as often and the world runs twice as fast: the RC-P90 fires
     * at 2x, Bond walks at 2x, the level timer runs at 2x.
     *
     * Nothing is clamped and nothing complains; it simply plays wrong, which is a poor
     * failure mode for a user-facing setting. Real 120 fps requires decoupling the
     * simulation tick from the render tick (fixed 60 Hz simulation, interpolated
     * presentation), which is a change to the pacing code, not a config value.
     *
     * This rejection is in the config layer only. `GETV_FPS=120` in the environment
     * still reaches gfx_sdl2.c:240 untouched, so a diagnostic run can still ask for
     * wrong-but-informative behaviour. */
 if (n > 60) {
 ge_err("framerate=%s is NOT SUPPORTED and has been ignored.\n""GoldenEye's timestep is whole video frames, so running ""above 60 does not\n""make the game smoother — it makes the game FASTER. ""Fire rates, physics,\n""animation and the mission clock all scale with the ""render rate. Real high-\n""refresh support needs simulation/render decoupling, ""which this build does\n""not have. Supported values: 30, 50, 60, or off%s", v, "");
 return;
    }
 if (n != 30 && n != 50 && n != 60) {
 ge_err("framerate=%s is not one of 30, 50, 60 or off%s", v, "");
 return;
    }
    /* 30 is accepted but is not free either: it halves game speed by the same argument
     * as above. It is offered because a 30 fps cap is the honest way to run this
     * renderer on a slow GPU, and the slowdown is at least uniform. */
 if (n == 30) {
 printf("[getv][config] note: framerate=30 also runs the SIMULATION at 30 — ""the game will play at half speed. This is inherent to GE's ""frame-counted timestep, not a bug.\n");
    }
    {
 char buf[16];
 snprintf(buf, sizeof buf, "%d", n);
 put("GETV_FPS", buf, over);
    }
}

static void key_supersample(const char *v, int over)
{
 int n = atoi(v);
 if (n != 1 && n != 2) {
 ge_err("supersample=%s — only 1 and 2 are supported%s", v, "");
 return;
    }
    /* Not a neutral speed knob: it changes the framebuffer size and therefore the heap
     * layout, so two runs at different settings are not directly comparable. For a
     * player it is simply anti-aliasing against speed. */
 put("GETV_SUPERSAMPLE", (n == 2) ? "2" : "1", over);
}

static void key_filtering(const char *v, int over)
{
    /* Two independent mechanisms exist and they do not mean the same thing:
     * configFiltering (port_support.c:107)  0=nearest 1=bilinear 2=three-point,
     * read by gfx_opengl.c:391 / gfx_pc.c:1966
     * GETV_POINT_FILTER (gfx_pc.c:1963) forces the literal N64 point-sample
     * We set both consistently so they cannot disagree. */
 if (strcmp(v, "point") == 0 || strcmp(v, "nearest") == 0 || strcmp(v, "n64") == 0) {
 configFiltering = 0;
 put("GETV_POINT_FILTER", "1", over);
    } else if (strcmp(v, "bilinear") == 0 || strcmp(v, "linear") == 0) {
 configFiltering = 1;
 put("GETV_POINT_FILTER", "0", over);
    } else if (strcmp(v, "three-point") == 0 || strcmp(v, "threepoint") == 0 ||
 strcmp(v, "3point") == 0 || strcmp(v, "default") == 0) {
 configFiltering = 2;   /* what the N64 RDP actually did */
 put("GETV_POINT_FILTER", "0", over);
    } else {
 ge_err("filtering=\"%s\" — expected point, bilinear or three-point%s", v, "");
    }
}

/* ---- gamepad profile / bindings / deadzone / invert-look ------------------------
 *
 * These nine keys are this file's own names for gates that already exist:
 * `GETV_GAMEPAD`, the six `GETV_BIND_*` action sources (port_os.c:458-471),
 * `GETV_DEADZONE` (port_os.c:673-694) and `GETV_INVERTLOOK`
 * (vendor/ge-decomp/src/game/file2.c:1413). Same choke point as every other key here:
 * parse, validate, put(). No consumer changes; port_os.c, port_input.c and file2.c are
 * untouched by this block. */

static void key_gamepad(const char *v, int over)
{
 if (strcmp(v, "auto") == 0 || strcmp(v, "xbox") == 0 ||
 strcmp(v, "playstation") == 0 || strcmp(v, "switch") == 0 ||
 strcmp(v, "generic") == 0) {
 put("GETV_GAMEPAD", v, over);
 return;
    }
 ge_err("gamepad=\"%s\" — expected auto, xbox, playstation, switch or generic%s",
 v, "");
}

/* Button names are positional, not label-based. "a" always means the bottom face
 * button on the player's pad, whatever it happens to be labelled: SDL maps the
 * physically-bottom face button to `_BUTTON_A` on every controller it knows, including
 * Nintendo's (where that same button is printed "B"). See the note next to
 * `geParseSrc()` in port_os.c. The `gamepad=` profile above only changes what gets
 * printed as an on-screen prompt glyph; it never changes what any binding below
 * actually does. */
static int is_bind_value(const char *v)
{
 return strcmp(v, "a") == 0 || strcmp(v, "b") == 0 || strcmp(v, "x") == 0 ||
 strcmp(v, "y") == 0 || strcmp(v, "lb") == 0 || strcmp(v, "rb") == 0 ||
 strcmp(v, "lt") == 0 || strcmp(v, "rt") == 0 ||
 strcmp(v, "start") == 0 || strcmp(v, "back") == 0 ||
 strcmp(v, "none") == 0;
}

static void key_bind(const char *gate, const char *key, const char *v, int over)
{
 if (is_bind_value(v)) { put(gate, v, over); return; }
 ge_err("%s=\"%s\" — expected a/b/x/y/lb/rb/lt/rt/start/back/none", key, v);
}

static void key_deadzone(const char *v, int over)
{
    /* Clamp rather than reject: port_os.c:683-685 clamps the same way, so a value
     * outside 0..40 still does something sane instead of erroring out over what is
     * only an out-of-range percentage. */
 int n = atoi(v);
 if (n < 0)  { n = 0; }
 if (n > 40) { n = 40; }
    {
 char buf[8];
 snprintf(buf, sizeof buf, "%d", n);
 put("GETV_DEADZONE", buf, over);
    }
}

static void key_invert_look(const char *v, int over)
{
    /* Absent must mean "no override", not "off". apply() is only ever called for a key
     * actually present in the file or on the CLI, so never writing an `invert_look`
     * line already leaves GETV_INVERTLOOK unset, and file2.c:1413 treats unset as "the     * save file's own Look Up/Down option wins", which is retail behaviour. Setting
     * invert_look=0 here is therefore an explicit override to non-inverted, not a
     * no-op. Do not fold this into key_bool_gate()'s pattern: that would make 0 and
     * unset indistinguishable and silently break the in-game watch option. */
 if (is_true(v))       { put("GETV_INVERTLOOK", "1", over); }
 else if (is_false(v)) { put("GETV_INVERTLOOK", "0", over); }
 else                  { ge_err("invert_look=\"%s\" — expected 0/1%s", v, ""); }
}

/* The 8 styles, in CONTROLLER_CONFIG_* order — src/bondconstants.h:1337-1364 and the
 * menu table at src/game/front.c:726-735, which also supplies the controller count. */
static const struct { const char *num; const char *name; int pads; } GE_CONTROL_STYLES[8] = {
    { "1.1", "honey",     1 },
    { "1.2", "solitaire", 1 },
    { "1.3", "kissy",     1 },
    { "1.4", "goodnight", 1 },
    { "2.1", "plenty",    2 },
    { "2.2", "galore",    2 },   /* true dual-analog: move on pad 2, look on pad 1 */
    { "2.3", "domino",    2 },
    { "2.4", "goodhead",  2 },   /* true dual-analog */
};

static void key_controls(const char *v, int over)
{
 int i;
 for (i = 0; i < 8; i++) {
 if (strcmp(v, GE_CONTROL_STYLES[i].num) == 0 ||
 strcmp(v, GE_CONTROL_STYLES[i].name) == 0) {
 char buf[8];
 snprintf(buf, sizeof buf, "%d", i);
 put("GETV_CONTROLS", buf, over);
 ge_config_controls = i;
 if (GE_CONTROL_STYLES[i].pads == 2) {
 printf("[getv][config] controls=%s %s is a TWO-CONTROLLER style ""(front.c:726-735). On a single modern gamepad this is the ""dual-analog layout; with 3-4 players front.c:4733 forces ""everyone back to 1.1 Honey.\n",
 GE_CONTROL_STYLES[i].num, GE_CONTROL_STYLES[i].name);
            }
 return;
        }
    }
 ge_err("controls=\"%s\" — expected one of 1.1/honey 1.2/solitaire 1.3/kissy ""1.4/goodnight 2.1/plenty 2.2/galore 2.3/domino 2.4/goodhead%s", v, "");
}

static void key_cheats(const char *v, int over)
{
 char buf[1024];
 char *tok, *save;
 int applied = 0, deferred = 0;

    (void)over;   /* cheats accumulate; there is no "unset a cheat" to overwrite */
 snprintf(buf, sizeof buf, "%s", v);

 for (tok = strtok_r(buf, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
 char *n = trim(tok);
 int i, hit = -1;
 if (*n == '\0') { continue; }
 for (i = 0; i < GE_CHEAT_COUNT; i++) {
 if (strcmp(n, GE_CHEATS[i].name) == 0) { hit = i; break; }
        }
 if (hit < 0) {
 ge_err("cheats: unknown cheat \"%s\" — run --list-cheats for the full set%s",
 n, "");
 continue;
        }
 if (GE_CHEATS[hit].id > GE_CHEAT_MAX_ID) { continue; }   /* belt and braces */

 g_CheatPlayerTextRelated[GE_CHEATS[hit].id] = GE_CHEAT_ALL_PLAYERS;
 applied++;

 if (GE_CHEATS[hit].id == 1) {
            /* CHEAT_EXTRA_MP_CHARS. Its switch arm is one line — front.c:4428's
             * unlock_all_mp_chars(), which just sets num_chars_selectable_mp = 0x40 —
             * so we can do it here and skip the unsafe call entirely.
             * 0x40 is sticky: front.c:5327 re-derives the roster every frame on the
             * character-select screen but guards the whole block with
             * `if (num_chars_selectable_mp != 0x40)`, so 0x40 short-circuits it
             * permanently. 0x21 would not stick — see key_roster(). */
 num_chars_selectable_mp = 0x40;
        } else if (!GE_CHEATS[hit].live) {
 deferred++;
 printf("[getv][config] cheats: \"%s\" — flag SET, but this cheat has no live ""cheatIsActive() consumer: its effect is applied once inside the ""turn-on switch (cheat.c:1084-1445), which needs a player context ""that does not exist at startup. Toggle it in-game for the effect.\n", n);
        }
    }
 if (applied > 0) {
 printf("[getv][config] cheats: %d set by name, %d of which are flag-only and ""need in-game activation. ""These are GE's OWN cheat flags (g_CheatPlayerTextRelated, ""bondconstants.h:1249), not GameShark addresses.\n", applied, deferred);
    }
}

static void key_roster(const char *v, int over)
{
 int n = atoi(v);
    (void)over;
 if (n == 64) {
        /* Same mechanism as cheats=extra_mp_chars, and sticky for the same reason. */
 num_chars_selectable_mp = 0x40;
 g_CheatPlayerTextRelated[1] = GE_CHEAT_ALL_PLAYERS;
 printf("[getv][config] roster=64 — full multiplayer character list ""(CHEAT_EXTRA_MP_CHARS, front.c:4428).\n");
 return;
    }
 if (n == 8) { return; }   /* the shipped default; nothing to do */
 if (n == 33) {
        /* Refused rather than faked. 33 (0x21) is not a settable state:
         * front.c:5327-5335 recomputes the roster every frame from the save file
         * (fileIsStageUnlockedAtDifficulty(..., SP_LEVEL_CRADLE, DIFFICULTY_AGENT)) for
         * any value that is not 0x40. Writing 0x21 here would be overwritten on the
         * first character-select frame and the setting would appear to do nothing. */
 ge_err("roster=33 cannot be forced. 33 is derived from the SAVE FILE — it ""unlocks by completing Cradle on Agent (front.c:5329) and is recomputed ""every frame. Only 8 (default) and 64 (cheat) are settable; use ""roster=64%s%s", "", "");
 return;
    }
 ge_err("roster=%s — expected 8 or 64 (33 is save-derived; see roster=33)%s", v, "");
}

static void list_cheats(void)
{
 int i;
 printf("Named cheats — GoldenEye's OWN cheat flags, set by name.\n""cheats = invincibility, dk_mode, paintball\n""\n""These are NOT GameShark codes. A GameShark code is a raw N64 RDRAM address\n""and this port has no RDRAM, so the published code lists cannot work here.\n""The names below drive the game's own cheat array instead, which is\n""layout-independent, ASLR-proof and survives relinking and modding.\n""\n""[live] takes effect straight from the config — the game reads this flag\n""directly via cheatIsActive() while it runs.\n""[flag] the flag is set, but the effect is applied once by the in-game\n""turn-on path, so toggle it in-game to actually get it.\n\n");
 for (i = 0; i < GE_CHEAT_COUNT; i++) {
 printf("%-22s id %-3d %s\n", GE_CHEATS[i].name, GE_CHEATS[i].id,
 GE_CHEATS[i].live ? "[live]" : "[flag]");
    }
 printf("\n roster = 8 | 64 multiplayer character count ""(33 is save-derived and cannot be forced)\n");
}

static void key_bool_gate(const char *gate, const char *key, const char *v, int over)
{
 if (is_true(v))       { put(gate, "1", over); }
 else if (is_false(v)) { put(gate, "0", over); }
 else                  { ge_err("%s=\"%s\" — expected 0/1 (or on/off)", key, v); }
}

/* ==================================================================== *
 * Enhancement keys -- a reserved seam, not yet implemented.
 *
 * These parse, validate, and export their GETV_* gate exactly like every other key, but
 * nothing consumes them yet. They exist so the option surface is stable before the
 * features land, so a config written today keeps working, and so none of them are
 * foreclosed by accident.
 *
 * Every one defaults off / faithful, deliberately:
 *   1. the N64 look is the product -- an option is a feature, a changed default is a
 * different game;
 *   2. QA here is comparison against real N64 captures, and anything that silently
 * alters output removes the ability to check correctness;
 *   3. `getv/port/fast3d/` is licence-contested (see PROVENANCE.md), so prefer new
 * passes in our own files over edits to inherited Fast3D internals.
 *
 * Enabling one prints a not-yet-implemented notice rather than silently doing nothing.
 * A key that accepts a value and ignores it is worse than no key.
 *
 * Do not implement any of these while correctness work is in flight: apparent
 * regressions are easily confused with artefacts of the comparison itself
 * (PORTING_PLAYBOOK.md §2.10-§2.16). Finish the faithful port, freeze a reference
 * capture set from this build, then enhance against that baseline.
 */
static void key_todo_flag(const char *gate, const char *key, const char *v, int over,
 const char *what)
{
 if (is_false(v)) { put(gate, "0", over); return; }
 if (is_true(v)) {
 put(gate, "1", over);
 printf("[getv][config] %s=1 accepted, but %s IS NOT IMPLEMENTED YET -- the gate ""is reserved and currently has no effect.\n", key, what);
 return;
    }
 ge_err("%s=\"%s\" — expected 0/1 (or on/off)", key, v);
}

static void key_todo_int(const char *gate, const char *key, const char *v, int over,
 int lo, int hi, const char *what)
{
 char buf[32];
 long n;
 char *end = NULL;
 n = strtol(v, &end, 10);
 if (end == v || (end && *end != '\0')) {
 ge_err("%s=\"%s\" — expected an integer", key, v); return;
    }
 if (n < lo) { n = lo; }
 if (n > hi) { n = hi; }
 snprintf(buf, sizeof buf, "%ld", n);
 put(gate, buf, over);
 if (n > 0) {
 printf("[getv][config] %s=%ld accepted, but %s IS NOT IMPLEMENTED YET -- the ""gate is reserved and currently has no effect.\n", key, n, what);
    }
}

/* ------------------------------------------------------------ the dispatcher */

/* Returns 1 if the key was recognised. */
static int apply(const char *key_in, const char *val, int over)
{
 char key[128];
 snprintf(key, sizeof key, "%s", key_in);
 lower(key);

    /* Raw escape hatch: any gate by its real name, uppercased. Kept first so a raw
     * name is never shadowed by a friendly one. */
 if (strncmp(key_in, "GETV_", 5) == 0) { put(key_in, val, over); return 1; }

 if (strcmp(key, "resolution") == 0)  { key_resolution(val, over); return 1; }
 if (strcmp(key, "aspect") == 0)      { key_aspect(val, over); return 1; }
 if (strcmp(key, "framerate") == 0)   { key_framerate(val, over); return 1; }
 if (strcmp(key, "supersample") == 0) { key_supersample(val, over); return 1; }
 if (strcmp(key, "controls") == 0)    { key_controls(val, over); return 1; }
 if (strcmp(key, "filtering") == 0)   { key_filtering(val, over); return 1; }

    /* ---- gamepad / bindings / deadzone / invert-look -------------------------- */
 if (strcmp(key, "gamepad") == 0) { key_gamepad(val, over); return 1; }
 if (strcmp(key, "fire") == 0) { key_bind("GETV_BIND_FIRE", key, val, over); return 1; }
 if (strcmp(key, "aim") == 0) { key_bind("GETV_BIND_AIM", key, val, over); return 1; }
 if (strcmp(key, "use") == 0) { key_bind("GETV_BIND_USE", key, val, over); return 1; }
 if (strcmp(key, "weapon_next") == 0) {
 key_bind("GETV_BIND_WEAPON_NEXT", key, val, over); return 1;
    }
 if (strcmp(key, "weapon_prev") == 0) {
 key_bind("GETV_BIND_WEAPON_PREV", key, val, over); return 1;
    }
 if (strcmp(key, "pause") == 0) { key_bind("GETV_BIND_PAUSE", key, val, over); return 1; }
 if (strcmp(key, "deadzone") == 0) { key_deadzone(val, over); return 1; }
 if (strcmp(key, "invert_look") == 0 || strcmp(key, "invertlook") == 0) {
 key_invert_look(val, over); return 1;
    }

    /* Secondary but genuinely useful. Each is an existing gate. */
 if (strcmp(key, "fullscreen") == 0) { key_bool_gate("GETV_FULLSCREEN", key, val, over); return 1; }

    /* --- reserved enhancement seam; see the block above. Parsed, gated, unconsumed. --- */
 if (strcmp(key, "depth_bits") == 0) {
 key_todo_int("GETV_DEPTH_BITS", key, val, over, 16, 32,
 "a wider depth buffer (N64 z-fighting is a 16-bit Z limit, not an aesthetic)");
 return 1;
    }
 if (strcmp(key, "anisotropic") == 0) {
 key_todo_int("GETV_ANISO", key, val, over, 0, 16, "anisotropic filtering"); return 1;
    }
 if (strcmp(key, "msaa") == 0) {
 key_todo_int("GETV_MSAA", key, val, over, 0, 8,
 "MSAA (note: the N64 HAD anti-aliasing and we currently do not)");
 return 1;
    }
 if (strcmp(key, "mipmaps") == 0) {
 key_todo_flag("GETV_MIPMAPS", key, val, over, "mipmapping + LOD bias"); return 1;
    }
 if (strcmp(key, "fog_per_pixel") == 0) {
 key_todo_flag("GETV_FOG_PERPIXEL", key, val, over,
 "per-pixel fog (N64 fog is per-VERTEX; FRIGATE is the one fogless level)");
 return 1;
    }
 if (strcmp(key, "muzzle_lights") == 0) {
 key_todo_flag("GETV_MUZZLE_LIGHTS", key, val, over,
 "dynamic muzzle-flash lighting"); return 1;
    }
 if (strcmp(key, "audio_3d") == 0 || strcmp(key, "hrtf") == 0) {
 key_todo_flag("GETV_AUDIO_3D", key, val, over, "positional 3D audio / HRTF"); return 1;
    }
 if (strcmp(key, "ssao") == 0) {
 key_todo_flag("GETV_SSAO", key, val, over, "screen-space ambient occlusion"); return 1;
    }
 if (strcmp(key, "shadows") == 0) {
 key_todo_flag("GETV_SHADOWS", key, val, over,
 "real-time shadow maps (GE ships blob shadows)"); return 1;
    }
 if (strcmp(key, "per_pixel_lighting") == 0) {
 key_todo_flag("GETV_PERPIXEL_LIGHT", key, val, over,
 "per-pixel lighting -- this one changes the LOOK most of any enhancement; ""N64 lighting is per-vertex Gouraud");
 return 1;
    }
 if (strcmp(key, "preset") == 0) {
 if (strcmp(val, "faithful") == 0)      { put("GETV_PRESET", "faithful", over); }
 else if (strcmp(val, "enhanced") == 0) {
 put("GETV_PRESET", "enhanced", over);
 printf("[getv][config] preset=enhanced accepted, but NO ENHANCEMENT IS ""IMPLEMENTED YET -- this is a reserved seam.\n");
        } else { ge_err("preset=\"%s\" — expected faithful|enhanced", val, ""); }
 return 1;
    }
 if (strcmp(key, "unlock_all") == 0 || strcmp(key, "unlockall") == 0) {
 key_bool_gate("GETV_UNLOCKALL", key, val, over); return 1;
    }
 if (strcmp(key, "save_dir") == 0 || strcmp(key, "savedir") == 0) {
 put("GETV_SAVEDIR", val, over); return 1;
    }
 if (strcmp(key, "audio") == 0) {
        /* inverted gate: GETV_NO_AUDIO is presence-tested, so it must be UNSET to
         * mean "on", never set to "0". */
 if (is_false(val))     { put("GETV_NO_AUDIO", "1", over); }
 else if (!is_true(val)) { ge_err("audio=\"%s\" — expected 0/1%s", val, ""); }
 return 1;
    }
 if (strcmp(key, "realclock") == 0 || strcmp(key, "real_clock") == 0) {
 key_bool_gate("GETV_REALCLOCK", key, val, over); return 1;
    }

    /* ---- Rare's own left-in developer features ------------------------------ */

 if (strcmp(key, "cheats") == 0) { key_cheats(val, over); return 1; }
 if (strcmp(key, "roster") == 0) { key_roster(val, over); return 1; }

 if (strcmp(key, "debug_position") == 0 || strcmp(key, "debugpos") == 0) {
 if (is_true(val)) {
 put("GETV_DEBUGPOS", "1", over);
 set_debug_testingmanpos_flag(1);
 printf("[getv][config] debug_position=1 — Rare's room + XYZ + heading ""readout is ON (debugmenu_handler.c:1018 -> bondview2.c:10367). ""Works in a stock build; no DEBUGMENU needed.\n");
        } else if (is_false(val)) {
 set_debug_testingmanpos_flag(0);
        } else {
 ge_err("debug_position=\"%s\" — expected 0/1%s", val, "");
        }
 return 1;
    }

 if (strcmp(key, "debug_menu") == 0 || strcmp(key, "debugmenu") == 0) {
        /* Not a runtime toggle. This branch exists to say so rather than to silently
         * do nothing.
         *
         * The roughly 1,100-line menu (src/debugmenu.c + src/game/debugmenu_handler.c)
         * is already compiled into every binary by -DLEFTOVERDEBUG. What is missing is
         * the trigger: src/boss.c:565 gates the C-Up + C-Down opener on `DEBUGMENU`, a
         * macro the decomp's own Makefile:101 defines but our build scripts do not.
         *
         * It cannot be a config key because it is not a branch that can be taken at
         * runtime — it changes codegen in two places:
         * boss.c:565-576 adds an `else if (joyGetButtons(0, START_BUTTON) == 0)`
         * arm that hijacks START into `g_DebugMode = <highlighted>`
         * debugmenu.c:419 turns `if ((randomGetNext() & 0xFF) < g_DebugMenuPercentage)`
         * into a literal `if (1)`
         *
         * The equivalent build knob is
         * GETV_DEBUGMENU=1 ./build_mac.sh lib && ./build_mac.sh app
         * (build_mac.sh:93-96 — note `lib`, not `port`: it is a game-object flag.) */
 if (is_true(val)) {
 printf("[getv][config] debug_menu cannot be enabled at runtime — it is a ""BUILD option because it changes codegen (boss.c:565 repurposes START, ""debugmenu.c:419 becomes if(1)). Rebuild with:\n""GETV_DEBUGMENU=1 ./build_mac.sh lib && ./build_mac.sh app\n""Also note there is NO working level select in it: ""DEB_LEVEL/DEB_REGION/DEB_SCALE are gutted no-ops ""(debugmenu_handler.c:511-521). Use GETV_STAGE=<n> instead.\n");
        }
 return 1;
    }

 return 0;
}

/* ------------------------------------------------------------- file location */

static int try_path(const char *p)
{
 FILE *f;
 if (p == NULL || *p == '\0') { return 0; }
 f = fopen(p, "r");
 if (f == NULL) { return 0; }
 fclose(f);
 snprintf(g_cfgpath, sizeof g_cfgpath, "%s", p);
 return 1;
}

static int locate(const char *argv0, const char *cliPath)
{
 char buf[1024];
 char dir[1024];

 if (try_path(getenv("GETV_CONFIG"))) { return 1; }
 if (try_path(cliPath)) { return 1; }

 if (argv0 != NULL) {
 const char *slash = strrchr(argv0, '/');
 if (slash != NULL) {
 size_t n = (size_t)(slash - argv0);
 if (n < sizeof buf - sizeof(GE_CFG_BASENAME) - 2) {
 memcpy(buf, argv0, n);
 buf[n] = '\0';
 strcat(buf, "/" GE_CFG_BASENAME);
 if (try_path(buf)) { return 1; }
            }
        } else if (try_path(GE_CFG_BASENAME)) {
 return 1;
        }
    }

    /* Search step 4: the per-user config directory. On macOS this is exactly the
     * "$HOME/Library/Application Support/GoldenEye/goldeneye.cfg" this function used
     * to build by hand; see getv/port/src/port_paths.c for the other hosts. */
 if (gePortUserDataDir("Goldeneye-Native", "Goldeneye-Native", dir, sizeof dir) == 0) {
 snprintf(buf, sizeof buf, "%s/" GE_CFG_BASENAME, dir);
 if (try_path(buf)) { return 1; }
    }
 return 0;
}

static void read_file(void)
{
 FILE *f = fopen(g_cfgpath, "r");
 char line[1024];
 int lineno = 0;

 if (f == NULL) { return; }
 while (fgets(line, sizeof line, f) != NULL) {
 char *k, *eq, *v;
 lineno++;
        /* '#' and ';' start a comment anywhere on the line. There is no escape for
         * them: no GETV_* value needs one, and a quoting rule is easy to get wrong. */
 k = strchr(line, '#'); if (k != NULL) { *k = '\0'; }
 k = strchr(line, ';'); if (k != NULL) { *k = '\0'; }

 k  = trim(line);
 if (*k == '\0') { continue; }
 eq = strchr(k, '=');
 if (eq == NULL) {
 printf("[getv][config] %s:%d: no '=' on this line, ignored: \"%s\"\n",
 g_cfgpath, lineno, k);
 continue;
        }
        *eq = '\0';
 v = trim(eq + 1);
 k = trim(k);
        {
 char lv[512];
 snprintf(lv, sizeof lv, "%s", v);
            /* Values are lowercased for matching except for paths and raw GETV_*
             * passthrough, where case is meaningful. */
 if (strncmp(k, "GETV_", 5) != 0 &&
 strcmp(k, "save_dir") != 0 && strcmp(k, "savedir") != 0) {
 lower(lv);
            }
 if (!apply(k, lv, /*overwrite=*/0)) {
 printf("[getv][config] %s:%d: unknown key \"%s\" (ignored). ""Prefix a raw gate with GETV_ to set it directly.\n",
 g_cfgpath, lineno, k);
            }
        }
    }
 fclose(f);
 ge_config_loaded = 1;
}

/* ------------------------------------------------------------------- CLI + help */

static void usage(void)
{
 printf(
"GoldenEye 007 — native macOS port\n"
"\n"
"goldeneye [--key=value ...]\n"
"\n"
"Settings come from, in DECREASING order of priority:\n"
"1. these command-line flags\n"
"2. GETV_* environment variables      (unchanged; every existing gate works)\n"
"3. " GE_CFG_BASENAME " beside the binary, or\n"
"~/Library/Application Support/GoldenEye/" GE_CFG_BASENAME "\n"
"4. built-in defaults\n"
"\n"
"--config=PATH read this config file instead of searching\n"
"--write-config[=P] write a commented default config file and exit\n"
"--help this text\n"
"\n"
"Day-0 keys (same names on the CLI and in the file):\n"
"resolution=WxH window size, min 320x240; or \"fullscreen\"[1280x960]\n"
"aspect=4:3|16:9|auto picks a default window shape if resolution is unset\n"
"framerate=30|50|60|off                                             [60]\n"
"120 is REJECTED — see the note in the written config\n"
"supersample=1|2      2 = render at 2x and downsample                [1 on macOS]\n"
"controls=1.1..2.4 or honey/solitaire/kissy/goodnight/plenty/galore/\n"
"domino/goodhead.  2.2 and 2.4 are dual-analog.\n"
"GE's own default is 1.1 Honey; the shipped template picks\n"
"2.2 Galore because this port presents one modern pad as\n"
"N64 ports 0+1.\n"
"filtering=point|bilinear|three-point                               [three-point]\n"
"gamepad=auto|xbox|playstation|switch|generic changes PROMPT GLYPHS only  [auto]\n"
"fire=aim=use=weapon_next=weapon_prev=pause=a|b|x|y|lb|rb|lt|rt|start|back|none\n"
"names are POSITIONAL (a = bottom face button), not label\n"
"[fire=rt aim=lt use=b weapon_next=a weapon_prev=none pause=start]\n"
"deadzone=0..40 stick deadzone, percent, clamped to range          [20]\n"
"invert_look=0|1 forces look inversion; UNSET = save file decides   [unset]\n"
"fullscreen=0|1 audio=0|1 unlock_all=0|1 save_dir=PATH\n"
"cheats=a,b,c GE's OWN named cheat flags (NOT GameShark addresses)\n"
"roster=8|64 multiplayer character count\n"
"debug_position=0|1 Rare's room + XYZ + heading readout\n"
"\n"
"--list-cheats every named cheat and what it needs\n"
"\n"
"Any raw gate can be set by its real name, e.g. GETV_STAGE=34 or --GETV_STAGE=34.\n");
}

static const char *DEFAULT_CFG =
"# goldeneye.cfg — GoldenEye 007, native macOS port\n"
"#\n"
"# Lines are key = value.  '#' and ';' start a comment.\n"
"# Precedence: command line  > GETV_* environment  > this file  > defaults.\n"
"# An environment variable ALWAYS beats this file, so measurement harnesses that\n"
"# export GETV_* keep working exactly as before.\n"
"\n"
"# --- display ---------------------------------------------------------------\n"
"resolution  = 1280x960     # WIDTHxHEIGHT (min 320x240), or \"fullscreen\"\n"
"aspect      = 4:3          # 4:3 | 16:9 | auto. Only used if resolution is unset.\n"
"fullscreen  = 0\n"
"supersample = 1            # 1 or 2. 2 renders at double size and downsamples.\n"
"filtering   = three-point  # point | bilinear | three-point (three-point = real N64)\n"
"\n"
"# --- framerate -------------------------------------------------------------\n"
"# 30, 50, 60 or off. 120 IS NOT SUPPORTED AND WILL BE REJECTED.\n"
"#\n"
"# GoldenEye counts time in WHOLE VIDEO FRAMES, not seconds. Fire rates, reload\n"
"# times, physics, animation and the mission clock are all integrated once per\n"
"# rendered frame. Running at 120 therefore does not make the game smoother — it\n"
"# makes the entire game run at DOUBLE SPEED. (30 likewise halves it.) Genuine\n"
"# high-refresh support needs the simulation decoupled from rendering, which this\n"
"# build does not do yet.\n"
"framerate   = 60\n"
"\n"
"# --- controls --------------------------------------------------------------\n"
"# All eight of Rare's control styles are selectable:\n"
"#   1.1 honey   1.2 solitaire   1.3 kissy    1.4 goodnight   (one controller)\n"
"#   2.1 plenty  2.2 galore      2.3 domino   2.4 goodhead    (two controllers)\n"
"# 2.2 galore and 2.4 goodhead are the true dual-analog layouts and are what a\n"
"# modern gamepad maps to, so 2.2 is the PORT default -- one physical pad is\n"
"# presented as N64 ports 0+1 (right stick looks, left stick moves). Set 1.1 for\n"
"# Rare's shipped single-controller scheme.\n"
"controls    = 2.2\n"
"\n"
"# --- gamepad / bindings ------------------------------------------------------\n"
"# gamepad picks which glyphs get PRINTED for prompts (auto|xbox|playstation|\n"
"# switch|generic) -- it never changes what a binding below does.\n"
"gamepad     = auto\n"
"#\n"
"# Binding values are POSITIONAL, not label-based: \"a\" always means the\n"
"# BOTTOM face button on the pad, whatever it is labelled -- SDL maps the\n"
"# physically-bottom button to _BUTTON_A on every controller it knows, including\n"
"# Nintendo's (where that same button is printed \"B\"). Valid values: a b x y\n"
"# lb rb lt rt start back none.\n"
"fire        = rt\n"
"aim         = lt\n"
"use         = b\n"
"weapon_next = a\n"
"# weapon_prev defaults to NONE on purpose -- GoldenEye has no back-cycle button.\n"
"# The retail gesture is hold-inventory + tap-fire (bondview2.c:5091-5111); the\n"
"# synthesised single-button version (port_os.c) is faithful to that gesture but\n"
"# unverified on real hardware, so it stays opt-in rather than on by default.\n"
"weapon_prev = none\n"
"pause       = start\n"
"deadzone    = 20          # percent, 0-40, clamped -- worn-pad drift trimmer\n"
"invert_look = 1           # stick UP looks UP. MEASURED, not a preference toggle:\n"
"#                         # GE's DEFAULT_OPTIONS omits OPTION_INVERTLOOK, which makes\n"
"#                         # invertPitch=1, so stick-up drives pitch DOWN at full rate\n"
"#                         # and pins at the -90 deg clamp in ~1.5s -- the camera ends up\n"
"#                         # staring at the floor with nothing to recentre it. Spawn\n"
"#                         # pitch itself is correct (-4.0 deg) and the sign in our code\n"
"#                         # is correct; it is the retail DEFAULT that is hostile to a\n"
"#                         # modern stick. Comment this line out for retail behaviour\n"
"#                         # (save file decides); set 0 to force non-inverted.\n"
"\n"
"# --- misc ------------------------------------------------------------------\n"
"audio       = 1\n"
"# unlock_all = 1          # show every mission on the file-select screen\n"
"# save_dir   = /path/to/saves\n"
"\n"
"# --- named cheats ----------------------------------------------------------\n"
"# These are GoldenEye's OWN cheat flags, exposed BY NAME — not GameShark codes.\n"
"# A GameShark code is a raw N64 RDRAM address, and this port has no RDRAM, so the\n"
"# published code lists cannot be applied here at all. Driving the game's own cheat\n"
"# system instead is layout-independent, ASLR-proof, and still correct after a\n"
"# relink or a mod that moves things around. Run --list-cheats for the full set.\n"
"# Six take effect straight from this file: dk_mode, infinite_ammo, paintball,\n"
"# no_radar, enemy_rockets, extra_mp_chars. The rest set the flag but need to be\n"
"# toggled in-game to apply — --list-cheats marks which is which.\n"
"# cheats = dk_mode, paintball, infinite_ammo\n"
"#\n"
"# roster = 64             # full multiplayer character list.\n"
"#                         # 8 = default, 64 = cheat. 33 is NOT settable: it is\n"
"#                         # derived from the save (complete Cradle on Agent).\n"
"\n"
"# --- Rare's own leftover developer features --------------------------------\n"
"# debug_position = 1      # room id + X/Y/Z + compass heading on screen.\n"
"#                         # Works in a stock build. Rare's own readout.\n"
"# debug_menu     = 1      # NOT a runtime setting — it changes codegen and\n"
"#                         # repurposes START. Rebuild instead:\n"
"#                         # GETV_DEBUGMENU=1 ./build_mac.sh lib && ./build_mac.sh app\n"
"#                         # Its level select does NOT work (gutted no-ops).\n"
"#                         # Use GETV_STAGE = <n> to pick a level.\n"
"\n"
"# --- FUTURE: enhancement seam (RESERVED, NOT IMPLEMENTED YET) --------------\n"
"# These keys parse and validate today but NOTHING CONSUMES THEM. They exist so the\n"
"# option surface is stable before the features land. Turning one on prints a\n"
"# not-implemented notice rather than silently doing nothing.\n"
"#\n"
"# ALL DEFAULT OFF ON PURPOSE. The N64 look is the product, and our QA method is\n"
"# comparison against real N64 captures -- anything that silently alters output\n"
"# destroys our ability to check correctness. Enhancements are OPTIONS, never the\n"
"# default, and none should be built until the faithful port is frozen.\n"
"#\n"
"# preset = faithful        # faithful | enhanced  (one switch for the lot)\n"
"#\n"
"# Tier 1 -- removes N64 HARDWARE LIMITS, does not change artistic intent:\n"
"# depth_bits    = 24       # 16 | 24 | 32. N64 z-fighting is a 16-bit Z limit.\n"
"# anisotropic   = 8        # 0-16. Tiny textures at grazing angles; biggest cheap win.\n"
"# msaa          = 4        # 0 | 2 | 4 | 8. The N64 HAD AA; we currently do not.\n"
"# mipmaps       = 1        # kills distant-texture shimmer\n"
"# fog_per_pixel = 1        # N64 fog is per-VERTEX. Frigate is the one fogless level.\n"
"#\n"
"# Tier 2 -- cheap and dramatic, no new art:\n"
"# muzzle_lights = 1        # dynamic light on muzzle flashes. Best value on the list:\n"
"#                          # firing in a dark level currently lights nothing.\n"
"# audio_3d      = 1        # positional audio / HRTF (alias: hrtf)\n"
"#\n"
"# Tier 3 -- real work, and the point where it stops being GoldenEye:\n"
"# ssao               = 1   # needs the depth buffer; strong in corridors\n"
"# shadows            = 1   # GE ships blob shadows; real shadow maps are a scope trap\n"
"# per_pixel_lighting = 1   # CHANGES THE LOOK MOST. N64 lighting is per-vertex Gouraud.\n"
"\n"
"# --- raw escape hatch ------------------------------------------------------\n"
"# Any of the port's ~100 GETV_* development gates can be set by its real name:\n"
"# GETV_STAGE = 34\n"
"# GETV_EXIT_FRAME = 61\n";

static int write_default(const char *path)
{
 char buf[1024];
 FILE *f;
 if (path == NULL || *path == '\0') {
 if (gePortUserDataDir("Goldeneye-Native", "Goldeneye-Native", buf, sizeof buf) != 0) {
 printf("[getv][config] no $HOME\n"); return 1;
        }
        /* Created in-process rather than by shelling out to `mkdir -p`, which would be
         * a hard dependency on a POSIX shell and on /bin/mkdir existing, and would
         * interpolate a path straight into a command line. gePortMakeDirTree() uses the
         * same 0777 & ~umask mode that mkdir(1) does, so an untouched umask 022 still
         * yields 0755. Unlike the save directory, this one is created recursively,
         * because --write-config is explicitly a "set this machine up" command. */
 if (gePortMakeDirTree(buf, 0777) != 0) {
 printf("[getv][config] mkdir failed: %s\n", buf);
        }
 strcat(buf, "/" GE_CFG_BASENAME);
 path = buf;
    }
 f = fopen(path, "w");
 if (f == NULL) { printf("[getv][config] cannot write %s\n", path); return 1; }
 fputs(DEFAULT_CFG, f);
 fclose(f);
 printf("[getv][config] wrote %s\n", path);
 return 0;
}

/* ---------------------------------------------------------------------- init */

/* Returned by geConfigInit() when a flag has done its whole job and the process should
 * stop successfully (--help, --write-config, --list-cheats). Distinct from a positive
 * return, which means a fatal config error. */
#define GE_CONFIG_STOP (-1)

int geConfigInit(int argc, char **argv)
{
 const char *cliPath = NULL;
 const char *writePath = NULL;
 int doWrite = 0, doHelp = 0;
 int i;

    /* Pass 1 — only the flags that change what happens next. Nothing is applied yet,
     * because --config must be known before the file is read and every other flag must
     * be applied after it. */
 for (i = 1; i < argc; i++) {
 const char *a = argv[i];
 if (strncmp(a, "--config=", 9) == 0)            { cliPath = a + 9; }
 else if (strcmp(a, "--config") == 0 && i + 1 < argc) { cliPath = argv[++i]; }
 else if (strncmp(a, "--write-config=", 15) == 0) { doWrite = 1; writePath = a + 15; }
 else if (strcmp(a, "--write-config") == 0)       { doWrite = 1; }
 else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { doHelp = 1; }
 else if (strcmp(a, "--list-cheats") == 0) { list_cheats(); return GE_CONFIG_STOP; }
    }
 if (doHelp)  { usage(); return GE_CONFIG_STOP; }
    /* write_default() uses the exit-code convention (0 = success), but the caller
     * treats nonzero as "stop". Returning it raw would make a successful
     * --write-config fall through and boot the game while a failed one exited cleanly,
     * i.e. exactly backwards. Map both onto the explicit stop sentinel. */
 if (doWrite) { return write_default(writePath) == 0 ? GE_CONFIG_STOP : 1; }

    /* Earlier builds wrote the config to a directory named "GoldenEye" while saves went
     * to "Goldeneye-Native". The two are unified now, but an existing install has a tuned
     * config under the old name; adopt it rather than presenting a fresh one. Nothing is
     * copied or deleted -- the file keeps working where it is. */
    if (cliPath == NULL) {
        const char *home = getenv("HOME");
        if (home != NULL && *home != '\0') {
            static char oldp[1024];
            char newp[1024];
            struct stat st;
            snprintf(oldp, sizeof oldp,
                     "%s/Library/Application Support/GoldenEye/" GE_CFG_BASENAME, home);
            snprintf(newp, sizeof newp,
                     "%s/Library/Application Support/Goldeneye-Native/" GE_CFG_BASENAME, home);
            if (stat(oldp, &st) == 0 && stat(newp, &st) != 0) {
                printf("[getv][config] using the pre-rename config: %s\n", oldp);
                cliPath = oldp;
            }
        }
    }

    /* Pass 2 — the file, with overwrite=0 so the environment always wins. */
 if (locate(argc > 0 ? argv[0] : NULL, cliPath)) {
 read_file();
    } else if (cliPath != NULL) {
 printf("[getv][config] --config=%s not found\n", cliPath);
 g_errors++;
    } else {
        /* First run: no config anywhere and none asked for, so write the template and
         * then read it. This is not a convenience; it is how the port's tuned defaults
         * reach a player at all.
         *
         * A default that only exists inside a file the user has never generated is not
         * a default. `invert_look = 1` is the case in point: a user with no config file
         * gets retail's default instead, and retail omits OPTION_INVERTLOOK, which
         * makes stick-up drive pitch down at full rate and pin at the -90 degree clamp
         * within about 1.5 seconds. A fresh install then opens with the camera staring
         * at the floor.
         *
         * Failure is deliberately non-fatal and near-silent: a read-only HOME must
         * still boot on built-in defaults. */
 if (write_default(NULL) == 0 &&
 locate(argc > 0 ? argv[0] : NULL, NULL)) {
 printf("[getv][config] first run -- wrote a default config; edit it to taste\n");
 read_file();
        }
    }

    /* Pass 3 — the command line, with overwrite=1 so it beats the environment. */
 for (i = 1; i < argc; i++) {
 char *a = argv[i];
 char kv[512], *eq;
 if (strncmp(a, "--", 2) != 0) { continue; }
 if (strncmp(a, "--config", 8) == 0 || strncmp(a, "--write-config", 14) == 0 ||
 strcmp(a, "--help") == 0 || strcmp(a, "--list-cheats") == 0) { continue; }
 snprintf(kv, sizeof kv, "%s", a + 2);
 eq = strchr(kv, '=');
 if (eq == NULL) {
 if (i + 1 < argc && argv[i + 1][0] != '-') {
 char joined[600];
 snprintf(joined, sizeof joined, "%s=%s", kv, argv[++i]);
 snprintf(kv, sizeof kv, "%s", joined);
 eq = strchr(kv, '=');
            } else {
 printf("[getv][config] ignoring \"%s\": expected --key=value\n", a);
 continue;
            }
        }
        *eq = '\0';
        {
 char lv[512];
 snprintf(lv, sizeof lv, "%s", eq + 1);
 if (strncmp(kv, "GETV_", 5) != 0 &&
 strcmp(kv, "save_dir") != 0 && strcmp(kv, "savedir") != 0) {
 lower(lv);
            }
 if (!apply(kv, lv, /*overwrite=*/1)) {
 printf("[getv][config] ignoring unknown flag --%s\n", kv);
            }
        }
    }

 printf("[getv][config] %s%s | window=%s fps=%s ss=%s controls=%d filtering=%u\n",
 ge_config_loaded ? "file " : "no config file",
 ge_config_loaded ? g_cfgpath : "",
 getenv("GETV_WINDOW")      ? getenv("GETV_WINDOW")      : "default",
 getenv("GETV_FPS")         ? getenv("GETV_FPS")         : "default",
 getenv("GETV_SUPERSAMPLE") ? getenv("GETV_SUPERSAMPLE") : "default",
 ge_config_controls, configFiltering);
 if (g_errors > 0) {
 printf("[getv][config] %d setting(s) were rejected — see above. ""The game will start with the defaults for those.\n", g_errors);
    }
 fflush(stdout);
 return 0;
}
