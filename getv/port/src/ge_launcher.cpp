/* The launcher window.
 *
 * What this is. A window that opens before the game, collects settings, and then starts the
 * game with them. It is not new engine capability: the mod surface is already about 275
 * GETV_* environment gates plus goldeneye.cfg, and this is a user interface over that
 * surface. Every control below resolves to an environment variable that already existed and
 * already worked from a shell.
 *
 * ---------------------------------------------------------------- why it re-execs
 *
 * The one measured fact that decides this file's shape: **76 of those gates are read once
 * into a static on first use** -- the `static int x = -1; if (x == -1) x = getenv(...)`
 * pattern, counted across getv/port. A setting changed after the game has started therefore
 * does nothing, silently, for most of the surface. An in-process launcher that toggled
 * options and then continued into SDL_main() would appear to work and would be wrong for
 * every gate that had already been touched.
 *
 * So the launcher sets the environment and execv()s the binary again with --launcher
 * removed. The game then starts in a pristine process where nothing has read anything yet,
 * which is the only arrangement where every gate is guaranteed to take effect. It also makes
 * "change a setting and relaunch" exactly as correct as starting from a shell, because it
 * *is* starting from a shell.
 *
 * A second reason, less obvious and just as decisive: the launcher creates its own SDL
 * window, GL context and ImGui context. The game's renderer creates its own later, and
 * gfx_sdl2.c assumes it is the one initialising SDL video. Handing a used SDL over to it
 * would be a source of subtle, platform-specific breakage for no benefit.
 *
 * ---------------------------------------------------------------- what it does not do
 *
 * It does not write goldeneye.cfg. The file stays the user's, edited by hand or by
 * --write-config, and the launcher composes a run on top of whatever it says. Every control
 * starts from the value already in the environment, which is the value the config layer just
 * resolved -- so the launcher opens showing the current configuration rather than a set of
 * defaults that disagree with it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "ge_launcher_policy.h"
#include "ge_developer_tools.h"
#include <time.h>
/* The headers ge_lua.c uses to walk the mods directory, and for the same reason: the launcher
 * has to discover exactly what the loader would.
 *
 * <sys/types.h> and the explicit <sys/stat.h> are the Mac's fix -- MinGW pulls `struct stat`
 * in transitively and Clang does not, so this compiled here and failed there. <dirent.h>
 * stays UNCONDITIONAL rather than moving into the non-Windows branch: mod_scan() calls
 * opendir/readdir on every platform, MinGW ships the header, and putting it behind #else
 * trades a Clang break for a MinGW one. Build-tests one platform, ships three -- in both
 * directions. */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "ge_actions.h"
#include "ge_bindings.h"
#include "ge_config.h"

/* The real errno. getv/port/include/ge_win_compat.h undefines errno on Windows so that
 * PR/os.h's struct fields of that name can parse; MSVCRT exposes the value through
 * _errno(). Spelled once here rather than at each use. */
#if defined(_WIN32)
#define ge_errno (*_errno())
#else
#define ge_errno errno
#endif


#if defined(GE_WITH_IMGUI)

#include <SDL2/SDL.h>
#if defined(_WIN32)
#include <windows.h>
#include <process.h>   /* _execv */
#else
#include <unistd.h>
#endif

/* Same icon as the game window, from the same pixels. See ge_icon_apply.c.
 *
 * Declared outside the _WIN32 split, not inside its #else. ge_icon_apply.c builds on every
 * desktop platform and the call site below is guarded by GE_PLATFORM_DESKTOP, so keeping the
 * declaration on the non-Windows branch alone left Windows compiling the call with nothing in
 * scope -- "gePortSetWindowIcon was not declared in this scope; did you mean SDL_SetWindowIcon".
 * The platform split above is about which system headers to pull in, which this is not. */
#if defined(__APPLE__) && defined(GE_PLATFORM_MAC)
#include "../mac/ge_renderer_choice.h"
#else
static bool geAppRendererActive(void) { return false; }
#endif

extern "C" void gePortSetWindowIcon(SDL_Window *w);

#ifdef RAPI_METAL
/* The launcher's own window uses its own standalone Metal context
 * (ge_launcher_metal.mm), NOT gfx_metal.mm's game-window state -- that does not exist yet
 * when the launcher runs (main()/SDL_main() calls gePortLauncherRun() before gfx_init()). */
#include "ge_launcher_metal.h"
#endif

/* Windows needs an extension loader before any GL header. opengl32.dll exports GL 1.1 and
 * nothing later, so glGetVertexAttribiv and everything else past 1997 resolves only through
 * GLEW -- without this the link fails on __imp_glGetVertexAttribiv, which reads like a
 * missing DLL import and is really a missing loader. gfx_opengl.c does exactly the same on
 * __MINGW32__; macOS and Linux need no equivalent. */
#if defined(_WIN32)
#define GLEW_STATIC
#include <GL/glew.h>
#endif

/* No GL on this target (RAPI_METAL). imgui.h/imgui_impl_sdl2.h stay unconditional -- the
 * UI-drawing helper functions throughout this file (ge_load_fonts, ge_apply_style, the page
 * renderers) use plain ImGui:: calls with no renderer dependency, and imgui_impl_sdl2.h is the
 * SDL PLATFORM backend (events/input), shared across GL and Metal. Only the OpenGL2 RENDERER
 * backend and the raw GL headers are GL-specific -- see gePortLauncherRun()'s own #ifdef
 * RAPI_METAL / #else split for why its body does not need them under RAPI_METAL either. */
#ifndef RAPI_METAL
#if defined(USE_GLES)
#include <SDL2/SDL_opengles2.h>
#else
#include <SDL2/SDL_opengl.h>
#endif
#endif /* RAPI_METAL */

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#ifndef RAPI_METAL
#include "imgui_impl_opengl2.h"
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <TargetConditionals.h>
#endif

namespace {

/* Defined further down with the exec helpers, used well above it to resolve a relative mods
 * path against the binary's own directory. Declared at the top of the namespace rather than
 * immediately before its first use: the mid-file form compiled under MinGW and Clang
 * rejected it. */
bool self_path(char *out, size_t n);

/* ---------------------------------------------------------------- the model
 *
 * Plain values, seeded from the environment, written back to the environment on launch.
 * Nothing here holds a default of its own: a default that disagreed with ge_config.c's would
 * be a second source of truth, and the first symptom would be the launcher quietly undoing a
 * setting from the config file. */

int   env_int(const char *k, int fallback)
{
    const char *v = getenv(k);
    if (v == NULL || *v == '\0') return fallback;
    return atoi(v);
}

bool  env_bool(const char *k, bool fallback)
{
    const char *v = getenv(k);
    if (v == NULL || *v == '\0') return fallback;
    return (*v != '0');
}

void  env_str(const char *k, char *dst, size_t n, const char *fallback)
{
    const char *v = getenv(k);
    snprintf(dst, n, "%s", (v && *v) ? v : fallback);
}

void  put_int(const char *k, int v) { char b[32]; snprintf(b, sizeof b, "%d", v); setenv(k, b, 1); }
void  put_str(const char *k, const char *v)
{
    if (v && *v) setenv(k, v, 1);
    else         unsetenv(k);
}

/* The stage list. Ids and names are from the project's own stage reference table, the
 * ground truth for which stages are solo, multiplayer-only or have no data at all. Only
 * loadable stages are offered: eleven ids can never load (nine cut, plus CITADEL, whose
 * background exists but whose setup file does not), and offering them would be offering a
 * hang. MP-only stages are marked because selecting one solo loads geometry with no setup,
 * which looks like a rendering bug and is not one. */
/* Ordered as the campaign is played, not by stage id. The id order is an artefact of the
 * ROM and means nothing to a player: "Bunker 1, Silo, Statue, Control..." is not a sequence
 * anyone recognises, while "01 Dam, 02 Facility, 03 Runway..." is the game people remember.
 * The mission number and the theatre it belongs to come from the game's own briefings, and
 * they are what make the list scannable -- the id is an implementation detail and is not
 * shown at all. `mission` is 0 for the multiplayer-only stages, which have no campaign slot
 * and are grouped separately in the UI for that reason. */
struct Stage { int id; const char *name; const char *place; int mission; bool mp_only; };
const Stage kStages[] = {
    { 33, "Dam",        "Arkangelsk",     1,  false },
    { 34, "Facility",   "Arkangelsk",     2,  false },
    { 35, "Runway",     "Arkangelsk",     3,  false },
    { 36, "Surface",    "Severnaya",      4,  false },
    {  9, "Bunker 1",   "Severnaya",      5,  false },
    { 20, "Silo",       "Kirghizstan",    6,  false },
    { 26, "Frigate",    "Monte Carlo",    7,  false },
    { 43, "Surface 2",  "Severnaya",      8,  false },
    { 27, "Bunker 2",   "Severnaya",      9,  false },
    { 22, "Statue",     "St Petersburg",  10, false },
    { 24, "Archives",   "St Petersburg",  11, false },
    { 29, "Streets",    "St Petersburg",  12, false },
    { 30, "Depot",      "St Petersburg",  13, false },
    { 25, "Train",      "St Petersburg",  14, false },
    { 37, "Jungle",     "Cuba",           15, false },
    { 23, "Control",    "Cuba",           16, false },
    { 39, "Caverns",    "Cuba",           17, false },
    { 41, "Cradle",     "Cuba",           18, false },
    { 28, "Aztec",      "Bonus",          19, false },
    { 32, "Egypt",      "Bonus",          20, false },
    { 31, "Complex",    "Multiplayer",    0,  true  },
    { 38, "Temple",     "Multiplayer",    0,  true  },
    { 45, "Basement",   "Multiplayer",    0,  true  },
    { 46, "Stack",      "Multiplayer",    0,  true  },
    { 48, "Library",    "Multiplayer",    0,  true  },
    { 50, "Caves",      "Multiplayer",    0,  true  },
};
const int kStageCount = (int)(sizeof kStages / sizeof kStages[0]);

/* Mirrors GE_CHEATS in ge_config.c. `live` there means the cheat has a real cheatIsActive()
 * consumer, so setting the flag is enough; the others need in-game activation because their
 * effect lives in the turn-on switch, which needs a player context that does not exist at
 * startup. That distinction is surfaced in the UI rather than hidden, because a checkbox that
 * silently does nothing is worse than one that says it will not apply yet. */
/* `name` is the token GETV_CHEATS is built from and must not change; `label` is what the
 * launcher shows. They were the same string until the UI grew up, and "10x_health" in a
 * settings list reads as a debug symbol rather than as a cheat anyone recognises. */
struct Cheat { const char *name; const char *label; bool live; };
const Cheat kCheats[] = {
    { "invincibility", "Invincibility",   false },
    { "all_guns",      "All guns",        false },
    { "max_ammo",      "Max ammo",        false },
    { "infinite_ammo", "Infinite ammo",   true  },
    { "dk_mode",       "DK mode",         true  },
    { "paintball",     "Paintball mode",  true  },
    { "no_radar",      "No radar",        true  },
    { "enemy_rockets", "Enemy rockets",   true  },
    { "invisibility",  "Invisibility",    false },
    { "tiny_bond",     "Tiny Bond",       false },
    { "golden_gun",    "Golden gun",      false },
    { "magnum",        "Magnum",          false },
    { "laser",         "Laser",           false },
    { "turbo_mode",    "Turbo mode",      false },
    { "10x_health",    "10x health",      false },
    { "2x_armor",      "2x armour",       false },
    { "extra_weapons", "Extra weapons",   false },
    { "fast_animation","Fast animation",  false },
};
const int kCheatCount = (int)(sizeof kCheats / sizeof kCheats[0]);

const char *kRulesets[] = { "classic", "hardcore", "survival", "chaos", "horde" };
const int   kRulesetCount = 5;

/* Matches GE_LUA_MAX_MODS in ge_lua.c. Listing more here than the loader will accept would
 * offer mods that silently never load. */
#define GE_MAX_MODS 32

/* The bindable actions and the pad sources, taken from ge_actions.h.
 *
 * These used to be two hand-written string tables here, with a comment explaining that
 * the launcher "never links the input layer". That was not true -- ge_launcher.cpp is
 * compiled into the same binary as ge_bindings.c -- and the duplication was already
 * stale in practice: the six actions and eleven sources listed here had to be edited in
 * lockstep with port_os.c, ge_config.c and two launcher UIs, with nothing to catch a
 * list that had only been updated in three of the four.
 *
 * Generating them from GE_ACTION_LIST / GE_SOURCE_LIST removes the possibility. The
 * DEFAULT is likewise no longer a repeated string: it comes from gePresetSource(), so
 * the UI shows what the selected preset will actually do rather than what somebody
 * wrote down once.
 */
struct BindAction { const char *label; const char *key; };
const BindAction kActions[] = {
#define M(id, lo, up) { NULL, up },
    GE_ACTION_LIST(M)
#undef M
};
const int kActionCount = (int)(sizeof kActions / sizeof kActions[0]);

/* Display names. A switch rather than a table so a missing case is a compile-time
 * -Wswitch rather than a blank row, and so the ordinal coupling that made the old
 * parallel arrays fragile does not come back through the side door. */
static const char *ActionLabel(int a)
{
    switch (a) {
        case GE_ACT_FIRE:        return "Fire";
        case GE_ACT_AIM:         return "Aim";
        case GE_ACT_USE:         return "Use / interact";
        case GE_ACT_RELOAD:      return "Reload";
        case GE_ACT_CROUCH:      return "Crouch";
        case GE_ACT_WEAPON_NEXT: return "Next weapon";
        case GE_ACT_WEAPON_PREV: return "Prev weapon";
        case GE_ACT_PAUSE:       return "Pause";
        default:                 return "?";
    }
}

static const char *AxisLabel(int x)
{
    switch (x) {
        case GE_AXIS_FORWARD:      return "Forward";
        case GE_AXIS_BACKWARD:     return "Back";
        case GE_AXIS_STRAFE_LEFT:  return "Strafe left";
        case GE_AXIS_STRAFE_RIGHT: return "Strafe right";
        case GE_AXIS_LOOK_UP:      return "Look up";
        case GE_AXIS_LOOK_DOWN:    return "Look down";
        case GE_AXIS_LOOK_LEFT:    return "Look left";
        case GE_AXIS_LOOK_RIGHT:   return "Look right";
        default:                   return "?";
    }
}

/* Positional, matching GE_SOURCE_LIST. Names are what the player types in
 * goldeneye.cfg, so they are shown verbatim rather than prettified -- "lt" here and
 * "lt" in the file is the whole point. */
const char *kSources[] = {
#define M(id, lo) lo,
    GE_SOURCE_LIST(M)
#undef M
};
const int   kSourceCount = (int)(sizeof kSources / sizeof kSources[0]);

struct Model {
    /* profile */
    /* 0 = Base Game, 1 = GoldenEye+. Base Game is enforced at launch as well as
     * shown here. The legacy config-file graphics preset keeps its own semantics. */
    int  profile;

    /* Brutal preferences remain selected while Base Game suppresses their effects. */
    bool base_game;
    int gibs;
    int blood;
    int blood_limit;

    /* ruleset */
    int  ruleset;             /* index into kRulesets */
    bool rs_custom;           /* show and send the nine individual percentages */
    int  enemy_health, enemy_damage, enemy_accuracy, enemy_reaction;
    int  player_health, player_armour, ammo, explosion_damage, turret_damage;

    /* co-op */
    int  coop_players;        /* 0 = off, otherwise 2-4 sharing a solo mission */
    bool coop_ff;             /* friendly fire; off is the co-op default */
    int  coop_respawn;        /* seconds before a dead player returns, 0 = button only */

    /* horde */
    bool horde;
    int  horde_per_kill, horde_per_kill_cap, horde_max_alive;
    int  horde_wave_kills, horde_growth;

    /* level */
    bool pick_stage;
    int  stage_idx;

    /* video */
    int  supersample, fov, framerate, msaa, aniso;
    bool uncapped;                /* GETV_FPS=0 plus the real clock; see the TIMING section */
    int  filtering;               /* 0 nearest, 1 bilinear, 2 three-point (configFiltering) */
    bool widescreen;              /* fill the real window instead of a 4:3 pillarbox (configWidescreen) */
    bool mipmaps;                 /* trilinear filtering on minification (GETV_MIPMAPS) */
    bool hd_textures;             /* check texpack for overrides before each N64 texture (configHDTextures) */
    bool parallax;                /* let a pack's height maps displace the diffuse UVs (GETV_PARALLAX) */
    /* Held as a percentage rather than the float the gate wants, because the slider needs an
     * int and one representation is better than two that can drift. 100 is retail. */
    int   crosshair_scale_pct;
    char texpack[256];            /* GETV_TEXPACK: override pack directory */
    float crosshair_color[3];     /* 0..1 RGB; GETV_CROSSHAIR_COLOR wants RRGGBB hex */
    bool fullscreen;
    char resolution[64];

    /* FXAA only. The CRT terms used to live here too, behind their own page; they moved to
     * mods/crt_screen/mod.lua when the CRT became a mod. Two controls for one effect is worse
     * than either alone -- the mod calls ge.postfx() at load and would have silently won over
     * anything set here -- and the tuning is more useful in the mod, where editing it is the
     * worked example of how a mod changes something. FXAA stays: it is an image-quality
     * setting like MSAA, not a look. */
    bool fxaa;

    /* mods, discovered by scanning rather than typed. `found` is what is on disk now; `on`
     * is per-entry and parallel to it. */
    int  mod_count;
    char mod_name[GE_MAX_MODS][64];
    bool mod_on[GE_MAX_MODS];
    char mod_scanned[512];        /* the directory `found` was filled from, for the UI */

    /* Bindings. -1 means "not set": for bind_all that is port_os.c's default, for bind_p it
     * is "inherit whatever bind_all resolved to". Holding the unset state rather than
     * resolving it on load is what lets the UI show three distinct things -- an explicit
     * choice, an inherited one, and the built-in default -- and what stops the launcher
     * pinning all 24 keys the first time anyone opens the page. */
    int  bind_all[GE_ACT_MAX];
    int  bind_p[4][GE_ACT_MAX];
    int  bind_tab;                /* 0 = all players, 1..4 = that player */

    /* Which set of defaults everything falls back to. 0 = modern, 1 = n64; see
     * GE_PRESET_* in ge_actions.h, which this deliberately matches so the two can be
     * used interchangeably. */
    int  preset;

    /* Keyboard and mouse bindings, as comma-separated SDL scancode names plus this
     * port's mouse1..mouse5/wheelup/wheeldown. Empty means "whatever the preset says",
     * which is the same tri-state the pad bindings use and for the same reason: the
     * launcher must not pin seventeen keys the first time anyone opens the page.
     *
     * Strings rather than a code, because a binding can be a LIST -- "C,Left Ctrl" --
     * and because a name round-trips into goldeneye.cfg without a second table. */
    char keybind[GE_ACT_MAX][96];
    char keyaxis[GE_AXIS_MAX][96];

    /* Hold or toggle, 0 and 1, matching GE_HOLD / GE_TOGGLE. */
    int  aim_mode;
    int  crouch_mode;

    /* The port's dedicated crouch/stand keys at all. Off leaves only the retail gesture
     * (hold aim, push down). */
    bool crouch_key;

    /* Mouse and keyboard. Both default ON in port_input.c, which is the right default -- a
     * gamepad is the minority case and the majority should not have to find a setting before
     * the game is playable. Held as tri-state is unnecessary here: the launcher only needs to
     * write the gate when it differs from that default. */
    bool mouse;
    int  mouse_mode;              /* 0: modern displacement, 1: classic N64 stick */
    int  mouse_sens;              /* percent, port_input.c clamps 1..1000, default 100 */
    bool mouse_invert;
    bool keyboard;

    /* Netplay. gePortNetInit (ge_net_udp.c) checks GETV_NET_HOST before GETV_NET_JOIN, so a
     * launcher writing both independently could silently end up hosting when the user meant to
     * join. mode holds the three-way choice explicitly instead of two checkboxes that could
     * both end up set: 0 off, 1 host, 2 join. */
    int  net_mode;
    char net_port[8];             /* host mode: the port to bind */
    char net_join[128];           /* join mode: host:port */
    int  net_players;             /* host mode only; ge_net_udp.c clamps 2..4, default 2 */
    int  net_delay;               /* ticks; 0 leaves GE_NET_DEFAULT_DELAY alone */

    /* misc */
    bool cheat_on[kCheatCount];
    bool dev_overlay;
    bool record_telemetry;
    int record_frames;
    int input_debug;
    bool pace_trace;
    char console_key[32];
    char moddir[512];
};

/* Where the game will actually look for mods, resolved the same way ge_lua.c resolves it:
 * GETV_MODDIR if set, otherwise "mods". ge_lua.c opens that path relative to the process's
 * working directory, so the launcher has to try the same relative path AND the directory the
 * binary sits in -- started from a shortcut or from Explorer those are not the same place,
 * and a launcher that scanned only one of them would show an empty list for a mods folder the
 * game is about to load happily. */
bool mod_dir_resolve(const Model &m, char *out, size_t n)
{
    const char *want = (m.moddir[0] != '\0') ? m.moddir : "mods";
    struct stat st;

    /* An absolute path is taken as given. */
    if (want[0] == '/' || want[0] == '\\' ||
        (want[0] != '\0' && want[1] == ':')) {
        snprintf(out, n, "%s", want);
        return stat(out, &st) == 0 && S_ISDIR(st.st_mode);
    }

    snprintf(out, n, "%s", want);
    if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return true;

    char exe[4096];
    if (self_path(exe, sizeof exe)) {
        char *fw = strrchr(exe, '/');
        char *bw = strrchr(exe, '\\');
        char *cut = (bw && (!fw || bw > fw)) ? bw : fw;
        if (cut) {
            *cut = '\0';
            snprintf(out, n, "%s/%s", exe, want);
            if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return true;
        }
    }
    snprintf(out, n, "%s", want);
    return false;
}

/* Scan for mods. A directory is a mod when it contains a mod.lua -- the same test ge_lua.c
 * applies, so the list shown here is exactly the list the game would load. Preserves the
 * enabled state of anything already known, so a rescan after adding a folder does not reset
 * the user's choices. */
void mod_scan(Model &m)
{
    bool  prev_on[GE_MAX_MODS];
    char  prev_name[GE_MAX_MODS][64];
    int   prev_count = m.mod_count;
    for (int i = 0; i < prev_count && i < GE_MAX_MODS; i++) {
        prev_on[i] = m.mod_on[i];
        snprintf(prev_name[i], sizeof prev_name[i], "%s", m.mod_name[i]);
    }

    m.mod_count = 0;
    mod_dir_resolve(m, m.mod_scanned, sizeof m.mod_scanned);

    DIR *d = opendir(m.mod_scanned);
    if (d == NULL) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && m.mod_count < GE_MAX_MODS) {
        if (e->d_name[0] == '.') continue;

        char sub[1024];
        struct stat st;
        snprintf(sub, sizeof sub, "%s/%s", m.mod_scanned, e->d_name);
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        snprintf(sub, sizeof sub, "%s/%s/mod.lua", m.mod_scanned, e->d_name);
        if (stat(sub, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        int k = m.mod_count++;
        snprintf(m.mod_name[k], sizeof m.mod_name[k], "%s", e->d_name);
        m.mod_on[k] = true;                       /* new mods default on, per wiki/Lua-mods.md */
        for (int j = 0; j < prev_count; j++) {
            if (strcmp(prev_name[j], m.mod_name[k]) == 0) { m.mod_on[k] = prev_on[j]; break; }
        }
    }
    closedir(d);
}

/* Apply GETV_MODS_OFF to a freshly scanned list. */
void mod_apply_off(Model &m)
{
    const char *list = getenv("GETV_MODS_OFF");
    if (list == NULL || *list == '\0') return;

    for (int i = 0; i < m.mod_count; i++) {
        size_t n = strlen(m.mod_name[i]);
        const char *p = list;
        while (*p != '\0') {
            while (*p == ',' || *p == ' ' || *p == '\t') p++;
            const char *end = p;
            while (*end != '\0' && *end != ',') end++;
            size_t len = (size_t)(end - p);
            while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
            if (len == n && strncmp(p, m.mod_name[i], n) == 0) { m.mod_on[i] = false; break; }
            p = (*end == ',') ? end + 1 : end;
        }
    }
}

/* Index of `v` in kSources, or -1 for NULL, empty or anything unrecognised. */
int bind_index(const char *v)
{
    if (v == NULL || *v == '\0') return -1;
    for (int i = 0; i < kSourceCount; i++) {
        if (strcmp(v, kSources[i]) == 0) return i;
    }
    return -1;
}

void apply_profile(Model &m);

void model_load(Model &m)
{
    memset(&m, 0, sizeof m);

    m.profile = env_bool("GETV_PROFILE_PLUS", false) ? 1 : 0;
    const char *base = getenv("GETV_BASE_GAME");
    m.base_game = !base || (strcmp(base, "0") && strcmp(base, "off") &&
                           strcmp(base, "false") && strcmp(base, "no"));
    /* Read preferences directly: querying runtime policy here would cache it before
     * the player has finished choosing settings. */
    {
        const char *gibs = getenv("GETV_GIBS");
        m.gibs = 0;
        if (gibs) {
            if (!strcmp(gibs, "1") || !strcmp(gibs, "on") || !strcmp(gibs, "true") ||
                !strcmp(gibs, "yes") || !strcmp(gibs, "explosion") || !strcmp(gibs, "explosions"))
                m.gibs = 1;
            else if (!strcmp(gibs, "high_damage") || !strcmp(gibs, "high-damage") ||
                     !strcmp(gibs, "highdamage")) m.gibs = 2;
            else if (!strcmp(gibs, "always")) m.gibs = 3;
        }
        const char *blood = getenv("GETV_BLOOD");
        m.blood = !blood || !strcmp(blood, "enhanced") ? 1 :
                  !strcmp(blood, "excessive") ? 2 : 0;
        const char *limit = getenv("GETV_BLOOD_LIMIT");
        char *end;
        long n = 128;
        if (limit) {
            ge_errno = 0;
            n = strtol(limit, &end, 10);
            if (end == limit || *end != '\0' || ge_errno == ERANGE) n = 128;
        }
        m.blood_limit = n < 16 ? 16 : n > 512 ? 512 : (int)n;
    }


    {
        const char *rs = getenv("GETV_RULESET");
        m.ruleset = 0;
        if (rs && *rs) {
            for (int i = 0; i < kRulesetCount; i++) {
                if (strcmp(rs, kRulesets[i]) == 0) { m.ruleset = i; break; }
            }
        }
    }
    m.enemy_health     = env_int("GETV_RS_ENEMY_HEALTH",     100);
    m.enemy_damage     = env_int("GETV_RS_ENEMY_DAMAGE",     100);
    m.enemy_accuracy   = env_int("GETV_RS_ENEMY_ACCURACY",   100);
    m.enemy_reaction   = env_int("GETV_RS_ENEMY_REACTION",   100);
    m.player_health    = env_int("GETV_RS_PLAYER_HEALTH",    100);
    m.player_armour    = env_int("GETV_RS_PLAYER_ARMOUR",    100);
    m.ammo             = env_int("GETV_RS_AMMO",             100);
    m.explosion_damage = env_int("GETV_RS_EXPLOSION_DAMAGE", 100);
    m.turret_damage    = env_int("GETV_RS_TURRET_DAMAGE",    100);
    m.rs_custom = (m.enemy_health != 100 || m.enemy_damage != 100 ||
                   m.enemy_accuracy != 100 || m.enemy_reaction != 100 ||
                   m.player_health != 100 || m.player_armour != 100 ||
                   m.ammo != 100 || m.explosion_damage != 100 || m.turret_damage != 100);

    m.horde              = env_bool("GETV_HORDE", false);
    m.coop_players       = env_int("GETV_COOP", 0);
    m.coop_ff            = env_int("GETV_COOP_FRIENDLYFIRE", 0) != 0;
    m.coop_respawn       = env_int("GETV_COOP_RESPAWN", 5);

    m.horde_per_kill     = env_int("GETV_HORDE_PER_KILL", 1);
    m.horde_per_kill_cap = env_int("GETV_HORDE_PER_KILL_CAP", 3);
    m.horde_max_alive    = env_int("GETV_HORDE_MAX_ALIVE", 12);
    m.horde_wave_kills   = env_int("GETV_HORDE_WAVE_KILLS", 10);
    m.horde_growth       = env_int("GETV_HORDE_GROWTH", 1);

    {
        /* Keep an explicit startup choice separate from the selected mission. A
         * fresh launch starts at the title screen unless a stage was requested. */
        m.pick_stage = env_bool("GETV_PICKSTAGE", getenv("GETV_STAGE") != NULL);
        m.stage_idx = 0;
        const char *st = getenv("GETV_STAGE");
        if (st != NULL && *st != '\0') {
            int id = atoi(st);
            for (int i = 0; i < kStageCount; i++) {
                if (kStages[i].id == id) { m.stage_idx = i; break; }
            }
        }
    }

    m.supersample = env_int("GETV_SUPERSAMPLE", 1);
    m.fov         = env_int("GETV_FOV", 100);
    {
        const int fps = env_int("GETV_FPS", 60);
        m.uncapped  = (fps == 0);
        m.framerate = m.uncapped ? 60 : fps;   /* keep a sane value under the disabled slider */
    }
    m.msaa        = env_int("GETV_MSAA", 0);
    m.aniso       = env_int("GETV_ANISO", 0);
    /* 2 (three-point), not 0, matches configFiltering's own compiled-in default
     * (port_support.c) -- what the N64 actually did. */
    m.filtering   = env_int("GETV_FILTERING", 2);
    if (m.filtering < 0 || m.filtering > 2) { m.filtering = 2; }
    /* Default true, matching configWidescreen's own compiled-in default (port_support.c):
     * the 4:3 pillarbox was never a deliberate choice, just what a non-4:3 window did with
     * nothing to correct for it. */
    m.widescreen  = env_bool("GETV_WIDESCREEN", true);
    m.mipmaps     = env_bool("GETV_MIPMAPS", false);
    /* Default false, matching configHDTextures' own compiled-in default (port_support.c) --
     * unlike widescreen/filtering above, this path has had no compiler available to verify
     * it against, so it stays opt-in rather than presenting itself as a finished feature. */
    m.hd_textures = env_bool("GETV_HD_TEXTURES", false);
    m.parallax    = env_bool("GETV_PARALLAX", true);
    {
        char sc[32];
        env_str("GETV_CROSSHAIR_SCALE", sc, sizeof sc, "1.0");
        m.crosshair_scale_pct = (int) (atof(sc) * 100.0 + 0.5);
        if (m.crosshair_scale_pct < 25 || m.crosshair_scale_pct > 200) {
            m.crosshair_scale_pct = 100;
        }
    }
    env_str("GETV_TEXPACK", m.texpack, sizeof m.texpack, "hdtextures");
    /* GETV_CROSSHAIR_COLOR is RRGGBB hex (ge_config.c, port_support.c); ImGui::ColorEdit3
     * wants 0..1 floats, so this is the one setting that round-trips through a different
     * representation than its own variable, in both directions. Default FFFFFF is retail's
     * own hardcoded white, so an unset variable changes nothing. */
    {
        char hex[8];
        unsigned r = 255, g = 255, b = 255;
        env_str("GETV_CROSSHAIR_COLOR", hex, sizeof hex, "FFFFFF");
        sscanf(hex, "%2x%2x%2x", &r, &g, &b);
        m.crosshair_color[0] = r / 255.0f;
        m.crosshair_color[1] = g / 255.0f;
        m.crosshair_color[2] = b / 255.0f;
    }
    m.fullscreen  = env_bool("GETV_FULLSCREEN", false);
    env_str("GETV_WINDOW", m.resolution, sizeof m.resolution, "1280x960");

    m.fxaa = env_bool("GETV_FXAA", false);

    {
        const char *c = getenv("GETV_CHEATS");
        if (c && *c) {
            for (int i = 0; i < kCheatCount; i++) {
                const char *p = strstr(c, kCheats[i].name);
                if (p) m.cheat_on[i] = true;
            }
        }
    }
    /* Bindings. Unrecognised spellings are left as "not set" rather than guessed at: the
     * launcher must not silently rewrite a key it did not understand into something else. */
    {
        /* GETV_LAUNCHER_BINDTAB=<0..4> opens the Controls page on that scope. Same reason as
         * GETV_LAUNCHER_PAGE: the probe never clicks, so the per-player view is otherwise
         * unreachable without a human driving the mouse. */
        m.bind_tab = env_int("GETV_LAUNCHER_BINDTAB", 0);
        if (m.bind_tab < 0 || m.bind_tab > 4) m.bind_tab = 0;
        for (int a = 0; a < kActionCount; a++) {
            char key[64];
            snprintf(key, sizeof key, "GETV_BIND_%s", kActions[a].key);
            m.bind_all[a] = bind_index(getenv(key));
            for (int p = 0; p < 4; p++) {
                snprintf(key, sizeof key, "GETV_P%d_BIND_%s", p + 1, kActions[a].key);
                m.bind_p[p][a] = bind_index(getenv(key));
            }
        }

        /* Preset first: every default shown on the page is read out of it. */
        {
            const char *pv = getenv("GETV_INPUT_PRESET");
            m.preset = (pv && (strcmp(pv, "n64") == 0 || strcmp(pv, "classic") == 0))
                       ? GE_PRESET_N64 : GE_PRESET_MODERN;
        }

        /* Keyboard bindings. Copied verbatim, including a spelling this build does not
         * recognise: port_input.c is the authority on what SDL accepts, and a launcher
         * that quietly dropped an unknown name would silently unbind a key that worked
         * a moment ago. Empty stays empty, meaning "inherit the preset". */
        for (int a = 0; a < kActionCount; a++) {
            char key[64];
            const char *v;
            snprintf(key, sizeof key, "GETV_KEY_%s", kActions[a].key);
            v = getenv(key);
            snprintf(m.keybind[a], sizeof m.keybind[a], "%s", v ? v : "");
        }
        for (int x = 0; x < GE_AXIS_MAX; x++) {
            char key[64];
            const char *v;
            snprintf(key, sizeof key, "GETV_KEY_%s", geAxisEnvSuffix(x));
            v = getenv(key);
            snprintf(m.keyaxis[x], sizeof m.keyaxis[x], "%s", v ? v : "");
        }

        /* GETV_AIM_MODE is the new spelling; GETV_AIM_TOGGLE is what shipped and still
         * decides when the new one is absent, so an existing config opens showing what
         * it actually does. */
        {
            const char *am = getenv("GETV_AIM_MODE");
            if (am && *am) {
                m.aim_mode = (strcmp(am, "toggle") == 0) ? GE_TOGGLE : GE_HOLD;
            } else {
                m.aim_mode = env_bool("GETV_AIM_TOGGLE", false) ? GE_TOGGLE : GE_HOLD;
            }
        }
        {
            const char *cm = getenv("GETV_CROUCH_MODE");
            m.crouch_mode = (cm && strcmp(cm, "toggle") == 0) ? GE_TOGGLE : GE_HOLD;
        }
        m.crouch_key = env_bool("GETV_CROUCH_KEY", true);
    }

    /* Defaults mirror port_input.c: mouse and keyboard both ON, sensitivity 100%. */
    m.mouse        = env_bool("GETV_MOUSE", true);
    m.mouse_mode   = getenv("GETV_MOUSE_MODE") &&
                     strcmp(getenv("GETV_MOUSE_MODE"), "classic") == 0;
    m.mouse_sens   = env_int("GETV_MOUSE_SENS", 100);
    m.mouse_invert = env_bool("GETV_MOUSE_INVERT", false);
    m.keyboard     = env_bool("GETV_KEYBOARD", true);

    /* mode is derived, not stored -- the two raw vars are the source of truth (gePortNetInit
     * reads them directly), so deriving on load is what keeps this page from drifting out of
     * sync with a session started by hand outside the launcher. */
    {
        const char *host = getenv("GETV_NET_HOST");
        const char *join = getenv("GETV_NET_JOIN");
        m.net_mode = (host && *host) ? 1 : (join && *join) ? 2 : 0;
    }
    env_str("GETV_NET_HOST", m.net_port, sizeof m.net_port, "7777");
    env_str("GETV_NET_JOIN", m.net_join, sizeof m.net_join, "");
    m.net_players = env_int("GETV_NET_PLAYERS", 2);
    m.net_delay   = env_int("GETV_NET_DELAY", 0);

    m.dev_overlay = env_bool("GETV_IMGUI", false);
    m.record_telemetry = env_bool("GETV_DEV_RECORD", false);
    m.record_frames = geDeveloperRecordingFrames(env_int("GETV_DEV_RECORD_FRAMES", 10800));
    m.input_debug = env_int("GETV_INPUT_DEBUG", 0);
    if (m.input_debug < 0 || m.input_debug > 2) m.input_debug = 0;
    m.pace_trace = env_bool("GETV_PACETRACE", false);
    env_str("GETV_CONSOLE_KEY", m.console_key, sizeof m.console_key, "grave");
    env_str("GETV_MODDIR", m.moddir, sizeof m.moddir, "");
    /* After moddir is known: the scan needs it to decide where to look. */
    mod_scan(m);
    mod_apply_off(m);
    if (m.profile == 0) {
        const bool uncapped = m.uncapped;
        apply_profile(m);
        m.uncapped = uncapped; // Opening or starting must retain the selected timing.
    }
}

void model_store(const Model &chosen)
{
    Model m = chosen;
    if (m.profile == 0) {
        const bool uncapped = m.uncapped;
        apply_profile(m);
        m.uncapped = uncapped; // Opening or starting must retain the selected timing.
    }
    setenv("GETV_LAUNCHER_BASE", m.profile == 0 ? "1" : "0", 1);
    setenv("GETV_PROFILE_PLUS", m.profile ? "1" : "0", 1);
    static const char *const gibs[] = { "off", "explosions", "high_damage", "always" };
    static const char *const blood[] = { "original", "enhanced", "excessive" };
    setenv("GETV_BASE_GAME", m.base_game ? "1" : "0", 1);
    put_str("GETV_GIBS", gibs[m.gibs]);
    put_str("GETV_BLOOD", blood[m.blood]);
    put_int("GETV_BLOOD_LIMIT", m.blood_limit);

    put_str("GETV_RULESET", kRulesets[m.ruleset]);

    /* The nine percentages are sent only when custom is on. Sending them unconditionally
     * would pin every value at whatever the preset happened to be when the window opened,
     * which would make the preset list inert the moment anyone touched it once. */
    static const char *kRsKeys[9] = {
        "GETV_RS_ENEMY_HEALTH", "GETV_RS_ENEMY_DAMAGE", "GETV_RS_ENEMY_ACCURACY",
        "GETV_RS_ENEMY_REACTION", "GETV_RS_PLAYER_HEALTH", "GETV_RS_PLAYER_ARMOUR",
        "GETV_RS_AMMO", "GETV_RS_EXPLOSION_DAMAGE", "GETV_RS_TURRET_DAMAGE"
    };
    const int vals[9] = {
        m.enemy_health, m.enemy_damage, m.enemy_accuracy, m.enemy_reaction,
        m.player_health, m.player_armour, m.ammo, m.explosion_damage, m.turret_damage
    };
    for (int i = 0; i < 9; i++) {
        if (m.rs_custom) put_int(kRsKeys[i], vals[i]);
        else             unsetenv(kRsKeys[i]);
    }

    setenv("GETV_HORDE", m.horde ? "1" : "0", 1);
    if (m.horde) {
    /* GETV_PADS follows the player count: co-op needs a controller port per player, and a
     * player with no port never gets input. Left alone when co-op is off. */
    if (m.coop_players >= 2) {
        put_int("GETV_COOP", m.coop_players);
        put_int("GETV_PADS", m.coop_players);
        setenv("GETV_COOP_FRIENDLYFIRE", m.coop_ff ? "1" : "0", 1);
        put_int("GETV_COOP_RESPAWN", m.coop_respawn);
    } else {
        unsetenv("GETV_COOP");
        unsetenv("GETV_COOP_FRIENDLYFIRE");
        unsetenv("GETV_COOP_RESPAWN");
    }

        put_int("GETV_HORDE_PER_KILL",     m.horde_per_kill);
        put_int("GETV_HORDE_PER_KILL_CAP", m.horde_per_kill_cap);
        put_int("GETV_HORDE_MAX_ALIVE",    m.horde_max_alive);
        put_int("GETV_HORDE_WAVE_KILLS",   m.horde_wave_kills);
        put_int("GETV_HORDE_GROWTH",       m.horde_growth);
    }

    setenv("GETV_PICKSTAGE", m.pick_stage ? "1" : "0", 1);
    if (m.pick_stage) put_int("GETV_STAGE", kStages[m.stage_idx].id);
    else              unsetenv("GETV_STAGE");

    put_int("GETV_SUPERSAMPLE", m.supersample);
    put_int("GETV_FOV",         m.fov);
    /* Uncapped has to set the clock too, and the launcher is the one place that has to do it
     * by hand. ge_config.c's key_framerate pairs `framerate = off` with GETV_REALCLOCK=1,
     * but this writes GETV_FPS straight to the environment and never passes through that
     * layer, so the pairing would simply not happen. Uncapped on the synthetic clock is the
     * worst configuration available here -- measured at 811.9 fields a second against the
     * correct 60, a game running thirteen times too fast -- so the two belong together
     * wherever either is set. */
    if (m.uncapped) {
        put_int("GETV_FPS", 0);
        setenv("GETV_REALCLOCK", "1", 1);
    } else {
        put_int("GETV_FPS", m.framerate);
        unsetenv("GETV_REALCLOCK");
    }
    put_int("GETV_MSAA",        m.msaa);
    put_int("GETV_ANISO",       m.aniso);
    put_int("GETV_FILTERING",   m.filtering);
    setenv("GETV_WIDESCREEN", m.widescreen ? "1" : "0", 1);
    setenv("GETV_MIPMAPS", m.mipmaps ? "1" : "0", 1);
    setenv("GETV_HD_TEXTURES", m.hd_textures ? "1" : "0", 1);
    setenv("GETV_PARALLAX",    m.parallax    ? "1" : "0", 1);
    {
        char sc[32];
        snprintf(sc, sizeof sc, "%.2f", (double) m.crosshair_scale_pct / 100.0);
        put_str("GETV_CROSSHAIR_SCALE", sc);
    }
    put_str("GETV_TEXPACK", m.texpack);
    {
        char hex[8];
        snprintf(hex, sizeof hex, "%02X%02X%02X",
                 (unsigned) (m.crosshair_color[0] * 255.0f + 0.5f),
                 (unsigned) (m.crosshair_color[1] * 255.0f + 0.5f),
                 (unsigned) (m.crosshair_color[2] * 255.0f + 0.5f));
        put_str("GETV_CROSSHAIR_COLOR", hex);
    }
    setenv("GETV_FULLSCREEN", m.fullscreen ? "1" : "0", 1);
    put_str("GETV_WINDOW", m.resolution);

    setenv("GETV_FXAA", m.fxaa ? "1" : "0", 1);
    /* GETV_CRT and the four GETV_CRT_* terms are deliberately NOT written here. The CRT is
     * mods/crt_screen now, and the launcher turns it on and off through the mod list like any
     * other mod. The gates still exist for anyone driving the game from a shell -- ge_postfx.c
     * reads them -- they simply are not this window's to set. */

    {
        char list[512];
        list[0] = '\0';
        for (int i = 0; i < kCheatCount; i++) {
            if (!m.cheat_on[i]) continue;
            if (list[0]) strncat(list, ",", sizeof list - strlen(list) - 1);
            strncat(list, kCheats[i].name, sizeof list - strlen(list) - 1);
        }
        put_str("GETV_CHEATS", list);
    }

    /* Only what was actually chosen is written; everything else is unset. Writing all 24 keys
     * would freeze today's defaults into the environment, so a later change to port_os.c's
     * defaults would never reach anyone who had opened this page once. Same reasoning as the
     * nine ruleset percentages. */
    for (int a = 0; a < kActionCount; a++) {
        char key[64];
        snprintf(key, sizeof key, "GETV_BIND_%s", kActions[a].key);
        if (m.bind_all[a] >= 0) setenv(key, kSources[m.bind_all[a]], 1);
        else                    unsetenv(key);

        for (int p = 0; p < 4; p++) {
            snprintf(key, sizeof key, "GETV_P%d_BIND_%s", p + 1, kActions[a].key);
            if (m.bind_p[p][a] >= 0) setenv(key, kSources[m.bind_p[p][a]], 1);
            else                     unsetenv(key);
        }

        /* Keyboard, same rule: an empty box means "inherit the preset" and must leave
         * the gate unset, not write today's default string into it. */
        snprintf(key, sizeof key, "GETV_KEY_%s", kActions[a].key);
        if (m.keybind[a][0]) setenv(key, m.keybind[a], 1);
        else                 unsetenv(key);
    }
    for (int x = 0; x < GE_AXIS_MAX; x++) {
        char key[64];
        snprintf(key, sizeof key, "GETV_KEY_%s", geAxisEnvSuffix(x));
        if (m.keyaxis[x][0]) setenv(key, m.keyaxis[x], 1);
        else                 unsetenv(key);
    }

    setenv("GETV_INPUT_PRESET", (m.preset == GE_PRESET_N64) ? "n64" : "modern", 1);
    setenv("GETV_CROUCH_MODE",  (m.crouch_mode == GE_TOGGLE) ? "toggle" : "hold", 1);
    /* Aim toggle is the ENGINE's own per-player aim-control option, reached through the
     * gate the 0015 patch reads. Both spellings are written: the new one because that is
     * what the config file and ge_bindings.c use, and the old one because the patch
     * itself still tests it. */
    setenv("GETV_AIM_MODE",   (m.aim_mode == GE_TOGGLE) ? "toggle" : "hold", 1);
    setenv("GETV_AIM_TOGGLE", (m.aim_mode == GE_TOGGLE) ? "1" : "0", 1);
    setenv("GETV_CROUCH_KEY", m.crouch_key ? "1" : "0", 1);

    setenv("GETV_MOUSE",        m.mouse        ? "1" : "0", 1);
    setenv("GETV_MOUSE_MODE", m.mouse_mode ? "classic" : "modern", 1);
    setenv("GETV_MOUSE_INVERT", m.mouse_invert ? "1" : "0", 1);
    setenv("GETV_KEYBOARD",     m.keyboard     ? "1" : "0", 1);
    /* Sensitivity only when it differs from the default, so a later change to port_input.c's
     * 100 reaches anyone who never touched the slider. Same reasoning as the ruleset
     * percentages and the binding table. */
    if (m.mouse_sens != 100) put_int("GETV_MOUSE_SENS", m.mouse_sens);
    else                     unsetenv("GETV_MOUSE_SENS");

    if (m.net_mode == 1) {
        put_str("GETV_NET_HOST", m.net_port);
        unsetenv("GETV_NET_JOIN");
        put_int("GETV_NET_PLAYERS", m.net_players);
    } else if (m.net_mode == 2) {
        put_str("GETV_NET_JOIN", m.net_join);
        unsetenv("GETV_NET_HOST");
        unsetenv("GETV_NET_PLAYERS");    /* host-only; ge_net_udp.c ignores it when joining */
    } else {
        unsetenv("GETV_NET_HOST");
        unsetenv("GETV_NET_JOIN");
        unsetenv("GETV_NET_PLAYERS");
    }
    if (m.net_mode != 0 && m.net_delay != 0) put_int("GETV_NET_DELAY", m.net_delay);
    else                                     unsetenv("GETV_NET_DELAY");

    setenv("GETV_IMGUI", m.dev_overlay ? "1" : "0", 1);
    put_int("GETV_INPUT_DEBUG", m.input_debug);
    setenv("GETV_PACETRACE", m.pace_trace ? "1" : "0", 1);
    if (strcmp(m.console_key, "grave") == 0 || m.console_key[0] == '\0')
        unsetenv("GETV_CONSOLE_KEY");
    else
        put_str("GETV_CONSOLE_KEY", m.console_key);
    put_str("GETV_MODDIR", m.moddir);

    /* Only the mods that were switched OFF are recorded; see the denylist note in ge_lua.c.
     * A mod added to the folder later is on by default, which is the documented behaviour and
     * the reason this is not an allowlist. */
    {
        char off[1024];
        off[0] = '\0';
        for (int i = 0; i < m.mod_count; i++) {
            if (m.mod_on[i]) continue;
            if (off[0]) strncat(off, ",", sizeof off - strlen(off) - 1);
            strncat(off, m.mod_name[i], sizeof off - strlen(off) - 1);
        }
        put_str("GETV_MODS_OFF", off);
    }
    geConfigApplyLauncherProfile();
}

/* Keep the visible model consistent with the launch contract. Base Game restores
 * original presentation and gameplay; input and selected frame rate survive.
 * geConfigApplyLauncherProfile also enforces it after the child re-reads config.
 * GoldenEye+ supplies enhancement defaults and allows subsequent customization. */
void apply_profile(Model &m)
{
    if (m.profile == 1) {
        m.fov         = (m.fov < 100) ? 100 : m.fov;
        m.msaa        = (m.msaa  < 4) ? 4 : m.msaa;
        m.aniso       = (m.aniso < 8) ? 8 : m.aniso;
        m.mipmaps     = true;
        m.supersample = (m.supersample < 2) ? 2 : m.supersample;

        /* Costs nothing without a pack installed. ge_texpack_dir() treats a missing
         * directory as the normal case and stays silent, so this is "use a pack if one is
         * there" rather than a dependency on one being there. */
        m.hd_textures = true;

        /* Only does anything with a pack that ships height maps, and there is none in the
         * game's own assets. It is here so the same installed pack means different things
         * when enabled: a pack can supply resolution and displacement together. */
        m.parallax = true;

        /* A 32-pixel sight was sized for 320x240 on a CRT across a room; at a desk it covers
         * noticeably more of what you are aiming at than it did in 1997. Applied after the
         * aspect corrections in gunDrawSight, so the shape is unchanged and only the size
         * moves. 1.0 is retail exactly and stays the default outside this profile. */
        m.crosshair_scale_pct = 60;

        /* Edge antialiasing over the finished frame. An image-quality setting like MSAA
         * rather than a look, which is why it belongs to the profile and the CRT terms do
         * not: those moved to mods/crt_screen. */
        m.fxaa = true;

        /* Uncapped, which model_store() pairs with the real clock. Without that pairing the
         * synthetic counter advances one video field per rendered frame by construction and
         * the world runs as fast as the renderer draws: measured at 811.9 fields a second
         * against the correct 60, a game running thirteen times too fast.
         *
         * What this delivers depends on vsync, which stays ON by default and is not part of
         * the profile. Uncapped against vsync means "as fast as the display", so a 120 Hz
         * panel gets 120 with correct game speed and no tearing. GETV_VSYNC=0 releases it
         * entirely; measured on DAM that is 449 fps at 60.8 fields a second. Making tearing
         * the default for everyone in exchange for frames nobody's monitor can show would be
         * the wrong trade. */
        m.uncapped = true;
    } else {
        m.msaa = 0;
        m.aniso = 0;
        m.mipmaps = false;
        m.supersample = 1;
        m.fov = 100;
        m.hd_textures = false;
        m.fxaa = false;
        m.uncapped = false;
        m.parallax = false;
        m.crosshair_scale_pct = 100;
        m.ruleset = 0;
        m.rs_custom = false;
        m.horde = false;
        m.base_game = true;
        m.coop_players = 0;
        m.net_mode = 0;
        m.filtering = 2;
        m.widescreen = false;
        for (int c = 0; c < 3; ++c) m.crosshair_color[c] = 1.0f;
        for (int i = 0; i < kCheatCount; ++i) m.cheat_on[i] = false;
    }
}

/* SDL owns the per-user reports directory; nothing is saved beside game assets. */
static char *developer_reports_dir(void)
{
    return SDL_GetPrefPath("goldeneyenative", "developer-tools");
}

static bool developer_prepare_recording(const Model &m)
{
    if (!m.record_telemetry) return true;
    char *dir = developer_reports_dir();
    if (!dir) return false;
    char id[128], path[4096];
    snprintf(id, sizeof id, "run-%llu-%llu", (unsigned long long)time(NULL),
             (unsigned long long)SDL_GetPerformanceCounter());
    int n = snprintf(path, sizeof path, "%s%s.jsonl", dir, id);
    SDL_free(dir);
    if (n < 0 || (size_t)n >= sizeof path) return false;
    geDeveloperStartRecording(m.record_frames, id, path);
    return true;
}

/* Percent-encode local paths rather than treating spaces, # or % as URL syntax. */
static bool developer_open_path(const char *path)
{
    char url[12296];
    size_t n = 0;
#ifdef _WIN32
    const char *prefix = "file:///";
#else
    const char *prefix = "file://";
#endif
    n = strlen(prefix);
    memcpy(url, prefix, n);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        unsigned char c = *p;
#ifdef _WIN32
        if (c == '\\') c = '/';
#endif
        if (n + 4 >= sizeof url) return false;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '-' || c == '_' || c == '.')
            url[n++] = (char)c;
        else { snprintf(url + n, 4, "%%%02X", c); n += 3; }
    }
    url[n] = '\0';
    return SDL_OpenURL(url) == 0;
}

static bool developer_open_reports(bool latest)
{
    char *dir = developer_reports_dir();
    if (!dir) return false;
    char path[4096];
    snprintf(path, sizeof path, "%s", dir);
    bool found = !latest;
    if (latest) {
#ifdef _WIN32
        wchar_t pattern[4096];
        char search[4096];
        int n = snprintf(search, sizeof search, "%srun-*.jsonl.txt", dir);
        if (n > 0 && (size_t)n < sizeof search &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, search, -1, pattern, 4096)) {
            WIN32_FIND_DATAW entry;
            HANDLE d = FindFirstFileW(pattern, &entry);
            FILETIME newest = { 0, 0 };
            if (d != INVALID_HANDLE_VALUE) {
                do {
                    if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                    char name[4096], candidate[4096];
                    if (!WideCharToMultiByte(CP_UTF8, 0, entry.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
                    n = snprintf(candidate, sizeof candidate, "%s%s", dir, name);
                    if (n > 0 && (size_t)n < sizeof candidate &&
                        (!found || CompareFileTime(&entry.ftLastWriteTime, &newest) >= 0)) {
                        newest = entry.ftLastWriteTime;
                        snprintf(path, sizeof path, "%s", candidate);
                        found = true;
                    }
                } while (FindNextFileW(d, &entry));
                FindClose(d);
            }
        }
#else
        DIR *d = opendir(dir);
        struct dirent *entry;
        time_t newest = 0;
        if (d) {
            while ((entry = readdir(d)) != NULL) {
                size_t len = strlen(entry->d_name);
                if (strncmp(entry->d_name, "run-", 4) != 0 || len < 10 ||
                    strcmp(entry->d_name + len - 10, ".jsonl.txt") != 0) continue;
                char candidate[4096];
                struct stat st;
                int n = snprintf(candidate, sizeof candidate, "%s%s", dir, entry->d_name);
                if (n > 0 && (size_t)n < sizeof candidate && lstat(candidate, &st) == 0 &&
                    S_ISREG(st.st_mode) && (!found || st.st_mtime >= newest)) {
                    newest = st.st_mtime;
                    snprintf(path, sizeof path, "%s", candidate);
                    found = true;
                }
            }
            closedir(d);
        }
#endif
    }
    SDL_free(dir);
    return found && developer_open_path(path);
}

/* ---------------------------------------------------------------- Swift bridge
 *
 * GeNativeLauncher.swift (tvOS/iOS's launcher UI, see its own header comment for why a
 * native UI replaced this file's ImGui one on those two platforms) needs read/write
 * access to the exact same Model this file's UI code below edits, so a setting changed
 * from the native UI takes effect exactly the way this UI's own "Start Mission" already
 * does -- through model_store()'s existing env-var writes, which geConfigInit() (already
 * called once per boot, before either launcher runs) reads back before the game itself
 * starts. A single static instance, not a struct handed across the bridge: the
 * enumerable tables (stages/rulesets/cheats) are read-only globals already, so exposing
 * them by index here cannot let a Swift-side copy drift out of sync -- there is only one
 * copy of any of this data, ever.
 *
 * Declared in GeLauncherBridge.h, which both this file and Swift (via
 * SWIFT_OBJC_BRIDGING_HEADER) include -- unlike gePortNativeLauncherRun()'s @_cdecl
 * (Swift exporting a C symbol, needing no header on the C side), this is the other
 * direction: C++ exporting symbols for SWIFT TO CALL, which Xcode only picks up through
 * an explicit bridging header. */
/* ---- persisting the control settings --------------------------------------
 *
 * The launcher applies settings by setenv() and then re-exec'ing the game, which is
 * enough for them to take effect and not enough for them to survive quitting. That was
 * tolerable while every setting was a graphics preference somebody would set once from
 * a shell; it is not tolerable for a rebound key, which is the sort of thing a player
 * does once and expects to still be there next week.
 *
 * Only the CONTROLS page is written. The rest of the launcher deliberately keeps its
 * env-var-and-relaunch model -- writing every page would mean deciding what a config
 * file should say about, say, a one-off mission selection, and that is a larger design
 * question than this change. What goes in is exactly what the controls page can change.
 *
 * Empty values are passed through as empty on purpose: geConfigSave() comments the key
 * out, which is how "back to the preset default" is represented in the file. Writing
 * the current default instead would freeze it, so a later change to the preset would
 * never reach anyone who had opened this page once -- the same rule model_store()
 * already follows for the environment.
 */
static void controls_save_to_config(const Model &m)
{
    /* Sized for every action and axis twice over plus the scalars, so no bound is ever
     * reached at runtime. The arrays are parallel; `n` indexes both. */
    static char keybuf[(GE_ACT_MAX * 2 + GE_AXIS_MAX + 8)][64];
    static char valbuf[(GE_ACT_MAX * 2 + GE_AXIS_MAX + 8)][96];
    const char *keys[(GE_ACT_MAX * 2 + GE_AXIS_MAX + 8)];
    const char *vals[(GE_ACT_MAX * 2 + GE_AXIS_MAX + 8)];
    int n = 0;

    #define PUSH(k, v)                                                     \
        do {                                                               \
            snprintf(keybuf[n], sizeof keybuf[n], "%s", (k));              \
            snprintf(valbuf[n], sizeof valbuf[n], "%s", (v));              \
            keys[n] = keybuf[n];                                           \
            vals[n] = valbuf[n];                                           \
            n++;                                                           \
        } while (0)

    PUSH("input_preset", (m.preset == GE_PRESET_N64) ? "n64" : "modern");
    PUSH("aim_mode",     (m.aim_mode == GE_TOGGLE) ? "toggle" : "hold");
    PUSH("crouch_mode",  (m.crouch_mode == GE_TOGGLE) ? "toggle" : "hold");
    PUSH("crouch_key",   m.crouch_key ? "1" : "0");
    PUSH("mouse_mode",   m.mouse_mode ? "classic" : "modern");

    for (int a = 0; a < kActionCount; a++) {
        char k[64];
        /* The config spelling of the action, which is the env suffix lowercased --
         * geActionName() gives it directly rather than requiring a second table. */
        snprintf(k, sizeof k, "%s", geActionName(a));
        PUSH(k, (m.bind_all[a] >= 0) ? kSources[m.bind_all[a]] : "");

        snprintf(k, sizeof k, "key.%s", geActionName(a));
        PUSH(k, m.keybind[a]);
    }
    for (int x = 0; x < GE_AXIS_MAX; x++) {
        char k[64];
        snprintf(k, sizeof k, "key.%s", geAxisName(x));
        PUSH(k, m.keyaxis[x]);
    }
    #undef PUSH

    /* Per-player pad bindings are NOT written. They are a split-screen setting that the
     * page presents as a tab, and saving all four scopes would put twenty-seven mostly
     * empty lines in the file every time anyone pressed Save. They keep working through
     * the environment and can be set by hand as p2.fire = rb. */

    (void) geConfigSave(keys, vals, n);
}

#include "GeLauncherBridge.h"

static Model g_bridgeModel;

extern "C" {

void geBridgeLoad(void) { model_load(g_bridgeModel); }
void geBridgeSave(void) { model_store(g_bridgeModel); }
/* Writes the CONTROLS page to goldeneye.cfg. Separate from geBridgeSave() because
 * applying settings and persisting them are different acts with different blast radii:
 * one lasts until you quit, the other edits a file the player also edits by hand. */
void geBridgeSaveControls(void) { controls_save_to_config(g_bridgeModel); }
const char *geBridgeConfigPath(void) { return geConfigPath(); }

int geBridgeStageCount(void) { return kStageCount; }
const char *geBridgeStageName(int i)  { return (i >= 0 && i < kStageCount) ? kStages[i].name  : ""; }
const char *geBridgeStagePlace(int i) { return (i >= 0 && i < kStageCount) ? kStages[i].place : ""; }
int geBridgeStageMission(int i)       { return (i >= 0 && i < kStageCount) ? kStages[i].mission : 0; }
int geBridgeStageMpOnly(int i)        { return (i >= 0 && i < kStageCount) ? (kStages[i].mp_only ? 1 : 0) : 0; }

int geBridgeRulesetCount(void) { return kRulesetCount; }
const char *geBridgeRulesetName(int i) { return (i >= 0 && i < kRulesetCount) ? kRulesets[i] : ""; }

int geBridgeCheatCount(void) { return kCheatCount; }
const char *geBridgeCheatLabel(int i) { return (i >= 0 && i < kCheatCount) ? kCheats[i].label : ""; }
int geBridgeCheatLive(int i)          { return (i >= 0 && i < kCheatCount) ? (kCheats[i].live ? 1 : 0) : 0; }

int  geBridgeGetPickStage(void)   { return g_bridgeModel.pick_stage ? 1 : 0; }
void geBridgeSetPickStage(int v)  { g_bridgeModel.pick_stage = (v != 0); }
int  geBridgeGetStageIdx(void)    { return g_bridgeModel.stage_idx; }
void geBridgeSetStageIdx(int v)   { if (v >= 0 && v < kStageCount) g_bridgeModel.stage_idx = v; }

int  geBridgeGetProfile(void)     { return g_bridgeModel.profile; }
void geBridgeSetProfile(int v)    { g_bridgeModel.profile = v; apply_profile(g_bridgeModel); }

int  geBridgeGetRuleset(void)     { return g_bridgeModel.ruleset; }
void geBridgeSetRuleset(int v)    { if (v >= 0 && v < kRulesetCount) g_bridgeModel.ruleset = v; }

int  geBridgeGetBaseGame(void)       { return g_bridgeModel.base_game ? 1 : 0; }
void geBridgeSetBaseGame(int v)      { g_bridgeModel.base_game = (v != 0); }
int  geBridgeGetGibs(void)           { return g_bridgeModel.gibs; }
void geBridgeSetGibs(int v)          { if (v >= 0 && v <= 3) g_bridgeModel.gibs = v; }
int  geBridgeGetBlood(void)          { return g_bridgeModel.blood; }
void geBridgeSetBlood(int v)         { if (v >= 0 && v <= 2) g_bridgeModel.blood = v; }
int  geBridgeGetBloodLimit(void)     { return g_bridgeModel.blood_limit; }
void geBridgeSetBloodLimit(int v)    { if (v >= 16 && v <= 512) g_bridgeModel.blood_limit = v; }

int  geBridgeGetHorde(void)             { return g_bridgeModel.horde ? 1 : 0; }
void geBridgeSetHorde(int v)            { g_bridgeModel.horde = (v != 0); }
int  geBridgeGetHordePerKill(void)      { return g_bridgeModel.horde_per_kill; }
void geBridgeSetHordePerKill(int v)     { g_bridgeModel.horde_per_kill = v; }
int  geBridgeGetHordePerKillCap(void)   { return g_bridgeModel.horde_per_kill_cap; }
void geBridgeSetHordePerKillCap(int v)  { g_bridgeModel.horde_per_kill_cap = v; }
int  geBridgeGetHordeMaxAlive(void)     { return g_bridgeModel.horde_max_alive; }
void geBridgeSetHordeMaxAlive(int v)    { g_bridgeModel.horde_max_alive = v; }
int  geBridgeGetHordeWaveKills(void)    { return g_bridgeModel.horde_wave_kills; }
void geBridgeSetHordeWaveKills(int v)   { g_bridgeModel.horde_wave_kills = v; }
int  geBridgeGetHordeGrowth(void)       { return g_bridgeModel.horde_growth; }
void geBridgeSetHordeGrowth(int v)      { g_bridgeModel.horde_growth = v; }

int  geBridgeGetSupersample(void) { return g_bridgeModel.supersample; }
void geBridgeSetSupersample(int v){ g_bridgeModel.supersample = v; }
int  geBridgeGetFov(void)         { return g_bridgeModel.fov; }
void geBridgeSetFov(int v)        { g_bridgeModel.fov = v; }
int  geBridgeGetFramerate(void)   { return g_bridgeModel.framerate; }
void geBridgeSetFramerate(int v)  { g_bridgeModel.framerate = v; }
int  geBridgeGetMsaa(void)        { return g_bridgeModel.msaa; }
void geBridgeSetMsaa(int v)       { g_bridgeModel.msaa = v; }
int  geBridgeGetFxaa(void)        { return g_bridgeModel.fxaa ? 1 : 0; }
void geBridgeSetFxaa(int v)       { g_bridgeModel.fxaa = (v != 0); }
int  geBridgeGetHdTextures(void)  { return g_bridgeModel.hd_textures ? 1 : 0; }
void geBridgeSetHdTextures(int v) { g_bridgeModel.hd_textures = (v != 0); }
const char *geBridgeGetTexpackPath(void) { return g_bridgeModel.texpack; }
void geBridgeSetTexpackPath(const char *path) {
    if (path == NULL) return;
    snprintf(g_bridgeModel.texpack, sizeof g_bridgeModel.texpack, "%s", path);
}

int  geBridgeGetAniso(void)       { return g_bridgeModel.aniso; }
void geBridgeSetAniso(int v)      { g_bridgeModel.aniso = v; }
int  geBridgeGetFiltering(void)   { return g_bridgeModel.filtering; }
void geBridgeSetFiltering(int v)  { if (v >= 0 && v <= 2) g_bridgeModel.filtering = v; }
int  geBridgeGetWidescreen(void)  { return g_bridgeModel.widescreen ? 1 : 0; }
void geBridgeSetWidescreen(int v) { g_bridgeModel.widescreen = (v != 0); }
int  geBridgeGetMipmaps(void)     { return g_bridgeModel.mipmaps ? 1 : 0; }
void geBridgeSetMipmaps(int v)    { g_bridgeModel.mipmaps = (v != 0); }
int  geBridgeGetParallax(void)    { return g_bridgeModel.parallax ? 1 : 0; }
void geBridgeSetParallax(int v)   { g_bridgeModel.parallax = (v != 0); }
int  geBridgeGetCrosshairScalePct(void)  { return g_bridgeModel.crosshair_scale_pct; }
void geBridgeSetCrosshairScalePct(int v) { g_bridgeModel.crosshair_scale_pct = v; }
float geBridgeGetCrosshairR(void) { return g_bridgeModel.crosshair_color[0]; }
float geBridgeGetCrosshairG(void) { return g_bridgeModel.crosshair_color[1]; }
float geBridgeGetCrosshairB(void) { return g_bridgeModel.crosshair_color[2]; }
void  geBridgeSetCrosshairColor(float r, float g, float b) {
    g_bridgeModel.crosshair_color[0] = r;
    g_bridgeModel.crosshair_color[1] = g;
    g_bridgeModel.crosshair_color[2] = b;
}
int  geBridgeGetFullscreen(void)  { return g_bridgeModel.fullscreen ? 1 : 0; }
void geBridgeSetFullscreen(int v) { g_bridgeModel.fullscreen = (v != 0); }
const char *geBridgeGetResolution(void) { return g_bridgeModel.resolution; }
void geBridgeSetResolution(const char *wh) {
    if (wh == NULL) return;
    snprintf(g_bridgeModel.resolution, sizeof g_bridgeModel.resolution, "%s", wh);
}
int  geBridgeGetUncapped(void)    { return g_bridgeModel.uncapped ? 1 : 0; }
void geBridgeSetUncapped(int v)   { g_bridgeModel.uncapped = (v != 0); }
int  geBridgeGetDevOverlay(void)  { return g_bridgeModel.dev_overlay ? 1 : 0; }
void geBridgeSetDevOverlay(int v) { g_bridgeModel.dev_overlay = (v != 0); }

int  geBridgeGetRsCustom(void)    { return g_bridgeModel.rs_custom ? 1 : 0; }
void geBridgeSetRsCustom(int v)   { g_bridgeModel.rs_custom = (v != 0); }
int  geBridgeGetEnemyHealth(void)      { return g_bridgeModel.enemy_health; }
void geBridgeSetEnemyHealth(int v)     { g_bridgeModel.enemy_health = v; }
int  geBridgeGetEnemyDamage(void)      { return g_bridgeModel.enemy_damage; }
void geBridgeSetEnemyDamage(int v)     { g_bridgeModel.enemy_damage = v; }
int  geBridgeGetEnemyAccuracy(void)    { return g_bridgeModel.enemy_accuracy; }
void geBridgeSetEnemyAccuracy(int v)   { g_bridgeModel.enemy_accuracy = v; }
int  geBridgeGetEnemyReaction(void)    { return g_bridgeModel.enemy_reaction; }
void geBridgeSetEnemyReaction(int v)   { g_bridgeModel.enemy_reaction = v; }
int  geBridgeGetPlayerHealth(void)     { return g_bridgeModel.player_health; }
void geBridgeSetPlayerHealth(int v)    { g_bridgeModel.player_health = v; }
int  geBridgeGetPlayerArmour(void)     { return g_bridgeModel.player_armour; }
void geBridgeSetPlayerArmour(int v)    { g_bridgeModel.player_armour = v; }
int  geBridgeGetAmmoPct(void)          { return g_bridgeModel.ammo; }
void geBridgeSetAmmoPct(int v)         { g_bridgeModel.ammo = v; }
int  geBridgeGetExplosionDamage(void)  { return g_bridgeModel.explosion_damage; }
void geBridgeSetExplosionDamage(int v) { g_bridgeModel.explosion_damage = v; }
int  geBridgeGetTurretDamage(void)     { return g_bridgeModel.turret_damage; }
void geBridgeSetTurretDamage(int v)    { g_bridgeModel.turret_damage = v; }

int  geBridgeGetMouse(void)        { return g_bridgeModel.mouse ? 1 : 0; }
void geBridgeSetMouse(int v)       { g_bridgeModel.mouse = (v != 0); }
int  geBridgeGetMouseSens(void)    { return g_bridgeModel.mouse_sens; }
void geBridgeSetMouseSens(int v)   { g_bridgeModel.mouse_sens = v; }
int  geBridgeGetMouseInvert(void)  { return g_bridgeModel.mouse_invert ? 1 : 0; }
void geBridgeSetMouseInvert(int v) { g_bridgeModel.mouse_invert = (v != 0); }
int  geBridgeGetKeyboard(void)     { return g_bridgeModel.keyboard ? 1 : 0; }
void geBridgeSetKeyboard(int v)    { g_bridgeModel.keyboard = (v != 0); }

const char *geBridgeGetModDir(void) { return g_bridgeModel.moddir; }
void geBridgeSetModDir(const char *dir) {
    if (dir == NULL) return;
    snprintf(g_bridgeModel.moddir, sizeof g_bridgeModel.moddir, "%s", dir);
}
void geBridgeRescanMods(void) { mod_scan(g_bridgeModel); }

int geBridgeActionCount(void) { return kActionCount; }
const char *geBridgeActionLabel(int i)   { return (i >= 0 && i < kActionCount) ? ActionLabel(i) : ""; }
/* The default now depends on the selected preset rather than being a fixed string, so
 * the SwiftUI page shows what picking N64 will actually do instead of what modern does. */
const char *geBridgeActionDefault(int i)
{
    if (i < 0 || i >= kActionCount) return "";
    return kSources[gePresetSource(g_bridgeModel.preset, i)];
}

/* ---- keyboard bindings, preset and modes, for the SwiftUI launcher ---------
 *
 * Same shared model as the ImGui page (g_bridgeModel), so the two UIs cannot drift
 * apart in what they mean by a setting -- only in how they draw it. */
int  geBridgeAxisCount(void)          { return GE_AXIS_MAX; }
const char *geBridgeAxisLabel(int i)  { return (i >= 0 && i < GE_AXIS_MAX) ? AxisLabel(i) : ""; }

const char *geBridgeGetKeyBind(int action)
{
    return (action >= 0 && action < kActionCount) ? g_bridgeModel.keybind[action] : "";
}
void geBridgeSetKeyBind(int action, const char *v)
{
    if (action < 0 || action >= kActionCount) return;
    snprintf(g_bridgeModel.keybind[action], sizeof g_bridgeModel.keybind[action],
             "%s", (v != NULL) ? v : "");
}
const char *geBridgeGetKeyBindDefault(int action)
{
    return (action >= 0 && action < kActionCount)
        ? gePresetKeys(g_bridgeModel.preset, action) : "";
}

const char *geBridgeGetAxisBind(int axis)
{
    return (axis >= 0 && axis < GE_AXIS_MAX) ? g_bridgeModel.keyaxis[axis] : "";
}
void geBridgeSetAxisBind(int axis, const char *v)
{
    if (axis < 0 || axis >= GE_AXIS_MAX) return;
    snprintf(g_bridgeModel.keyaxis[axis], sizeof g_bridgeModel.keyaxis[axis],
             "%s", (v != NULL) ? v : "");
}
const char *geBridgeGetAxisBindDefault(int axis)
{
    return (axis >= 0 && axis < GE_AXIS_MAX)
        ? gePresetAxisKeys(g_bridgeModel.preset, axis) : "";
}

int  geBridgeGetInputPreset(void) { return g_bridgeModel.preset; }
void geBridgeSetInputPreset(int v)
{
    if (v < 0 || v >= GE_PRESET_MAX || v == g_bridgeModel.preset) return;
    g_bridgeModel.preset = v;
    /* Clearing every explicit choice is the same rule the ImGui page applies, and for
     * the same reason: the value of a preset is that picking it describes the layout
     * completely, which a leftover binding from the other one would break. */
    for (int a = 0; a < kActionCount; a++) {
        g_bridgeModel.keybind[a][0] = '\0';
        g_bridgeModel.bind_all[a] = -1;
        for (int p = 0; p < 4; p++) g_bridgeModel.bind_p[p][a] = -1;
    }
    for (int x = 0; x < GE_AXIS_MAX; x++) g_bridgeModel.keyaxis[x][0] = '\0';
}

int  geBridgeGetAimMode(void)     { return g_bridgeModel.aim_mode; }
void geBridgeSetAimMode(int v)    { g_bridgeModel.aim_mode = (v == GE_TOGGLE) ? GE_TOGGLE : GE_HOLD; }
int  geBridgeGetCrouchMode(void)  { return g_bridgeModel.crouch_mode; }
void geBridgeSetCrouchMode(int v) { g_bridgeModel.crouch_mode = (v == GE_TOGGLE) ? GE_TOGGLE : GE_HOLD; }
int  geBridgeGetCrouchKey(void)   { return g_bridgeModel.crouch_key ? 1 : 0; }
void geBridgeSetCrouchKey(int v)  { g_bridgeModel.crouch_key = (v != 0); }
int geBridgeSourceCount(void) { return kSourceCount; }
const char *geBridgeSourceName(int i) { return (i >= 0 && i < kSourceCount) ? kSources[i] : ""; }

int  geBridgeGetBindTab(void)  { return g_bridgeModel.bind_tab; }
void geBridgeSetBindTab(int v) { if (v >= 0 && v <= 4) g_bridgeModel.bind_tab = v; }
int  geBridgeGetBindAll(int action) {
    return (action >= 0 && action < kActionCount) ? g_bridgeModel.bind_all[action] : -1;
}
void geBridgeSetBindAll(int action, int src) {
    if (action >= 0 && action < kActionCount) g_bridgeModel.bind_all[action] = src;
}
int  geBridgeGetBindP(int player, int action) {
    return (player >= 0 && player < 4 && action >= 0 && action < kActionCount)
        ? g_bridgeModel.bind_p[player][action] : -1;
}
void geBridgeSetBindP(int player, int action, int src) {
    if (player >= 0 && player < 4 && action >= 0 && action < kActionCount)
        g_bridgeModel.bind_p[player][action] = src;
}
void geBridgeResetBindTab(void) {
    for (int a = 0; a < kActionCount; a++) {
        if (g_bridgeModel.bind_tab == 0) g_bridgeModel.bind_all[a] = -1;
        else g_bridgeModel.bind_p[g_bridgeModel.bind_tab - 1][a] = -1;
    }
}

int geBridgeModCount(void) { return g_bridgeModel.mod_count; }
const char *geBridgeModName(int i) {
    return (i >= 0 && i < g_bridgeModel.mod_count) ? g_bridgeModel.mod_name[i] : "";
}
int  geBridgeGetModOn(int i) {
    return (i >= 0 && i < g_bridgeModel.mod_count) ? (g_bridgeModel.mod_on[i] ? 1 : 0) : 0;
}
void geBridgeSetModOn(int i, int on) {
    if (i >= 0 && i < g_bridgeModel.mod_count) g_bridgeModel.mod_on[i] = (on != 0);
}

int  geBridgeGetCheatOn(int i) {
    return (i >= 0 && i < kCheatCount) ? (g_bridgeModel.cheat_on[i] ? 1 : 0) : 0;
}
void geBridgeSetCheatOn(int i, int on) {
    if (i >= 0 && i < kCheatCount) g_bridgeModel.cheat_on[i] = (on != 0);
}

} // extern "C"

/* ---------------------------------------------------------------- exec
 *
 * argv[0] is not reliable -- it is whatever the caller passed and may be relative to a
 * directory the process has since left -- so the real path is asked of the OS. */
bool self_path(char *out, size_t n)
{
#if defined(_WIN32)
    DWORD r = GetModuleFileNameA(NULL, out, (DWORD) n);
    return r > 0 && r < n;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t) n;
    return _NSGetExecutablePath(out, &sz) == 0;
#elif defined(__linux__)
    ssize_t r = readlink("/proc/self/exe", out, n - 1);
    if (r <= 0) return false;
    out[r] = '\0';
    return true;
#else
    (void) out; (void) n;
    return false;
#endif
}

int  g_argc;
char **g_argv;

void relaunch()
{
    char exe[4096];

    if (!self_path(exe, sizeof exe)) {
        printf("[getv][launcher] cannot determine own path; not relaunching\n");
        return;
    }

#if defined(__APPLE__) && defined(GE_PLATFORM_MAC)
    if (geAppRendererActive()) {
        int renderer = geAppRendererSelected();
        const char *target = geAppRendererPath(renderer);
        if (!geAppRendererAvailable(renderer) || !target || strlen(target) >= sizeof exe) {
            fprintf(stderr, "[getv][launcher] selected renderer is unavailable\n");
            exit(1); // The app monitor offers recovery; never fall through into another backend.
        }
        snprintf(exe, sizeof exe, "%s", target);
    }
#endif

    /* The environment must stop asking for the launcher too, or a GETV_LAUNCHER=1 set in
     * goldeneye.cfg would survive into the child and open the launcher again, forever. On
     * Windows, 0 must remain present: an absent setting plus a plain argv is intentionally
     * interpreted as a double-click and opens the launcher by default. */
#if defined(_WIN32)
    setenv("GETV_LAUNCHER", "0", 1);
#else
    if (geAppRendererActive()) setenv("GETV_LAUNCHER", "0", 1);
    else unsetenv("GETV_LAUNCHER");
#endif
    unsetenv("GETV_LAUNCHER_AUTOPLAY");

    /* Rebuild argv without --launcher, or the new process opens the launcher again. */
    char **nv = (char **) calloc((size_t) g_argc + 1, sizeof(char *));
    if (nv == NULL) return;
    int n = 0;
    nv[n++] = exe;
    for (int i = 1; i < g_argc; i++) {
        if (strcmp(g_argv[i], "--launcher") == 0) continue;
        nv[n++] = g_argv[i];
    }
    nv[n] = NULL;

    fflush(stdout);
    /* Windows has no execve that replaces the image in place: _execv starts a new process
     * and ends this one, so the shell sees the child rather than the launcher. That is the
     * behaviour wanted here anyway -- the launcher's job is finished -- but it is worth
     * naming, because the parent's exit is not observable to the child the way a real exec
     * would be, and a caller waiting on the original PID will see it return early. */
#if defined(_WIN32)
    _execv(exe, nv);
#elif defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH)
    /* execv() is a hard compile error on tvOS/watchOS (marked __TVOS_PROHIBITED /
     * __WATCHOS_PROHIBITED in unistd.h) -- the sandbox does not allow a process to replace
     * its own image. This whole function is only reachable here through the
     * GETV_LAUNCHER_AUTOPLAY path (gePortLauncherRun() declines to exec under RAPI_METAL
     * everywhere else), and that path's own comment already establishes that falling through
     * without relaunching is correct: the model was written by model_store() above, so the
     * game just continues in this same process instead of a fresh one. */
    printf("[getv][launcher] not relaunching: execv is unavailable on this platform; "
           "continuing in this process\n");
    free(nv);
    return;
#else
    execv(exe, nv);
#endif

#if defined(__APPLE__) && defined(GE_PLATFORM_MAC)
    if (geAppRendererActive()) {
        fprintf(stderr, "[getv][launcher] renderer exec failed: %s\n", strerror(ge_errno));
        free(nv);
        exit(1);
    }
#endif

    /* Only reached if execv failed. The environment is already set, so falling through into
     * the game is still correct -- it just keeps the launcher's process rather than replacing
     * it, and every gate is still unread at this point because nothing has started yet. */
    printf("[getv][launcher] execv failed (%s); continuing in this process\n", strerror(ge_errno));
    free(nv);
}

/* ---------------------------------------------------------------- look
 *
 * The launcher is the first thing anyone sees, and until now it was stock ImGui: grey
 * rounded boxes and the built-in ProggyClean bitmap font. The font is most of why it read
 * as a debug tool -- a 13px bitmap face at one weight cannot express hierarchy, so every
 * line had equal emphasis and the eye had nowhere to land.
 *
 * What is copied from the game is its menu grammar, not a screenshot: black ground, hard
 * right angles with no rounding anywhere, thin rules, and a single gold accent that means
 * "this one" and is never used for decoration. Hierarchy comes from size, letterspacing and
 * colour rather than from weight, which is both closer to the original's lettering and the
 * only option available -- see the font note below.
 */

/* Roboto Condensed, SIL OFL 1.1, bundled at port/assets/fonts with its OFL.txt. Condensed
 * because the labels here are long ("Enemy reaction", "Multiplayer only") and a condensed
 * face fits them at a readable size without truncation.
 *
 * It is the VARIABLE font, and ImGui's stb_truetype has no variable-axis support: it
 * rasterises the default master, which for this family is Regular. There is therefore no
 * bold available at all, and asking for one silently gets Regular back. That is why every
 * heading below is distinguished by size, colour and letterspacing instead -- not a
 * stylistic preference, a constraint of the rasteriser. */
ImFont *g_fTitle = NULL, *g_fH = NULL, *g_fBody = NULL, *g_fSmall = NULL;

const ImU32 kBg     = IM_COL32(  8,   9,  11, 255);
const ImU32 kPanel  = IM_COL32( 14,  16,  20, 255);
const ImU32 kPanel2 = IM_COL32( 21,  24,  30, 255);
const ImU32 kLine   = IM_COL32( 38,  42,  50, 255);
const ImU32 kGold   = IM_COL32(198, 160,  46, 255);
const ImU32 kGoldHi = IM_COL32(242, 208,  96, 255);
const ImU32 kText   = IM_COL32(223, 221, 214, 255);
const ImU32 kDim    = IM_COL32(128, 135, 145, 255);
const ImU32 kWarn   = IM_COL32(214, 132,  46, 255);

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImU32 mix(ImU32 a, ImU32 b, float t)
{
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

void ge_load_fonts()
{
    ImGuiIO &io = ImGui::GetIO();
    char exe[4096], dir[4096], path[4096];

    dir[0] = '\0';
    if (self_path(exe, sizeof exe)) {
        snprintf(dir, sizeof dir, "%s", exe);
        char *fw = strrchr(dir, '/');
        char *bw = strrchr(dir, '\\');
        char *cut = (bw && (!fw || bw > fw)) ? bw : fw;
        if (cut) *cut = '\0'; else dir[0] = '\0';
    }

    /* Next to the binary first -- that is where the build script copies it, and it is the
     * only location that is right for an installed copy. The source-tree paths after it are
     * for running the exe straight out of build-windows/ during development. */
    static const char *kRel[] = {
        "assets/fonts/RobotoCondensed-VF.ttf",
        "../port/assets/fonts/RobotoCondensed-VF.ttf",
        "port/assets/fonts/RobotoCondensed-VF.ttf",
        "getv/port/assets/fonts/RobotoCondensed-VF.ttf",
    };

    for (int i = 0; i < (int)(sizeof kRel / sizeof kRel[0]); i++) {
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0) {
                if (!dir[0]) continue;
                snprintf(path, sizeof path, "%s/%s", dir, kRel[i]);
            } else {
                snprintf(path, sizeof path, "%s", kRel[i]);
            }
            FILE *fp = fopen(path, "rb");
            if (!fp) continue;
            fclose(fp);

            g_fBody  = io.Fonts->AddFontFromFileTTF(path, 18.0f);
            g_fSmall = io.Fonts->AddFontFromFileTTF(path, 14.0f);
            g_fH     = io.Fonts->AddFontFromFileTTF(path, 25.0f);
            g_fTitle = io.Fonts->AddFontFromFileTTF(path, 46.0f);
            if (g_fBody && g_fSmall && g_fH && g_fTitle) {
                printf("[getv][launcher] font: %s\n", path);
                io.FontDefault = g_fBody;
                return;
            }
        }
    }

    /* Not fatal. A missing font must not stop the game starting, so fall back and say so --
     * silently rendering in ProggyClean would look like the redesign had not landed. */
    printf("[getv][launcher] bundled font not found; falling back to the built-in bitmap font\n");
    g_fBody = g_fSmall = g_fH = g_fTitle = io.Fonts->AddFontDefault();
    io.FontDefault = g_fBody;
}

void ge_apply_style()
{
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();

    /* Nothing is rounded. The original's menus are drawn with hard rectangles and so is
     * this; a single rounded corner anywhere reads as a different product. */
    s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.0f;
    s.PopupRounding  = s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize  = 1.0f;
    s.FrameBorderSize  = 1.0f;
    s.PopupBorderSize  = 1.0f;
    s.WindowPadding    = ImVec2(0, 0);
    s.FramePadding     = ImVec2(10, 7);
    s.ItemSpacing      = ImVec2(10, 10);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.ScrollbarSize    = 12.0f;
    s.GrabMinSize      = 12.0f;

    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]            = v4(kBg);
    c[ImGuiCol_ChildBg]             = v4(kPanel);
    c[ImGuiCol_PopupBg]             = v4(kPanel2);
    c[ImGuiCol_Border]              = v4(kLine);
    c[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text]                = v4(kText);
    c[ImGuiCol_TextDisabled]        = v4(kDim);
    c[ImGuiCol_FrameBg]             = v4(kPanel2);
    c[ImGuiCol_FrameBgHovered]      = v4(mix(kPanel2, kGold, 0.18f));
    c[ImGuiCol_FrameBgActive]       = v4(mix(kPanel2, kGold, 0.28f));
    c[ImGuiCol_Button]              = v4(kPanel2);
    c[ImGuiCol_ButtonHovered]       = v4(mix(kPanel2, kGold, 0.25f));
    c[ImGuiCol_ButtonActive]        = v4(mix(kPanel2, kGold, 0.40f));
    c[ImGuiCol_Header]              = v4(mix(kPanel2, kGold, 0.22f));
    c[ImGuiCol_HeaderHovered]       = v4(mix(kPanel2, kGold, 0.32f));
    c[ImGuiCol_HeaderActive]        = v4(mix(kPanel2, kGold, 0.42f));
    c[ImGuiCol_CheckMark]           = v4(kGoldHi);
    c[ImGuiCol_SliderGrab]          = v4(kGold);
    c[ImGuiCol_SliderGrabActive]    = v4(kGoldHi);
    c[ImGuiCol_ScrollbarBg]         = v4(kBg);
    c[ImGuiCol_ScrollbarGrab]       = v4(kLine);
    c[ImGuiCol_ScrollbarGrabHovered]= v4(mix(kLine, kGold, 0.35f));
    c[ImGuiCol_ScrollbarGrabActive] = v4(kGold);
    c[ImGuiCol_Separator]           = v4(kLine);
    c[ImGuiCol_ResizeGrip]          = ImVec4(0, 0, 0, 0);
}

/* Letterspaced text, drawn a glyph at a time. ImGui has no tracking control, and tracking
 * is most of what makes small caps read as a title rather than as a label -- the original's
 * headings are widely spaced and lose their character without it. */
void TextLS(ImFont *f, float sz, ImVec2 p, ImU32 col, const char *s, float sp)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (const char *c = s; *c; c++) {
        dl->AddText(f, sz, p, col, c, c + 1);
        p.x += f->CalcTextSizeA(sz, FLT_MAX, 0.0f, c, c + 1).x + sp;
    }
}

float TextLSWidth(ImFont *f, float sz, const char *s, float sp)
{
    float w = 0.0f;
    for (const char *c = s; *c; c++) w += f->CalcTextSizeA(sz, FLT_MAX, 0.0f, c, c + 1).x + sp;
    return (w > 0.0f) ? w - sp : 0.0f;
}

/* A gold small-caps heading with a rule running out to the right margin. */
void Section(const char *title)
{
    ImGui::Dummy(ImVec2(0, 4));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  w = ImGui::GetContentRegionAvail().x;
    TextLS(g_fSmall, 13.0f, p, kGold, title, 2.4f);
    float tw = TextLSWidth(g_fSmall, 13.0f, title, 2.4f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + tw + 14, p.y + 8),
                                        ImVec2(p.x + w, p.y + 8), kLine, 1.0f);
    ImGui::Dummy(ImVec2(0, 20));
}

void Hint(const char *text)
{
    ImGui::PushFont(g_fSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kDim));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/* Left-hand navigation. The active page carries a gold bar on its leading edge -- one
 * unambiguous marker, in the one colour that means "selected" everywhere else in the UI. */
bool NavItem(const char *label, bool active, int idx)
{
    const float h = 44.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  w = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(idx);
    bool clicked = ImGui::InvisibleButton("nav", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (active)   dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), mix(kPanel, kGold, 0.13f));
    else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), mix(kPanel, kGold, 0.06f));
    if (active)   dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + h), kGold);

    TextLS(g_fSmall, 14.0f, ImVec2(p.x + 22, p.y + h * 0.5f - 8.0f),
           active ? kGoldHi : (hov ? kText : kDim), label, 1.8f);
    return clicked;
}

/* A segmented control: one row of hard-edged cells, the chosen one filled gold. Used where
 * the options are few and worth showing at once -- a dropdown hides the alternatives, and
 * for a two-way choice like the profile that is strictly worse. */
bool Segmented(const char *id, int *v, const char *const *opts, int n, float cellw)
{
    bool changed = false;
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 32.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    ImGui::PushID(id);
    for (int i = 0; i < n; i++) {
        ImVec2 a(p.x + i * cellw, p.y), b(a.x + cellw - 2, p.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("seg", ImVec2(cellw - 2, h))) { *v = i; changed = true; }
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();

        bool on = (*v == i);
        dl->AddRectFilled(a, b, on ? mix(kPanel, kGold, 0.30f)
                                   : (hov ? mix(kPanel2, kGold, 0.12f) : kPanel2));
        dl->AddRect(a, b, on ? kGold : kLine, 0.0f, 0, 1.0f);
        float tw = TextLSWidth(g_fSmall, 14.0f, opts[i], 1.6f);
        TextLS(g_fSmall, 14.0f,
               ImVec2(a.x + (cellw - 2 - tw) * 0.5f, a.y + h * 0.5f - 8.0f),
               on ? kGoldHi : kDim, opts[i], 1.6f);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 6));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

/* One mission in the list: number, name, theatre. The number is set in gold and given its
 * own column so the campaign order is readable as a sequence at a glance. */
bool MissionRow(const Stage &s, bool selected, float w, int idx)
{
    const float h = 40.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::PushID(idx);
    bool clicked = ImGui::InvisibleButton("mi", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      selected ? mix(kPanel, kGold, 0.22f)
                               : (hov ? mix(kPanel2, kGold, 0.08f) : kPanel2));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), selected ? kGold : kLine, 0.0f, 0, 1.0f);
    if (selected) dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + h), kGoldHi);

    char num[8];
    if (s.mission > 0) snprintf(num, sizeof num, "%02d", s.mission);
    else               snprintf(num, sizeof num, "MP");
    TextLS(g_fSmall, 15.0f, ImVec2(p.x + 16, p.y + h * 0.5f - 8.5f),
           selected ? kGoldHi : kGold, num, 1.0f);

    dl->AddText(g_fBody, 18.0f, ImVec2(p.x + 54, p.y + h * 0.5f - 11.0f),
                selected ? kText : mix(kText, kDim, 0.25f), s.name);

    float pw = g_fSmall->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, s.place).x;
    dl->AddText(g_fSmall, 13.0f, ImVec2(p.x + w - pw - 14, p.y + h * 0.5f - 7.0f), kDim, s.place);
    /* Draw-list colours bypass ImGui's disabled alpha. Dim the entire custom row
     * as well as disabling its hit target, so the mission remains visible. */
    if (ImGui::GetStyle().Alpha < 1.0f)
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(8, 9, 11, 175));
    return clicked;
}

/* The two footer actions. `primary` is the one the window exists to reach, so it is the only
 * filled-gold control anywhere in the launcher. */
bool Btn(const char *label, ImVec2 size, bool primary)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("b", size);
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImGui::PopID();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 b(p.x + size.x, p.y + size.y);
    ImU32 fill = primary ? (act ? kGoldHi : (hov ? mix(kGold, kGoldHi, 0.5f) : kGold))
                         : (act ? mix(kPanel2, kGold, 0.35f)
                                : (hov ? mix(kPanel2, kGold, 0.18f) : kPanel2));
    dl->AddRectFilled(p, b, fill);
    dl->AddRect(p, b, primary ? kGoldHi : kLine, 0.0f, 0, 1.0f);

    float tw = TextLSWidth(g_fSmall, 15.0f, label, 2.0f);
    TextLS(g_fSmall, 15.0f, ImVec2(p.x + (size.x - tw) * 0.5f, p.y + size.y * 0.5f - 9.0f),
           primary ? IM_COL32(12, 11, 8, 255) : kText, label, 2.0f);
    return clicked;
}

/* A labelled slider: name on the left, value on the right, filled track underneath.
 *
 * Custom rather than ImGui::SliderInt because ImGui draws a grab block whose WIDTH is a
 * function of the value range -- `Spawns per kill` (0..8) gets a 90px block and `Max alive`
 * (1..64) gets a 12px one, so two adjacent rows look like two different controls. It also
 * draws no fill, which is the part that actually communicates magnitude. Here the track is
 * always the same shape and the fill carries the value. */
bool SliderRow(const char *label, int *v, int lo, int hi, const char *suffix,
               float w, bool enabled)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float th = 8.0f;
    const float ty = p.y + 27.0f;

    dl->AddText(g_fBody, 17.0f, p, enabled ? kText : kDim, label);
    char val[40];
    snprintf(val, sizeof val, "%d%s", *v, suffix ? suffix : "");
    float vw = g_fBody->CalcTextSizeA(17.0f, FLT_MAX, 0.0f, val).x;
    dl->AddText(g_fBody, 17.0f, ImVec2(p.x + w - vw, p.y), enabled ? kGoldHi : kDim, val);

    ImGui::SetCursorScreenPos(ImVec2(p.x, ty - 7.0f));
    ImGui::PushID(label);
    ImGui::InvisibleButton("sl", ImVec2(w, th + 14.0f));
    bool active = enabled && ImGui::IsItemActive();
    bool hov    = enabled && ImGui::IsItemHovered();
    ImGui::PopID();

    if (active && w > 0.0f) {
        float t = (ImGui::GetIO().MousePos.x - p.x) / w;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        *v = lo + (int) (t * (float) (hi - lo) + 0.5f);
    }

    float t = (hi > lo) ? (float) (*v - lo) / (float) (hi - lo) : 0.0f;
    ImU32 fill = !enabled ? mix(kPanel2, kDim, 0.35f) : ((hov || active) ? kGoldHi : kGold);
    dl->AddRectFilled(ImVec2(p.x, ty), ImVec2(p.x + w, ty + th), kPanel2);
    dl->AddRectFilled(ImVec2(p.x, ty), ImVec2(p.x + w * t, ty + th), fill);
    dl->AddRect(ImVec2(p.x, ty), ImVec2(p.x + w, ty + th), kLine, 0.0f, 0, 1.0f);
    if (enabled) {
        float hx = p.x + w * t;
        dl->AddRectFilled(ImVec2(hx - 2.0f, ty - 5.0f), ImVec2(hx + 2.0f, ty + th + 5.0f), kGoldHi);
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, ty + th + 16.0f));
    ImGui::Dummy(ImVec2(0, 0));
    return active;
}

/* ---- binding capture ------------------------------------------------------
 *
 * "Click the row, press the key." The alternative -- a dropdown of every SDL scancode
 * -- is roughly 240 entries and asks the player to know what their key is CALLED before
 * they can bind it, which is the wrong way round.
 *
 * One capture is live at a time, identified by a stable id string. Capture is armed by
 * a click and disarmed by the first input it sees, so it cannot be left on by
 * navigating away.
 */
static char g_captureId[64] = "";

static bool CaptureActive(const char *id)
{
    return g_captureId[0] != '\0' && strcmp(g_captureId, id) == 0;
}

static void CaptureBegin(const char *id)
{
    snprintf(g_captureId, sizeof g_captureId, "%s", id);
}

static void CaptureCancel(void)
{
    g_captureId[0] = '\0';
}

/* The name of whatever the player just pressed, or NULL if nothing yet.
 *
 * Reads the keyboard state array rather than events because the ImGui backend has
 * already consumed this frame's events, and a second SDL_PollEvent loop here would race
 * it. Mouse buttons come from ImGui for the same reason.
 *
 * ESCAPE is reserved as "cancel", so it cannot be bound. That is a real cost -- someone
 * may want it for pause -- but the alternative is a capture with no way out, and ESC is
 * already the mouse-release key in game, so binding it would collide anyway.
 */
static const char *CapturePoll(void)
{
    static char name[96];
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    ImGuiIO &io = ImGui::GetIO();

    if (k != NULL) {
        if (k[SDL_SCANCODE_ESCAPE]) { CaptureCancel(); return NULL; }
        for (int i = 0; i < SDL_NUM_SCANCODES; i++) {
            if (!k[i]) continue;
            /* A bare modifier is almost always the player reaching for a chord this
             * port does not support, but Left Ctrl and Left Shift ARE the conventional
             * crouch keys, so they have to be bindable. Only the ones nothing sensible
             * binds are skipped. */
            if (i == SDL_SCANCODE_LGUI || i == SDL_SCANCODE_RGUI ||
                i == SDL_SCANCODE_CAPSLOCK || i == SDL_SCANCODE_NUMLOCKCLEAR) {
                continue;
            }
            {
                const char *n = SDL_GetScancodeName((SDL_Scancode) i);
                if (n == NULL || *n == '\0') continue;
                snprintf(name, sizeof name, "%s", n);
                return name;
            }
        }
    }

    /* Mouse. Button 0 is left, 1 right, 2 middle, matching SDL_BUTTON_* order, and the
     * names are the ones geParseInputCode() in port_input.c accepts. */
    for (int b = 0; b < 5 && b < IM_ARRAYSIZE(io.MouseDown); b++) {
        if (!io.MouseDown[b]) continue;
        static const char *const mb[] = { "mouse1", "mouse2", "mouse3", "mouse4", "mouse5" };
        snprintf(name, sizeof name, "%s", mb[b]);
        return name;
    }
    if (io.MouseWheel > 0.0f) { snprintf(name, sizeof name, "wheelup");   return name; }
    if (io.MouseWheel < 0.0f) { snprintf(name, sizeof name, "wheeldown"); return name; }

    return NULL;
}

/* One editable binding row: a label, the current value, and a button that captures.
 *
 * `value` is the model's string, empty meaning "inherit". `dflt` is what that inherits
 * to, shown in the button so an untouched row still says something concrete rather than
 * leaving the player to guess. Returns true if it changed.
 */
static bool KeyBindRow(const char *id, const char *label, char *value, size_t cap,
                       const char *dflt, float labelw)
{
    bool changed = false;
    ImVec2 p = ImGui::GetCursorScreenPos();
    char btn[160];

    ImGui::GetWindowDrawList()->AddText(g_fBody, 18.0f, ImVec2(p.x, p.y + 6.0f), kText, label);
    ImGui::SetCursorScreenPos(ImVec2(p.x + labelw, p.y));
    ImGui::PushID(id);

    if (CaptureActive(id)) {
        const char *got = CapturePoll();
        if (got != NULL) {
            snprintf(value, cap, "%s", got);
            CaptureCancel();
            changed = true;
        }
    }

    if (CaptureActive(id)) {
        snprintf(btn, sizeof btn, "press a key...  (ESC cancels)");
    } else if (value[0] != '\0') {
        snprintf(btn, sizeof btn, "%s", value);
    } else {
        snprintf(btn, sizeof btn, "%s  (default)", (dflt && *dflt) ? dflt : "unbound");
    }

    if (Btn(btn, ImVec2(230, 30), CaptureActive(id))) {
        if (CaptureActive(id)) CaptureCancel();
        else                   CaptureBegin(id);
    }

    /* Clearing is a separate, smaller control rather than a value in the capture, because
     * there is no key that means "none" and a player who wants an action off should not
     * have to find that out. */
    ImGui::SetCursorScreenPos(ImVec2(p.x + labelw + 240.0f, p.y));
    if (Btn("x", ImVec2(30, 30), false)) {
        /* Two states to clear through, and they are different: a row showing an explicit
         * binding goes back to the preset default; a row already on the default is
         * explicitly UNBOUND. Collapsing them would make "no binding at all"
         * unreachable. */
        if (value[0] != '\0') { value[0] = '\0'; }
        else                  { snprintf(value, cap, "none"); }
        CaptureCancel();
        changed = true;
    }

    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 36.0f));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}


/* A text field with its label above it rather than to the right. ImGui puts the label after
 * the box, which reads as a stray word floating beside a field. */
void InputRow(const char *label, char *buf, size_t n, float w)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(g_fBody, 17.0f, p, kText, label);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 26.0f));
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("##t", buf, n);
    ImGui::PopID();
}

} /* namespace */

extern "C" int gePortLauncherRun(int argc, char **argv)
{
    const char *launcher_env = getenv("GETV_LAUNCHER");
#if defined(_WIN32)
    const int default_on_plain_start = 1;
#else
    const int default_on_plain_start = 0;
#endif
    bool wanted = geLauncherPolicyWantsWindow(
        argc, argv,
        launcher_env != NULL,
        env_bool("GETV_LAUNCHER", false),
        env_bool("GETV_LAUNCHER_AUTOPLAY", false),
        default_on_plain_start) != 0;

    /* On Windows the locally built game behaves like the Mac app: double-clicking the
     * executable opens the settings UI. GETV_LAUNCHER=0 and command-line gameplay arguments
     * remain explicit direct-start paths for automation and advanced use. */
    /* Autoplay implies the launcher. Without this it is unreachable, because the check above
     * would return before ever looking at it -- which is exactly what happened the first time
     * it was tested, and the run looked like a pass because the environment being asserted on
     * had been set by hand for the test anyway. */
    if (!wanted) return 0;

    g_argc = argc;
    g_argv = argv;

    /* GETV_LAUNCHER_AUTOPLAY=1 takes the launcher's path without opening the window: read
     * the environment into the model, write it back, relaunch. It exists because the window
     * blocks on a human, so the parts that can be wrong on their own -- whether a setting
     * survives model_load/model_store, and whether the exec actually happens -- would
     * otherwise only ever be tested by hand. It is the same code the Play button runs. */
    if (env_bool("GETV_LAUNCHER_AUTOPLAY", false)) {
        Model am;
        model_load(am);
        /* Apply the profile, which the interactive path does the moment the radio changes and
         * this path had no equivalent for. Without it, GETV_PROFILE_PLUS=1 here loaded
         * profile 1, wrote every setting back at whatever it already was, and produced a
         * half-applied GoldenEye+: MSAA and FXAA arrived from ge_config.c's own preset pass
         * while supersampling and the frame cap did not, because model_store had just written
         * them explicitly and the config layer will not displace an explicit value.
         *
         * Choosing the profile is the whole reason to set that variable, so acting on it is
         * what "autoplay takes the launcher's path" has to mean. */
        if (am.profile == 1) { apply_profile(am); }
        if (!developer_prepare_recording(am)) {
            fputs("[getv][launcher] cannot prepare telemetry reports directory\n", stderr);
            return 1;
        }
        model_store(am);
        printf("[getv][launcher] autoplay: profile=%s ruleset=%s%s\n",
               am.profile ? "goldeneye+" : "base-game",
               kRulesets[am.ruleset], am.horde ? " horde" : "");
        relaunch();
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("[getv][launcher] SDL_Init failed: %s\n", SDL_GetError());
        return geAppRendererActive() ? -1 : 0;                      /* fall through to the game rather than refusing to start */
    }

    /* A plain, small, resizable window. No GL attributes are requested beyond a double
     * buffer: this context exists only to draw ImGui, and asking for depth/MSAA here would
     * be asking for the game's settings in a window that is not the game. */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef GE_PLATFORM_DESKTOP
    /* Clamped to what the display actually offers. 1120x780 is the size the two-column pages
     * are laid out for, but this machine's panel is 1440x960 logical, and a window asked for
     * at a size the desktop cannot show is simply placed off the edge -- the right-hand third
     * of the launcher, including the Start button, ends up past the screen. Ask for the
     * design size, take what fits. */
    int winw = 1120, winh = 780;
    {
        SDL_Rect ub;
        if (SDL_GetDisplayUsableBounds(0, &ub) == 0) {
            if (winw > ub.w - 60) winw = ub.w - 60;
            if (winh > ub.h - 60) winh = ub.h - 60;
        }
        if (winw < 900) winw = 900;
        if (winh < 620) winh = 620;
    }
#else
    /* tvOS/iOS have exactly one display mode and no windowing -- see port_support.c's
     * ConfigWindow default for this platform (1920x1080, fullscreen=true). A window asked
     * for at a desktop-style clamped size (below) is never shown: SDL's UIKit backend has
     * no concept of a small movable window, so it silently produced a black screen with no
     * crash -- verified on the real Apple TV, matching this exact symptom, and confirmed by
     * a stray "Unbalanced calls to begin/end appearance transitions" UIKit warning that
     * stopped appearing once this matched gfx_sdl2.c's game-window creation below.
     *
     * The size itself is queried, not hardcoded to tvOS's fixed 1920x1080: iPhones and
     * iPads cover a wide range of portrait-shaped resolutions, and a landscape 1920x1080
     * request handed to a portrait screen does not get corrected by the fullscreen call
     * below -- verified on the iOS Simulator, where the whole launcher rendered into a
     * small landscape-shaped box pinned to one corner of the actual portrait frame rather
     * than filling it. SDL_GetDisplayBounds() reports whatever the real screen is on every
     * platform this branch runs on, tvOS included, so there is no need to special-case it. */
    int winw = 1920, winh = 1080;
    {
        SDL_Rect db;
        int dbrc = SDL_GetDisplayBounds(0, &db);
        printf("[getv][launcher][diag] SDL_GetDisplayBounds rc=%d w=%d h=%d\n",
               dbrc, db.w, db.h);
        if (dbrc == 0 && db.w > 0 && db.h > 0) {
            winw = db.w;
            winh = db.h;
        }
    }
    printf("[getv][launcher][diag] winw=%d winh=%d\n", winw, winh);
    /* TEMPORARY: printf capture from a UIKit sim app is unreliable through every
     * simctl/log mechanism tried (--console, --console-pty, --stdout file redirect, `log
     * show`/`log stream` predicated on the process) -- all came back empty even though the
     * process was confirmed alive via launchctl. A plain file write sidesteps all of that:
     * readable straight from the host via `simctl get_app_container ... data`. Remove once
     * the iOS sizing bug is closed. */
    {
        char *pp = SDL_GetPrefPath("goldeneyenative", "getv-diag");
        if (pp != NULL) {
            char path[4096];
            snprintf(path, sizeof path, "%sdiag.txt", pp);
            FILE *df = fopen(path, "w");
            if (df != NULL) {
                SDL_Rect db2;
                SDL_GetDisplayBounds(0, &db2);
                fprintf(df, "GetDisplayBounds=%dx%d\nwinw=%d winh=%d\n", db2.w, db2.h, winw, winh);
                fclose(df);
            }
            SDL_free(pp);
        }
    }
    /* See gfx_sdl2.c's identical comment: SDL_WINDOW_RESIZABLE makes SDL's iOS backend
     * ignore Info.plist's landscape-only lock and allow every orientation, which is what
     * produced this exact launcher UI squeezed into a small box in one corner of a
     * portrait frame on the iOS Simulator. Setting the hint AND dropping RESIZABLE below
     * (this window has nothing to resize on tvOS/iOS anyway) fixes it. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    SDL_Window *win = SDL_CreateWindow("GoldenEye 007",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       winw, winh,
#ifdef RAPI_METAL
                                       SDL_WINDOW_METAL | SDL_WINDOW_SHOWN |
                                       SDL_WINDOW_ALLOW_HIGHDPI
#else
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                                       SDL_WINDOW_ALLOW_HIGHDPI
#endif
#ifdef GE_PLATFORM_DESKTOP
                                       | SDL_WINDOW_RESIZABLE
#endif
                                       );
    if (win != NULL) {
        /* Same icon as the game window, from the same pixels. See ge_icon_apply.c. */
        gePortSetWindowIcon(win);
    }

#ifdef GE_PLATFORM_DESKTOP
    /* The mission list needs vertical room and the two-column pages need width; below this
     * the layout starts overlapping rather than reflowing. */
    if (win != NULL) SDL_SetWindowMinimumSize(win, 900, 620);
#else
    /* Mirrors gfx_sdl2.c's gfx_sdl_set_fullscreen(): on tvOS/iOS the window must be
     * explicitly transitioned to fullscreen-desktop, or nothing is displayed at all. */
    if (win != NULL) SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (win != NULL) {
        extern void gePortForceLandscapeOrientation(void);
        gePortForceLandscapeOrientation();
    }
    if (win != NULL) {
        int aw = 0, ah = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(win, &aw, &ah);
#ifdef RAPI_METAL
        SDL_Metal_GetDrawableSize(win, &dw, &dh);
#endif
        printf("[getv][launcher][diag] after fullscreen: SDL_GetWindowSize=%dx%d "
               "drawable=%dx%d\n", aw, ah, dw, dh);
        char *pp = SDL_GetPrefPath("goldeneyenative", "getv-diag");
        if (pp != NULL) {
            char path[4096];
            snprintf(path, sizeof path, "%sdiag.txt", pp);
            FILE *df = fopen(path, "a");
            if (df != NULL) {
                fprintf(df, "after_fullscreen GetWindowSize=%dx%d drawable=%dx%d\n", aw, ah, dw, dh);
                fclose(df);
            }
            SDL_free(pp);
        }
    }
#endif
    if (win == NULL) {
        printf("[getv][launcher] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return geAppRendererActive() ? -1 : 0;
    }
#ifndef RAPI_METAL
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (ctx == NULL) {
        printf("[getv][launcher] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return geAppRendererActive() ? -1 : 0;
    }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ge_apply_style();
    /* Before the first NewFrame: the backend builds the atlas texture there, and a font
     * added afterwards would not be in it. */
    ge_load_fonts();
#ifdef RAPI_METAL
    ImGui_ImplSDL2_InitForMetal(win);
    /* AFTER ImGui::CreateContext(), not before -- unlike SDL_GL_CreateContext() above (pure
     * GL, no ImGui calls in it), this calls ImGui_ImplMetal_Init() internally, which asserts
     * a current ImGui context via ImGui::GetIO(). Moving it earlier (originally grouped with
     * window creation, mirroring the GL context's position) crashed with exactly that
     * assertion the first time this ran -- verified on the tvOS Simulator, not just reasoned
     * about; see the commit this landed in. */
    if (!geLauncherMetalCreate(win)) {
        printf("[getv][launcher] geLauncherMetalCreate failed\n");
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(win);
        SDL_Quit();
        return geAppRendererActive() ? -1 : 0;
    }
#else
    /* imgui_impl_opengl2, matching ge_imgui.cpp: this build takes macOS's legacy context and
     * the GL3 backend calls glGenVertexArrays, which a 2.1 context does not have. */
    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL2_Init();
#endif

    Model m;
    model_load(m);

    bool running = true;
    bool launch  = false;
    /* GETV_LAUNCHER_PAGE=<0..7> opens on that page. It exists so the pages the probe
     * cannot reach -- the probe never clicks anything -- can each be rendered and looked at
     * without a human driving the mouse. */
    int  page    = env_int("GETV_LAUNCHER_PAGE", 0);
#if defined(__APPLE__) && defined(GE_PLATFORM_MAC)
    if (geAppRendererActive() && !geAppRendererAvailable(geAppRendererSelected())) page = 5;
    bool rendererSaveFailed = false;
#endif
    if (page < 0 || page > 7) page = 0;
    const int probe_frames = env_int("GETV_LAUNCHER_PROBE", 0);
    int probe_seen = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) { printf("[getv][launcher] SDL_QUIT received\n"); running = false; }
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(win)) {
                printf("[getv][launcher] SDL_WINDOWEVENT_CLOSE received\n");
                running = false;
            }
        }

        /* No Metal "new frame" call here -- geLauncherMetalRenderAndPresent() calls
         * ImGui_ImplMetal_NewFrame() itself, once the frame's render pass exists (see its
         * comment; same reasoning as ge_imgui.cpp's gePortImguiNewFrame()). */
#ifndef RAPI_METAL
        ImGui_ImplOpenGL2_NewFrame();
#endif
        ImGui_ImplSDL2_NewFrame();
#ifndef GE_PLATFORM_DESKTOP
        /* tvOS/iOS: SDL reports the real, fullscreen window size (e.g. 1920x1080), which is
         * far too fine-grained for a screen viewed from ten feet away -- every size in this
         * file is an absolute pixel tuned for a desktop canvas, and read literally against a
         * 1920x1080 backdrop the whole launcher (fonts, buttons, spacing) comes out tiny.
         * Render into a smaller logical canvas instead and let ImGui's own NDC projection
         * stretch it to fill the real screen. The canvas keeps the PHYSICAL aspect ratio
         * (not the desktop design size's 1120x780, which is a different, narrower ratio) so
         * nothing gets squashed. 620 is the height this exact layout is already proven not
         * to overlap below -- see SDL_SetWindowMinimumSize(win, 900, 620) in the desktop
         * branch above, which exists for the same reason. */
        {
            ImGuiIO &io2 = ImGui::GetIO();
            float ph_w = io2.DisplaySize.x, ph_h = io2.DisplaySize.y;
            if (ph_w > 0.0f && ph_h > 0.0f) {
                /* Clamped to the real screen height, never above it: on tvOS (ph_h=1080)
                 * this is still 620, giving the intended ~1.7x enlarge. On an iPhone in
                 * landscape (ph_h≈393, well under 620) an UNCLAMPED 620 would make the
                 * logical canvas TALLER than the real screen -- the opposite of the
                 * intended effect, since FramebufferScale then has to SHRINK everything
                 * to fit, on top of the launcher's already-modest desktop-authored sizes.
                 * Verified on Evan's real iPhone 14 Pro: with the unclamped constant the
                 * launcher came back "sooo small, not filling the screen" even after
                 * SDL_GetDisplayBounds() was already reporting the correct real 852x393.
                 * Clamping makes log_h==ph_h on any screen shorter than 620, which
                 * collapses the whole transform to identity (scale 1.0) -- exactly "use
                 * the real size, don't shrink it further" for anything phone-sized. */
                float log_h = (ph_h < 620.0f) ? ph_h : 620.0f;
                float log_w = log_h * ph_w / ph_h;
                if (io2.MousePos.x != FLT_MAX) io2.MousePos.x *= log_w / ph_w;
                if (io2.MousePos.y != FLT_MAX) io2.MousePos.y *= log_h / ph_h;
                io2.DisplaySize = ImVec2(log_w, log_h);
                /* imgui_impl_metal.mm sets its MTLViewport to DisplaySize * FramebufferScale
                 * (see its ImGui_ImplMetal_SetupRenderState), not to the drawable's real
                 * pixel size -- leaving FramebufferScale at SDL's default (1,1) here shrank
                 * the viewport itself to the small logical canvas, rendering the whole UI
                 * into a tiny box in the corner of the screen instead of stretching it.
                 * Scaling FramebufferScale back up by the same factor DisplaySize was
                 * scaled down recovers the full physical viewport while keeping every
                 * widget's logical coordinates (and therefore on-screen size) small. */
                io2.DisplayFramebufferScale = ImVec2(ph_w / log_w, ph_h / log_h);
            }
        }
#endif
        ImGui::NewFrame();

        {
            ImGuiIO &io2 = ImGui::GetIO();
            int ww = (int) io2.DisplaySize.x, wh = (int) io2.DisplaySize.y;
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2((float) ww, (float) wh));
            ImGui::Begin("GoldenEye", NULL,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

            const float W = (float) ww, H = (float) wh;
            const float headerH = 154.0f, footerH = 78.0f, navW = 208.0f;
            ImDrawList *dl = ImGui::GetWindowDrawList();

            /* ------------------------------------------------------------ header */
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, headerH), kPanel);
            dl->AddLine(ImVec2(0, headerH - 2.0f), ImVec2(W, headerH - 2.0f), kGold, 2.0f);

            TextLS(g_fTitle, 46.0f, ImVec2(34, 20), kGoldHi, "GOLDENEYE", 5.0f);
            {
                float gw = TextLSWidth(g_fTitle, 46.0f, "GOLDENEYE", 5.0f);
                TextLS(g_fTitle, 46.0f, ImVec2(34 + gw + 16, 20), kGold, "007", 5.0f);
            }
            TextLS(g_fSmall, 12.0f, ImVec2(36, 74), kDim,
#ifdef RAPI_METAL
                   "NATIVE PORT / METAL / DECOMPILED", 3.0f);
#else
                   "NATIVE PORT / OPENGL / DECOMPILED", 3.0f);
#endif

            /* The profile is the single choice that changes the meaning of everything else on
             * the Video page, so it sits in the header rather than inside one of the pages --
             * it is scope, not a setting. */
            TextLS(g_fSmall, 12.0f, ImVec2(W - 336, 26), kDim, "PROFILE", 2.6f);
            {
                int prev_profile = m.profile;
                static const char *const kProf[] = { "BASE GAME", "GOLDENEYE+" };
                ImGui::SetCursorScreenPos(ImVec2(W - 336, 46));
                Segmented("prof", &m.profile, kProf, 2, 150.0f);
                if (m.profile != prev_profile) apply_profile(m);
            }
            ImGui::SetCursorScreenPos(ImVec2(36, 104));
            ImGui::PushTextWrapPos(W - 36);
            Hint(m.profile == 0
                 ? "Base Game: N64 graphics with your selected controls and timing. Start at the title screen or choose a mission. "
                   "No Brutal effects, mods or launcher cheats. Resolution and fullscreen remain available."
                 : "GoldenEye+: enhanced graphics and optional gameplay, controls, mods and cheats. "
                   "Start from the title screen or choose a mission. Brutal effects are optional.");
            ImGui::PopTextWrapPos();

            /* ------------------------------------------------------------ nav */
            dl->AddRectFilled(ImVec2(0, headerH), ImVec2(navW, H - footerH), kPanel);
            dl->AddLine(ImVec2(navW, headerH), ImVec2(navW, H - footerH), kLine, 1.0f);

            static const char *const kPages[] =
                { "MISSION", "CO-OP", "GAMEPLAY", "CONTROLS", "CHEATS", "VIDEO", "MODS", "DEVELOPER TOOLS" };
            const int kPageCount = (int)(sizeof kPages / sizeof kPages[0]);
            ImGui::SetCursorScreenPos(ImVec2(0, headerH + 20));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(kPanel));
            ImGui::BeginChild("nav", ImVec2(navW, H - footerH - headerH - 20),
                              ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            for (int i = 0; i < kPageCount; i++) if (NavItem(kPages[i], page == i, i)) page = i;
            ImGui::EndChild();
            ImGui::PopStyleColor();

            /* ------------------------------------------------------------ content */
            ImGui::SetCursorScreenPos(ImVec2(navW + 30, headerH + 26));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::BeginChild("content", ImVec2(W - navW - 60, H - headerH - footerH - 44),
                              ImGuiChildFlags_None, 0);

            const bool locked_page = m.profile == 0 &&
                (page == 1 || page == 2 || page == 4 || page == 6);
            if (locked_page)
                Hint("Base Game uses original gameplay. "
                     "Choose GoldenEye+ above to customize these options.");
            ImGui::BeginDisabled(locked_page);
            if (page == 0) {
                Section("GAME START");
                static const char *const kStart[] = { "ORIGINAL GAME START", "MISSION SELECTOR" };
                int start = m.pick_stage ? 1 : 0;
                if (Segmented("start", &start, kStart, 2, 240.0f)) m.pick_stage = start != 0;
                Hint(m.pick_stage
                     ? "The game boots straight into the mission selected below."
                     : "The game boots to the title screen and the mission is chosen there.");

                ImGui::BeginDisabled(!m.pick_stage);
                {
                    float availw = ImGui::GetContentRegionAvail().x;
                    float colw   = (availw - 14.0f) * 0.5f;

                    Section("SOLO CAMPAIGN");
                    {
                        ImVec2 st = ImGui::GetCursorScreenPos();
                        int r = 0;
                        for (int i = 0; i < kStageCount; i++) {
                            if (kStages[i].mp_only) continue;
                            ImGui::SetCursorScreenPos(ImVec2(st.x + (r % 2) * (colw + 14.0f),
                                                             st.y + (r / 2) * 46.0f));
                            if (MissionRow(kStages[i], m.stage_idx == i, colw, i)) m.stage_idx = i;
                            r++;
                        }
                        ImGui::SetCursorScreenPos(ImVec2(st.x, st.y + ((r + 1) / 2) * 46.0f + 6.0f));
                        ImGui::Dummy(ImVec2(0, 0));
                    }

                    Section("MULTIPLAYER ARENAS");
                    {
                        ImVec2 st = ImGui::GetCursorScreenPos();
                        int r = 0;
                        for (int i = 0; i < kStageCount; i++) {
                            if (!kStages[i].mp_only) continue;
                            ImGui::SetCursorScreenPos(ImVec2(st.x + (r % 2) * (colw + 14.0f),
                                                             st.y + (r / 2) * 46.0f));
                            if (MissionRow(kStages[i], m.stage_idx == i, colw, i)) m.stage_idx = i;
                            r++;
                        }
                        ImGui::SetCursorScreenPos(ImVec2(st.x, st.y + ((r + 1) / 2) * 46.0f + 6.0f));
                        ImGui::Dummy(ImVec2(0, 0));
                    }

                    if (kStages[m.stage_idx].mp_only) {
                        ImGui::PushStyleColor(ImGuiCol_Text, v4(kWarn));
                        ImGui::PushFont(g_fSmall);
                        ImGui::TextWrapped("%s has no solo setup. Started alone it loads the "
                                           "geometry with no props and no spawn -- that is the "
                                           "level data, not a rendering fault.",
                                           kStages[m.stage_idx].name);
                        ImGui::PopFont();
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::EndDisabled();
            }

            /* ------------------------------------------------------------ co-op */
            else if (page == 1) {
                Section("CO-OP");
                Hint("Two to four players share one solo mission, split screen. Everyone gets "
                     "their own spawn and camera. The mission is written around a single Bond, "
                     "so treat this as bring-up rather than a finished mode.");

                const float cw = ImGui::GetContentRegionAvail().x * 0.55f;

                /* 0 and 1 both mean off -- the game only splits the screen at 2 or more, so
                 * offering 1 would look like a choice that does nothing. */
                static const char *const kCoopCounts[] =
                    { "Off", "2 players", "3 players", "4 players" };
                int sel = (m.coop_players >= 2) ? (m.coop_players - 1) : 0;
                /* Label above rather than to the right of the box: SliderRow puts its label on
                 * the left and ImGui's default puts a combo's on the right, which reads as two
                 * different forms on one page. */
                ImGui::TextUnformatted("Players");
                ImGui::SetNextItemWidth(cw);
                if (ImGui::Combo("##coopplayers", &sel, kCoopCounts, 4)) {
                    m.coop_players = (sel == 0) ? 0 : (sel + 1);
                }

                if (m.coop_players >= 2) {
                    ImGui::Spacing();
                    Section("TEAM OPTIONS");
                    ImGui::Checkbox("Friendly fire", &m.coop_ff);
                    Hint(m.coop_ff
                         ? "Players can damage each other. Everyone starts on one pad facing the "
                           "same way, so the first shot usually finds a team mate."
                         : "Players cannot damage each other. Guards, explosions and falls still "
                           "hurt.");

                    ImGui::Spacing();
                    SliderRow("Respawn delay", &m.coop_respawn, 0, 30, "s", cw, true);
                    Hint("Seconds before a player who died is put back on the level's start pad. "
                         "0 waits for a button press instead. If everyone is down at once the "
                         "mission is lost either way.");
                }

                ImGui::Spacing();
                Hint("Pick the mission on the MISSION page. Co-op needs a controller for each "
                     "player.");
            }

            else if (page == 2) {
                Section("GAMEPLAY PRESET");
                static const char *const kRsUp[] =
                    { "CLASSIC", "HARDCORE", "SURVIVAL", "CHAOS", "HORDE" };
                /* Verbatim from ge_presets[].blurb in ge_ruleset.c, so this cannot drift away
                 * from what the presets actually do. */
                static const char *const kRsDesc[] = {
                    "The game as shipped.",
                    "Tougher guards, less ammo, half the player health.",
                    "Hardcore-lite, with endless waves.",
                    "Everything turned up.",
                    "Stock difficulty, endless waves.",
                };
                Segmented("rs", &m.ruleset, kRsUp, 5, 124.0f);
                Hint(kRsDesc[m.ruleset]);

                ImGui::Dummy(ImVec2(0, 6));
                ImGui::Checkbox("Override with custom values", &m.rs_custom);

                if (m.rs_custom) {
                    Section("BALANCE");
                    Hint("Percentages of the original. 100 is unmodified. These replace the "
                         "preset above.");
                    ImGui::Dummy(ImVec2(0, 8));
                    {
                        float availw = ImGui::GetContentRegionAvail().x;
                        float colw   = (availw - 30.0f) * 0.5f;
                        struct { const char *n; int *v; } rows[] = {
                            { "Enemy health",   &m.enemy_health   },
                            { "Enemy damage",   &m.enemy_damage   },
                            { "Enemy accuracy", &m.enemy_accuracy },
                            { "Enemy reaction", &m.enemy_reaction },
                            { "Player health",  &m.player_health  },
                            { "Player armour",  &m.player_armour  },
                            { "Ammo",           &m.ammo           },
                            { "Explosions",     &m.explosion_damage },
                            { "Turrets",        &m.turret_damage  },
                        };
                        ImVec2 st = ImGui::GetCursorScreenPos();
                        for (int i = 0; i < 9; i++) {
                            ImGui::SetCursorScreenPos(ImVec2(st.x + (i % 2) * (colw + 30.0f),
                                                             st.y + (i / 2) * 62.0f));
                            SliderRow(rows[i].n, rows[i].v, 10, 500, "%", colw, true);
                        }
                        ImGui::SetCursorScreenPos(ImVec2(st.x, st.y + 5 * 62.0f + 4.0f));
                        ImGui::Dummy(ImVec2(0, 0));
                    }
                }

                Section("BRUTAL GOLDENEYE");
                bool brutal_enabled = !m.base_game;
                if (ImGui::Checkbox("Enable Brutal effects", &brutal_enabled)) {
                    m.base_game = !brutal_enabled;
                    if (brutal_enabled && m.gibs == 0) m.gibs = 1;
                }
                Hint("Off by default. Enable for added gore on the next launch. "
                     "Other mods and gameplay options remain separately configured.");
                ImGui::BeginDisabled(m.base_game);
                static const char *const kGibs[] = { "Off", "Explosions", "High damage", "Always" };
                static const char *const kBlood[] = { "Original", "Enhanced", "Excessive" };
                ImGui::Combo("Gib trigger", &m.gibs, kGibs, 4);
                ImGui::Combo("Blood intensity", &m.blood, kBlood, 3);
                SliderRow("Stain limit", &m.blood_limit, 16, 512, NULL,
                          ImGui::GetContentRegionAvail().x, true);
                ImGui::EndDisabled();
                Hint("Blood effects apply when an enemy gibs. Choose a gib trigger to enable them.");

                Section("HORDE MODE");
                ImGui::Checkbox("Endless waves", &m.horde);
                if (m.horde) {
                    Hint("When a guard dies, replacements spawn where it fell. Waves grow with "
                         "kills. The engine refuses to spawn with fewer than three free guard "
                         "slots, so the real ceiling belongs to the level.");
                    ImGui::Dummy(ImVec2(0, 8));
                    {
                        float hw = ImGui::GetContentRegionAvail().x;
                        SliderRow("Spawns per kill", &m.horde_per_kill,     0, 8,   NULL, hw, true);
                        SliderRow("Per-kill cap",    &m.horde_per_kill_cap, 1, 8,   NULL, hw, true);
                        SliderRow("Max alive",       &m.horde_max_alive,    1, 64,  NULL, hw, true);
                        SliderRow("Kills per wave",  &m.horde_wave_kills,   1, 100, NULL, hw, true);
                        SliderRow("Growth per wave", &m.horde_growth,       0, 8,   NULL, hw, true);
                    }
                }
            }

            else if (page == 3) {
                float cw = ImGui::GetContentRegionAvail().x;

                Section("MOUSE AND KEYBOARD");
                ImGui::Checkbox("Mouse look", &m.mouse);
                if (m.mouse) {
                    ImGui::Combo("Mouse response", &m.mouse_mode, "Modern\0Classic N64\0");
                    Hint(m.mouse_mode ? "Original stick acceleration and turning limit."
                                      : "Direct mouse look. No stick acceleration or turning limit.");
                    ImGui::Dummy(ImVec2(0, 8));
                    SliderRow("Sensitivity", &m.mouse_sens, 10, 400, "%", cw, true);
                    ImGui::Checkbox("Invert Y", &m.mouse_invert);
                    ImGui::Dummy(ImVec2(0, 6));
                    Hint("Left button fires, right aims, ESC releases the cursor. "
                         "Sensitivity depends on mouse DPI; modern mode uses 0.1 degree per count at 100%.");
                    if (!m.mouse_mode)
                        Hint("Vehicles, network sessions and scripted replays use Classic N64 controls.");
                } else {
                    Hint("Off. A connected gamepad still works either way; the two are ORed "
                         "rather than exclusive.");
                }

                ImGui::Dummy(ImVec2(0, 10));
                ImGui::Checkbox("Keyboard", &m.keyboard);
                if (!m.keyboard) {
                    Hint("Off. A connected gamepad still works; the two are ORed rather "
                         "than exclusive.");
                }

                Section("PRESET");
                {
                    static const char *const kPresets[] = { "MODERN", "N64" };
                    int before = m.preset;
                    Segmented("inputpreset", &m.preset, kPresets, 2, 120.0f);
                    /* Changing the preset must not silently keep a binding the player set
                     * under the other one -- the whole value of a preset is that picking it
                     * describes the layout completely. Explicit choices are cleared, which
                     * is what "unset" already means everywhere else on this page. */
                    if (m.preset != before) {
                        for (int a = 0; a < kActionCount; a++) {
                            m.keybind[a][0] = '\0';
                            m.bind_all[a] = -1;
                            for (int p2 = 0; p2 < 4; p2++) m.bind_p[p2][a] = -1;
                        }
                        for (int x = 0; x < GE_AXIS_MAX; x++) m.keyaxis[x][0] = '\0';
                        CaptureCancel();
                    }
                }
                Hint(m.preset == GE_PRESET_N64
                     ? "What this port defaulted to before remapping existed: Q aims, R "
                       "cycles weapon, no reload key, use on the east face button. Pick this "
                       "to revert rather than rebinding by hand."
                     : "WASD and the mouse. Right button aims, E interacts, R reloads, C "
                       "crouches, the wheel changes weapon. On a pad: south interacts, west "
                       "reloads, east crouches, north cycles weapon.");

                Section("HOLD OR TOGGLE");
                {
                    static const char *const kHT[] = { "HOLD", "TOGGLE" };
                    ImVec2 hp = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddText(
                        g_fBody, 18.0f, ImVec2(hp.x, hp.y + 6.0f), kText, "Aim");
                    ImGui::SetCursorScreenPos(ImVec2(hp.x + 170.0f, hp.y));
                    Segmented("aimmode", &m.aim_mode, kHT, 2, 110.0f);
                    ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + 40.0f));

                    hp = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddText(
                        g_fBody, 18.0f, ImVec2(hp.x, hp.y + 6.0f), kText, "Crouch");
                    ImGui::SetCursorScreenPos(ImVec2(hp.x + 170.0f, hp.y));
                    Segmented("crouchmode", &m.crouch_mode, kHT, 2, 110.0f);
                    ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + 44.0f));
                    ImGui::Dummy(ImVec2(0, 0));
                }
                Hint("Aim toggle is GoldenEye's own per-player aim-control option, which the "
                     "engine reads as a press rather than a hold -- not something bolted on "
                     "top. Crouch is the port's, because the engine has no crouch button.");
                ImGui::Dummy(ImVec2(0, 4));
                Hint("There is no stand key. Crouch TOGGLES: press it again to stand up. "
                     "In HOLD mode, releasing it stands you up instead.");

                Section("KEYBOARD AND MOUSE");
                Hint("Click a binding and press the key or mouse button you want. ESC "
                     "cancels. The x button clears back to the preset default, and clears "
                     "again to unbound.");
                ImGui::Dummy(ImVec2(0, 8));
                {
                    const float labelw = 170.0f;
                    for (int a = 0; a < kActionCount; a++) {
                        char id[64];
                        snprintf(id, sizeof id, "key%d", a);
                        KeyBindRow(id, ActionLabel(a), m.keybind[a], sizeof m.keybind[a],
                                   gePresetKeys(m.preset, a), labelw);
                    }
                    ImGui::Dummy(ImVec2(0, 10));
                    ImGui::GetWindowDrawList()->AddText(
                        g_fBody, 17.0f, ImGui::GetCursorScreenPos(), kDim, "Movement");
                    ImGui::Dummy(ImVec2(0, 24));
                    for (int x = 0; x < GE_AXIS_MAX; x++) {
                        char id[64];
                        snprintf(id, sizeof id, "axis%d", x);
                        KeyBindRow(id, AxisLabel(x), m.keyaxis[x], sizeof m.keyaxis[x],
                                   gePresetAxisKeys(m.preset, x), labelw);
                    }
                }
                ImGui::Dummy(ImVec2(0, 6));
                if (Btn("RESET KEYBOARD", ImVec2(200, 32), false)) {
                    for (int a = 0; a < kActionCount; a++) m.keybind[a][0] = '\0';
                    for (int x = 0; x < GE_AXIS_MAX; x++)   m.keyaxis[x][0] = '\0';
                    CaptureCancel();
                }
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::Checkbox("Dedicated crouch key", &m.crouch_key);
                Hint("Off leaves only the retail gesture: hold aim and push down. The crouch "
                     "binding above stops working.");

                Section("BINDINGS FOR");
                /* ALL first, then the four players. The tab IS the scope, so the thing being
                 * edited is never ambiguous -- which matters here more than usual, because
                 * "fire" on this page can mean two different keys. */
                static const char *const kWho[] = { "ALL", "P1", "P2", "P3", "P4" };
                Segmented("bindwho", &m.bind_tab, kWho, 5, 96.0f);
                Hint(m.bind_tab == 0
                     ? "Applies to every player. A player with its own choice below overrides "
                       "this one."
                     : "Applies to this player only. Anything left on \"same as all\" follows "
                       "the ALL tab.");

                Section("ACTIONS");
                {
                    const float labelw = 170.0f;
                    for (int a = 0; a < kActionCount; a++) {
                        int *slot = (m.bind_tab == 0) ? &m.bind_all[a]
                                                      : &m.bind_p[m.bind_tab - 1][a];

                        /* What this action will actually do if nothing more is chosen: the
                         * ALL tab's value if there is one, otherwise port_os.c's default.
                         * Shown so an inherited row still says something concrete rather than
                         * leaving the player to work it out. */
                        const char *dflt = kSources[gePresetSource(m.preset, a)];
                        const char *eff = (m.bind_all[a] >= 0) ? kSources[m.bind_all[a]]
                                                               : dflt;

                        ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::GetWindowDrawList()->AddText(
                            g_fBody, 18.0f, ImVec2(p.x, p.y + 6.0f), kText, ActionLabel(a));

                        ImGui::SetCursorScreenPos(ImVec2(p.x + labelw, p.y));
                        ImGui::PushID(a);
                        ImGui::SetNextItemWidth(160.0f);

                        char preview[64];
                        if (*slot >= 0) {
                            snprintf(preview, sizeof preview, "%s", kSources[*slot]);
                        } else if (m.bind_tab == 0) {
                            snprintf(preview, sizeof preview, "default (%s)", dflt);
                        } else {
                            snprintf(preview, sizeof preview, "same as all (%s)", eff);
                        }

                        if (ImGui::BeginCombo("##src", preview)) {
                            bool unset = (*slot < 0);
                            char none_label[64];
                            if (m.bind_tab == 0) {
                                snprintf(none_label, sizeof none_label,
                                         "default (%s)", dflt);
                            } else {
                                snprintf(none_label, sizeof none_label,
                                         "same as all (%s)", eff);
                            }
                            if (ImGui::Selectable(none_label, unset)) *slot = -1;
                            ImGui::Separator();
                            for (int s = 0; s < kSourceCount; s++) {
                                bool sel = (*slot == s);
                                if (ImGui::Selectable(kSources[s], sel)) *slot = s;
                                if (sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::PopID();

                        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 40.0f));
                        ImGui::Dummy(ImVec2(0, 0));
                    }
                    (void) cw;
                }

                ImGui::Dummy(ImVec2(0, 6));
                if (Btn("RESET THIS TAB", ImVec2(180, 32), false)) {
                    for (int a = 0; a < kActionCount; a++) {
                        if (m.bind_tab == 0) m.bind_all[a] = -1;
                        else                 m.bind_p[m.bind_tab - 1][a] = -1;
                    }
                }

                Section("NOTES");
                Hint("Button names are positional, not printed labels. \"a\" is always the "
                     "bottom face button, including on Nintendo pads where it is marked B. "
                     "The gamepad profile changes prompts only, so it cannot make \"a\" mean "
                     "a different physical button.");
                ImGui::Dummy(ImVec2(0, 6));
                Hint("Crouch and reload are bindable now. Neither reaches the game through "
                     "the N64 controller -- the engine has no button for either -- so the "
                     "port reads them back out of the binding table directly. The retail "
                     "crouch gesture still works: hold aim and push down. Retail RELOAD was "
                     "the use button with nothing in reach; binding a reload key turns that "
                     "double duty off, so interacting no longer reloads.");
                ImGui::Dummy(ImVec2(0, 6));
                Hint("Prev weapon is unbound on the pad on purpose. GoldenEye has no "
                     "back-cycle button; the port synthesises the retail hold-inventory + "
                     "tap-fire gesture, which is faithful but unverified on hardware. The "
                     "mouse wheel binds to it by default, where one notch is unambiguous.");

                Section("SAVE");
                /* Everything else in this launcher lasts until you quit -- settings are
                 * handed to the re-exec'd game as environment variables and nothing writes
                 * them down. For a rebound key that is not good enough, so this page can
                 * write itself to goldeneye.cfg.
                 *
                 * A deliberate button rather than an automatic save on every change: the
                 * config file is a document the player edits by hand too, and a UI that
                 * rewrites it whenever a combo box moves is one nobody can trust to leave
                 * their file alone. */
                {
                    static double saved_at = -1.0;
                    if (Btn("SAVE CONTROLS", ImVec2(210, 34), true)) {
                        controls_save_to_config(m);
                        saved_at = ImGui::GetTime();
                    }
                    ImGui::Dummy(ImVec2(0, 6));
                    if (saved_at > 0.0 && ImGui::GetTime() - saved_at < 4.0) {
                        char msg[640];
                        const char *path = geConfigPath();
                        snprintf(msg, sizeof msg, "Saved to %s",
                                 (path && *path) ? path : "(nowhere -- no config file)");
                        Hint(msg);
                    } else {
                        char msg[640];
                        const char *path = geConfigPath();
                        snprintf(msg, sizeof msg,
                                 "Writes this page to %s. Comments and settings from other "
                                 "pages are left alone. Without this, control changes last "
                                 "only until you quit.",
                                 (path && *path) ? path : "your config file");
                        Hint(msg);
                    }
                }
            }

            else if (page == 4) {
                Section("CHEATS");
                Hint("The game's own cheats. Those marked IN-GAME still need switching on from "
                     "the pause menu -- their effect lives in the turn-on handler, which needs "
                     "a player that does not exist at startup.");
                ImGui::Dummy(ImVec2(0, 10));
                {
                    float availw = ImGui::GetContentRegionAvail().x;
                    float colw   = (availw - 20.0f) * 0.5f;
                    ImVec2 st = ImGui::GetCursorScreenPos();
                    for (int i = 0; i < kCheatCount; i++) {
                        float rx = st.x + (i % 2) * (colw + 20.0f);
                        float ry = st.y + (i / 2) * 34.0f;
                        ImGui::SetCursorScreenPos(ImVec2(rx, ry));
                        ImGui::PushID(i);
                        ImGui::Checkbox(kCheats[i].label, &m.cheat_on[i]);
                        ImGui::PopID();
                        /* Anchored to the row's own y, not to wherever the cursor ended up
                         * after the checkbox -- that varies with the label and left the tags
                         * on a ragged line. */
                        if (!kCheats[i].live) {
                            TextLS(g_fSmall, 11.0f,
                                   ImVec2(rx + colw - 52.0f, ry + 6.0f), kDim, "IN-GAME", 1.2f);
                        }
                    }
                    ImGui::SetCursorScreenPos(
                        ImVec2(st.x, st.y + ((kCheatCount + 1) / 2) * 34.0f + 4.0f));
                    ImGui::Dummy(ImVec2(0, 0));
                }
            }

            else if (page == 5) {
                float vw = ImGui::GetContentRegionAvail().x;

#if defined(__APPLE__) && defined(GE_PLATFORM_MAC)
                if (geAppRendererActive()) {
                    Section("RENDERER");
                    int renderer = geAppRendererSelected();
                    const char *label = renderer == 0 ? "OpenGL" : renderer == 1 ? "Metal" : "Choose a renderer";
                    ImGui::SetNextItemWidth(260);
                    if (ImGui::BeginCombo("Renderer", label)) {
                        const char *names[] = {"OpenGL", "Metal"};
                        for (int choice = 0; choice < 2; choice++) {
                            bool available = geAppRendererAvailable(choice) != 0;
                            ImGui::BeginDisabled(!available);
                            if (ImGui::Selectable(names[choice], renderer == choice)) {
                                setenv("GETV_MAC_APP_RENDERER", choice ? "metal" : "gl", 1);
                                rendererSaveFailed = !geAppRememberRenderer(choice, CFSTR("org.goldeneyenative.renderer"));
                            }
                            ImGui::EndDisabled();
                        }
                        ImGui::EndCombo();
                    }
                    if (rendererSaveFailed) Hint("Could not save the renderer preference. This choice applies to this session only.");
                    Hint("Remembered separately from game settings. Takes effect when you start the game.");
                    if (!geAppRendererAvailable(0)) Hint("OpenGL is not installed. Install it to enable fallback.");
                    if (!geAppRendererAvailable(1)) Hint("Metal is not installed or is unavailable on this Mac.");
                    if (!geAppRendererAvailable(geAppRendererSelected()))
                        Hint("Choose an available renderer before starting the game.");
                }
#endif

                Section("DISPLAY");
                InputRow("Resolution", m.resolution, sizeof m.resolution, 260);
                ImGui::Dummy(ImVec2(0, 4));
                Hint("WIDTHxHEIGHT, minimum 320x240.");
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::Checkbox("Fullscreen", &m.fullscreen);

                ImGui::BeginDisabled(m.profile == 0);
                Section("IMAGE QUALITY");
                if (m.profile == 0) {
                    Hint("Base Game pins these to the console's own values. Switch "
                         "to GoldenEye+ in the header to change them.");
                    ImGui::Dummy(ImVec2(0, 10));
                }
                {
                    bool en = (m.profile != 0);
                    SliderRow("Supersampling", &m.supersample, 1, 2,    "x", vw, en);
                    SliderRow("MSAA",          &m.msaa,        0, 8,    "x", vw, en);
                    SliderRow("Anisotropic",   &m.aniso,       0, 16,   "x", vw, en);
                    SliderRow("Field of view", &m.fov,         50, 160, "%", vw, en);
                }
                Hint("Field of view is a percentage of the original. 100 is the N64's.");

                ImGui::Dummy(ImVec2(0, 8));
                ImGui::Checkbox("FXAA", &m.fxaa);
                Hint("Edge antialiasing over the finished frame. Cheaper than supersampling "
                     "and softer. Belongs here rather than with the CRT terms because it is "
                     "an image-quality choice, not a look.");

                /* Texture filtering, mipmapping and the pack controls were on the Windows
                 * tree's video page and had gone missing from this one, while the Model kept
                 * the fields they set. A launcher that holds a setting it cannot show is worse
                 * than one that never had it: the value round-trips through model_load and
                 * model_store on every launch, so it is live and invisible at the same time. */
                ImGui::Dummy(ImVec2(0, 10));
                ImGui::TextUnformatted("Texture filtering");
                ImGui::RadioButton("Nearest",     &m.filtering, 0); ImGui::SameLine(0, 24);
                ImGui::RadioButton("Bilinear",    &m.filtering, 1); ImGui::SameLine(0, 24);
                ImGui::RadioButton("Three-point", &m.filtering, 2);
                Hint("Three-point is what the N64's own RDP did: soft rather than blurry. "
                     "Bilinear is the standard modern filter. Nearest is the sharp, blocky "
                     "look with no filtering at all.");

                ImGui::Dummy(ImVec2(0, 6));
                ImGui::Checkbox("Mipmapping", &m.mipmaps);
                Hint("Blends distant textures toward a smaller mip level instead of "
                     "shimmering. Pairs with Anisotropic above, which sharpens the same "
                     "textures back up at grazing angles.");

                ImGui::Dummy(ImVec2(0, 6));
                ImGui::Checkbox("Widescreen", &m.widescreen);
                Hint("Fills the window at its own aspect ratio instead of the console's fixed "
                     "4:3, with a genuinely wider view of the level rather than a stretched "
                     "one. Off restores the original framing.");

                ImGui::Dummy(ImVec2(0, 10));
                ImGui::Checkbox("HD texture packs", &m.hd_textures);
                InputRow("Pack folder", m.texpack, sizeof m.texpack, 220);
                Hint("Replaces individual N64 textures with files from the pack folder, named "
                     "by content hash, wherever a match exists; anything not overridden renders "
                     "exactly as before. GETV_TEXPACK_DUMP writes the baseline to start a pack "
                     "from. No pack installed costs nothing.");

                ImGui::Dummy(ImVec2(0, 6));
                ImGui::Checkbox("Parallax from pack height maps", &m.parallax);
                Hint("Lets a pack's <hash>_h.png height maps displace the diffuse texture "
                     "coordinates. Does nothing without a pack that ships them, and there is "
                     "no height data in the game's own assets.");

                ImGui::Dummy(ImVec2(0, 10));
                SliderRow("Reticle size", &m.crosshair_scale_pct, 25, 200, "%", vw, true);
                Hint("100% is the retail sight exactly. The 1997 crosshair was sized for "
                     "320x240 on a CRT across a room; on a monitor it covers rather more of "
                     "what you are aiming at. GoldenEye+ asks for 60%.");

                ImGui::EndDisabled();
                Section("TIMING");
                ImGui::Checkbox("Uncapped (high refresh)", &m.uncapped);
                Hint("Removes the frame cap and switches to the real timebase together, "
                     "because on the default clock one rendered frame is one video field and "
                     "the world would simply run as fast as the renderer. Measured: 60.5 "
                     "fields a second, the correct value, at 456 fps. Costs reproducibility, "
                     "so measurement runs should stay capped.");
                ImGui::Dummy(ImVec2(0, 8));
                if (m.uncapped) {
                    ImGui::BeginDisabled();
                    int shown = 0;
                    SliderRow("Frame rate", &shown, 0, 60, " uncapped", vw, true);
                    ImGui::EndDisabled();
                } else {
                    SliderRow("Frame rate", &m.framerate, 30, 60, " fps", vw, true);
                    Hint("A cap above 60 is not offered: it either runs the game fast on the "
                         "default clock or is ignored on the real one. Uncapped above is the "
                         "high-refresh setting.");
                }
            }

            else if (page == 6) {
                float mw = ImGui::GetContentRegionAvail().x;

                Section("MOD DIRECTORY");
                InputRow("Folder", m.moddir, sizeof m.moddir, mw - 130.0f);
                ImGui::SameLine();
                {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(ImVec2(p.x + 10, p.y - 2));
                    if (Btn("RESCAN", ImVec2(110, 32), false)) { mod_scan(m); }
                }
                ImGui::Dummy(ImVec2(0, 8));
                Hint("Blank means ./mods beside the executable. A subdirectory counts as a mod "
                     "when it contains a mod.lua -- the same test the loader applies, so this "
                     "list is exactly what the game would load.");

                Section("INSTALLED");
                if (m.mod_count == 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, v4(kWarn));
                    ImGui::PushFont(g_fSmall);
                    ImGui::TextWrapped("Nothing found in %s", m.mod_scanned);
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0, 6));
                    Hint("Drop a folder containing a mod.lua in there and press Rescan.");
                } else {
                    int on = 0;
                    for (int i = 0; i < m.mod_count; i++) if (m.mod_on[i]) on++;

                    {
                        char hdr[256];
                        snprintf(hdr, sizeof hdr, "%d found in %s, %d enabled",
                                 m.mod_count, m.mod_scanned, on);
                        Hint(hdr);
                    }
                    ImGui::Dummy(ImVec2(0, 8));

                    if (Btn("ENABLE ALL", ImVec2(140, 32), false))
                        for (int i = 0; i < m.mod_count; i++) m.mod_on[i] = true;
                    ImGui::SameLine();
                    {
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::SetCursorScreenPos(ImVec2(p.x + 10, p.y));
                        if (Btn("DISABLE ALL", ImVec2(140, 32), false))
                            for (int i = 0; i < m.mod_count; i++) m.mod_on[i] = false;
                    }
                    ImGui::Dummy(ImVec2(0, 14));

                    /* One row per mod, full width and clickable across the whole row rather
                     * than only on the checkbox -- these are the primary control on this page
                     * and a 16px hit target for each would be the wrong way round. */
                    for (int i = 0; i < m.mod_count; i++) {
                        const float h = 40.0f;
                        ImVec2 p = ImGui::GetCursorScreenPos();

                        ImGui::PushID(i);
                        bool clicked = ImGui::InvisibleButton("mod", ImVec2(mw, h));
                        bool hov = ImGui::IsItemHovered();
                        ImGui::PopID();
                        if (clicked) m.mod_on[i] = !m.mod_on[i];

                        ImDrawList *ml = ImGui::GetWindowDrawList();
                        ml->AddRectFilled(p, ImVec2(p.x + mw, p.y + h),
                                          m.mod_on[i] ? mix(kPanel, kGold, 0.16f)
                                                      : (hov ? mix(kPanel2, kGold, 0.07f) : kPanel2));
                        ml->AddRect(p, ImVec2(p.x + mw, p.y + h),
                                    m.mod_on[i] ? kGold : kLine, 0.0f, 0, 1.0f);
                        if (m.mod_on[i])
                            ml->AddRectFilled(p, ImVec2(p.x + 3, p.y + h), kGoldHi);

                        /* Tick box drawn rather than an ImGui::Checkbox, so the whole row can
                         * be the hit target without two overlapping widgets fighting. */
                        ImVec2 b0(p.x + 16, p.y + h * 0.5f - 8.0f), b1(b0.x + 16, b0.y + 16);
                        ml->AddRect(b0, b1, m.mod_on[i] ? kGoldHi : kDim, 0.0f, 0, 1.0f);
                        if (m.mod_on[i]) ml->AddRectFilled(ImVec2(b0.x + 4, b0.y + 4),
                                                           ImVec2(b1.x - 4, b1.y - 4), kGoldHi);

                        ml->AddText(g_fBody, 18.0f, ImVec2(p.x + 48, p.y + h * 0.5f - 11.0f),
                                    m.mod_on[i] ? kText : kDim, m.mod_name[i]);

                        const char *state = m.mod_on[i] ? "ENABLED" : "DISABLED";
                        float sw = TextLSWidth(g_fSmall, 12.0f, state, 1.6f);
                        TextLS(g_fSmall, 12.0f, ImVec2(p.x + mw - sw - 14, p.y + h * 0.5f - 7.0f),
                               m.mod_on[i] ? kGold : kDim, state, 1.6f);

                        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 6.0f));
                        ImGui::Dummy(ImVec2(0, 0));
                    }

                    ImGui::Dummy(ImVec2(0, 10));
                    Hint("Disabled mods are recorded by name, so a mod added to the folder "
                         "later is enabled by default rather than silently ignored.");
                }
            }

            else if (page == 7) {
                static const char *notice = "";
                Section("RECORD PROP USAGE");
                ImGui::Checkbox("Record telemetry for this launch", &m.record_telemetry);
                Hint("Measure occupied object slots, peak usage and allocation failures. "
                     "Saves a local JSONL report and a readable summary. Nothing is uploaded.");
                const char *durations[] = { "3,600 frames (~1 minute at 60 FPS)",
                    "10,800 frames (~3 minutes at 60 FPS)", "18,000 frames (~5 minutes at 60 FPS)" };
                int duration = m.record_frames == 3600 ? 0 : m.record_frames == 18000 ? 2 : 1;
                ImGui::BeginDisabled(!m.record_telemetry);
                ImGui::SetNextItemWidth(360);
                if (ImGui::Combo("Recording length", &duration, durations, 3))
                    m.record_frames = duration == 0 ? 3600 : duration == 2 ? 18000 : 10800;
                ImGui::EndDisabled();
                Hint("The game closes automatically at the frame limit. Keyboard and mouse stay "
                     "active. Closing early leaves an incomplete report. Choose a mission before starting.");
                if (Btn("OPEN REPORTS", ImVec2(180, 32), false))
                    notice = developer_open_reports(false) ? "" : "Could not open the reports folder.";
                ImGui::SameLine();
                if (Btn("LATEST SUMMARY", ImVec2(200, 32), false))
                    notice = developer_open_reports(true) ? "" : "No summary found, or the file could not be opened.";
                if (*notice) Hint(notice);

                Section("DEBUGGING");
                ImGui::Checkbox("Show the developer overlay in game", &m.dev_overlay);
                ImGui::SetNextItemWidth(220);
                ImGui::InputText("Console hotkey", m.console_key, sizeof m.console_key);
                Hint("Default: grave/backquote. Close the console to return keyboard and mouse to the game.");
                const char *levels[] = { "Off", "Device diagnostics", "Detailed input diagnostics" };
                ImGui::SetNextItemWidth(280);
                ImGui::Combo("Input logging", &m.input_debug, levels, 3);
                ImGui::Checkbox("Log frame pacing", &m.pace_trace);
                Hint("Debug logs go to standard output, separate from saved telemetry reports. "
                     "Detailed logging can affect performance. These choices apply to this launch.");

                Section("HELP");
                if (Btn("GUIDE / FAQ ON GITHUB", ImVec2(280, 32), false))
                    notice = SDL_OpenURL("https://github.com/seb-patron/goldeneye-native/blob/main/docs/DEVELOPER_TOOLS.md") == 0
                        ? "" : "Could not open the browser.";
            }

            ImGui::EndDisabled();
            ImGui::EndChild();
            ImGui::PopStyleColor();

            /* ------------------------------------------------------------ footer */
            dl->AddRectFilled(ImVec2(0, H - footerH), ImVec2(W, H), kPanel);
            dl->AddLine(ImVec2(0, H - footerH), ImVec2(W, H - footerH), kLine, 1.0f);

            {
                /* What is actually about to be launched, in one line. The launcher composes a
                 * run out of five pages; without a summary the only way to check it is to go
                 * back through every page. */
                char sum[256];
                int n = 0;
                if (m.pick_stage) {
                    if (kStages[m.stage_idx].mission > 0)
                        n += snprintf(sum + n, sizeof sum - n, "MISSION %02d %s",
                                      kStages[m.stage_idx].mission, kStages[m.stage_idx].name);
                    else
                        n += snprintf(sum + n, sizeof sum - n, "ARENA %s",
                                      kStages[m.stage_idx].name);
                } else {
                    n += snprintf(sum + n, sizeof sum - n, "TITLE SCREEN");
                }
                n += snprintf(sum + n, sizeof sum - n, "   /   %s",
                              m.profile ? "GOLDENEYE+" : "BASE GAME");
                n += snprintf(sum + n, sizeof sum - n, "   /   %s",
                              m.rs_custom ? "CUSTOM GAMEPLAY" : kRulesets[m.ruleset]);
                if (m.horde) n += snprintf(sum + n, sizeof sum - n, "   /   HORDE");
                {
                    int nc = 0;
                    for (int i = 0; i < kCheatCount; i++) if (m.cheat_on[i]) nc++;
                    if (nc) snprintf(sum + n, sizeof sum - n, "   /   %d CHEAT%s",
                                     nc, nc == 1 ? "" : "S");
                }
                for (char *q = sum; *q; q++) *q = (char) toupper((unsigned char) *q);
                TextLS(g_fSmall, 13.0f, ImVec2(34, H - footerH + 31), kDim, sum, 1.6f);
            }

            ImGui::SetCursorScreenPos(ImVec2(W - 358, H - footerH + 21));
            if (Btn("QUIT", ImVec2(120, 36), false)) { running = false; }
            ImGui::SetCursorScreenPos(ImVec2(W - 222, H - footerH + 21));
#if defined(__APPLE__) && defined(GE_PLATFORM_MAC)
            ImGui::BeginDisabled(geAppRendererActive() && !geAppRendererAvailable(geAppRendererSelected()));
#endif
            if (Btn(m.pick_stage ? "START MISSION" : "START GAME", ImVec2(188, 36), true)) {
                if (developer_prepare_recording(m)) { launch = true; running = false; }
                else SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cannot start recording",
                    "The reports directory could not be prepared. Turn recording off or check folder access.", win);
            }

#if defined(__APPLE__) && defined(GE_PLATFORM_MAC)
            ImGui::EndDisabled();
#endif
            ImGui::End();
        }

        ImGui::Render();
#ifdef RAPI_METAL
        geLauncherMetalRenderAndPresent(ImGui::GetDrawData());
#else
        {
            /* The viewport is the DRAWABLE size, not io.DisplaySize.
             *
             * With SDL_WINDOW_ALLOW_HIGHDPI those two differ by the display's scale factor:
             * io.DisplaySize is logical (what SDL_GetWindowSize reports) while the framebuffer
             * is physical. This machine's panel runs at 150%, so a 1120x780 window has a
             * 1680x1170 drawable, and viewporting to the logical size drew the whole UI into
             * the bottom-left 2/3 of the buffer -- which reads as "zoomed in and cut off at
             * the right", not as a scaling bug, and is invisible at 100% scale. */
            int dw, dh;
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            glViewport(0, 0, dw, dh);
            glClearColor(0.031f, 0.035f, 0.043f, 1.0f);   /* kBg */
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        }
#endif

#ifdef RAPI_METAL
        /* GETV_LAUNCHER_SHOT and the pixel-counting half of GETV_LAUNCHER_PROBE are not
         * implemented on this backend (glReadPixels has no Metal equivalent wired up here
         * yet -- see ge_imgui.cpp's identical gap for GETV_IMGUI_PROBE). probe_frames still
         * closes the window after N frames, which is the half of the mechanism automated
         * testing needs most (proving the loop runs and exits, even without the pixel
         * check); it just cannot confirm what was drawn was non-empty. */
        if (probe_frames > 0 && ++probe_seen >= probe_frames) {
            if (getenv("GETV_LAUNCHER_SHOT") || getenv("GETV_LAUNCHER_PROBE")) {
                printf("[getv][launcher] GETV_LAUNCHER_SHOT/PROBE's pixel checks are not "
                       "implemented on the Metal backend -- closing after %d frames only\n",
                       probe_frames);
                fflush(stdout);
            }
            running = false;
        }
#else
        /* GETV_LAUNCHER_SHOT=<path.bmp> with GETV_LAUNCHER_PROBE=<frames>: write the drawable
         * to a file on the probe frame. An external screenshot tool cannot photograph this
         * window reliably -- the capturing process is DPI-unaware where this one is not, so
         * the rectangle it grabs is the virtualised one and the image comes back cropped.
         * Reading the framebuffer we just drew has no such ambiguity. */
        if (probe_frames > 0 && probe_seen + 1 >= probe_frames) {
            const char *shot = getenv("GETV_LAUNCHER_SHOT");
            if (shot != NULL && *shot != '\0') {
                int dw, dh;
                SDL_GL_GetDrawableSize(win, &dw, &dh);
                unsigned char *px = (unsigned char *) malloc((size_t) dw * (size_t) dh * 4);
                FILE *f = (px != NULL) ? fopen(shot, "wb") : NULL;
                if (px != NULL && f != NULL) {
                    glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    const int pad = (4 - (dw * 3) % 4) % 4;
                    const int dat = (dw * 3 + pad) * dh;
                    unsigned char hdr[54];
                    const unsigned char zero[3] = { 0, 0, 0 };
                    memset(hdr, 0, sizeof hdr);
                    hdr[0] = 'B'; hdr[1] = 'M';
                    *(int *) &hdr[2]  = 54 + dat;
                    *(int *) &hdr[10] = 54;
                    *(int *) &hdr[14] = 40;
                    *(int *) &hdr[18] = dw;
                    *(int *) &hdr[22] = dh;       /* positive: bottom-up, matching glReadPixels */
                    *(short *) &hdr[26] = 1;
                    *(short *) &hdr[28] = 24;
                    *(int *) &hdr[34] = dat;
                    fwrite(hdr, 1, 54, f);
                    for (int y = 0; y < dh; y++) {
                        for (int x = 0; x < dw; x++) {
                            const unsigned char *q = px + ((size_t) y * dw + x) * 4;
                            const unsigned char bgr[3] = { q[2], q[1], q[0] };
                            fwrite(bgr, 1, 3, f);
                        }
                        if (pad) fwrite(zero, 1, (size_t) pad, f);
                    }
                    printf("[getv][launcher] shot: %s (%dx%d)\n", shot, dw, dh);
                }
                if (f)  fclose(f);
                if (px) free(px);
            }
        }
        /* GETV_LAUNCHER_PROBE=<frames>: draw that many frames, count how many pixels differ
         * from the clear colour, report and close. The window blocks on a human, so without
         * this "the launcher renders" could only be asserted. Counting pixels distinguishes
         * a drawn UI from an empty window that merely failed to error, which is the actual
         * failure mode worth catching here. */
        if (probe_frames > 0 && ++probe_seen >= probe_frames) {
            int ww, wh, x, y, changed = 0;
            SDL_GL_GetDrawableSize(win, &ww, &wh);
            {
                unsigned char *px = (unsigned char *) malloc((size_t) ww * (size_t) wh * 4);
                if (px != NULL) {
                    glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    for (y = 0; y < wh; y++) {
                        for (x = 0; x < ww; x++) {
                            const unsigned char *q = px + ((size_t) y * ww + x) * 4;
                            /* the clear colour, 0.06/0.07/0.09 , is about 15/18/23 */
                            if (q[0] > 30 || q[1] > 30 || q[2] > 38) { changed++; }
                        }
                    }
                    free(px);
                }
            }
            printf("[getv][launcher] probe: %dx%d drawable, %d pixels above the clear "
                   "colour after %d frames\n", ww, wh, changed, probe_frames);
            running = false;
        }

        SDL_GL_SwapWindow(win);
#endif
    }

#ifdef RAPI_METAL
    geLauncherMetalDestroy();
#else
    ImGui_ImplOpenGL2_Shutdown();
#endif
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
#ifndef RAPI_METAL
    SDL_GL_DeleteContext(ctx);
#endif
    SDL_DestroyWindow(win);
    SDL_Quit();

    if (!launch) {
        printf("[getv][launcher] closed without starting the game\n");
        return 1;                       /* caller exits */
    }

    model_store(m);
    /* Persist the controls page on LAUNCH, not only when SAVE CONTROLS is pressed.
     *
     * The explicit button was not enough. Every other page in this launcher takes
     * effect by being handed to the relaunched game as environment variables, so
     * "change a binding, press launch, play" looks exactly like it worked -- and then
     * the remap is gone the next time the game starts cold, with nothing having said
     * so. Requiring a second, separate click to make a rebind permanent is a trap, and
     * a player who falls into it concludes that saving is broken rather than that they
     * missed a button.
     *
     * Launching is an explicit, deliberate act, which is what made the button-only
     * design defensible in the first place; this simply attaches the write to the act
     * the player already performs. The write itself is careful -- comments, ordering
     * and every other setting in the file survive -- so doing it more often is cheap.
     * The button stays, for saving without launching. */
    controls_save_to_config(m);
    printf("[getv][launcher] starting: profile=%s ruleset=%s%s%s\n",
           m.profile ? "goldeneye+" : "base-game",
           kRulesets[m.ruleset],
           m.horde ? " horde" : "",
           m.pick_stage ? "" : " (title screen)");
    relaunch();
    return 0;
}

#else  /* !GE_WITH_IMGUI */

/* Without ImGui there is nothing to draw the window with. Asking for the launcher says so
 * once and starts the game normally, rather than failing: --launcher must never be a way to
 * make a working binary refuse to run. */
extern "C" int gePortLauncherRun(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--launcher") == 0) {
            printf("[getv][launcher] this binary was built without ImGui.\n"
                   "[getv][launcher] run tools/fetch_imgui.sh, then ./getv/build_mac.sh all\n");
            break;
        }
    }
    return 0;
}

#endif /* GE_WITH_IMGUI */
