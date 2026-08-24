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
#include <errno.h>

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

/* Windows needs an extension loader before any GL header. opengl32.dll exports GL 1.1 and
 * nothing later, so glGetVertexAttribiv and everything else past 1997 resolves only through
 * GLEW -- without this the link fails on __imp_glGetVertexAttribiv, which reads like a
 * missing DLL import and is really a missing loader. gfx_opengl.c does exactly the same on
 * __MINGW32__; macOS and Linux need no equivalent. */
#if defined(_WIN32)
#define GLEW_STATIC
#include <GL/glew.h>
#endif

#if defined(USE_GLES)
#include <SDL2/SDL_opengles2.h>
#else
#include <SDL2/SDL_opengl.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

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

/* The stage list. Ids and names are from the table in CLAUDE.md, which is the project's
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
struct Cheat { const char *name; bool live; };
const Cheat kCheats[] = {
    { "invincibility", false }, { "all_guns",      false }, { "max_ammo",       false },
    { "infinite_ammo", true  }, { "dk_mode",       true  }, { "paintball",      true  },
    { "no_radar",      true  }, { "enemy_rockets", true  }, { "invisibility",   false },
    { "tiny_bond",     false }, { "golden_gun",    false }, { "magnum",         false },
    { "laser",         false }, { "turbo_mode",    false }, { "10x_health",     false },
    { "2x_armor",      false }, { "extra_weapons", false }, { "fast_animation", false },
};
const int kCheatCount = (int)(sizeof kCheats / sizeof kCheats[0]);

const char *kRulesets[] = { "classic", "hardcore", "survival", "chaos", "horde" };
const int   kRulesetCount = 5;

struct Model {
    /* profile */
    int  profile;             /* 0 faithful, 1 GoldenEye+ */

    /* ruleset */
    int  ruleset;             /* index into kRulesets */
    bool rs_custom;           /* show and send the nine individual percentages */
    int  enemy_health, enemy_damage, enemy_accuracy, enemy_reaction;
    int  player_health, player_armour, ammo, explosion_damage, turret_damage;

    /* horde */
    bool horde;
    int  horde_per_kill, horde_per_kill_cap, horde_max_alive;
    int  horde_wave_kills, horde_growth;

    /* level */
    bool pick_stage;
    int  stage_idx;

    /* video */
    int  supersample, fov, framerate, msaa, aniso;
    bool fullscreen;
    char resolution[64];

    /* misc */
    bool cheat_on[kCheatCount];
    bool dev_overlay;
    char moddir[512];
};

void model_load(Model &m)
{
    memset(&m, 0, sizeof m);

    m.profile = env_bool("GETV_PROFILE_PLUS", false) ? 1 : 0;

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
    m.horde_per_kill     = env_int("GETV_HORDE_PER_KILL", 1);
    m.horde_per_kill_cap = env_int("GETV_HORDE_PER_KILL_CAP", 3);
    m.horde_max_alive    = env_int("GETV_HORDE_MAX_ALIVE", 12);
    m.horde_wave_kills   = env_int("GETV_HORDE_WAVE_KILLS", 10);
    m.horde_growth       = env_int("GETV_HORDE_GROWTH", 1);

    {
        const char *st = getenv("GETV_STAGE");
        m.pick_stage = (st != NULL && *st != '\0');
        m.stage_idx = 0;
        if (m.pick_stage) {
            int id = atoi(st);
            for (int i = 0; i < kStageCount; i++) {
                if (kStages[i].id == id) { m.stage_idx = i; break; }
            }
        }
    }

    m.supersample = env_int("GETV_SUPERSAMPLE", 1);
    m.fov         = env_int("GETV_FOV", 100);
    m.framerate   = env_int("GETV_FPS", 60);
    m.msaa        = env_int("GETV_MSAA", 0);
    m.aniso       = env_int("GETV_ANISO", 0);
    m.fullscreen  = env_bool("GETV_FULLSCREEN", false);
    env_str("GETV_WINDOW", m.resolution, sizeof m.resolution, "1280x960");

    {
        const char *c = getenv("GETV_CHEATS");
        if (c && *c) {
            for (int i = 0; i < kCheatCount; i++) {
                const char *p = strstr(c, kCheats[i].name);
                if (p) m.cheat_on[i] = true;
            }
        }
    }
    m.dev_overlay = env_bool("GETV_IMGUI", false);
    env_str("GETV_MODDIR", m.moddir, sizeof m.moddir, "");
}

void model_store(const Model &m)
{
    setenv("GETV_PROFILE_PLUS", m.profile ? "1" : "0", 1);
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
        put_int("GETV_HORDE_PER_KILL",     m.horde_per_kill);
        put_int("GETV_HORDE_PER_KILL_CAP", m.horde_per_kill_cap);
        put_int("GETV_HORDE_MAX_ALIVE",    m.horde_max_alive);
        put_int("GETV_HORDE_WAVE_KILLS",   m.horde_wave_kills);
        put_int("GETV_HORDE_GROWTH",       m.horde_growth);
    }

    if (m.pick_stage) put_int("GETV_STAGE", kStages[m.stage_idx].id);
    else              unsetenv("GETV_STAGE");

    put_int("GETV_SUPERSAMPLE", m.supersample);
    put_int("GETV_FOV",         m.fov);
    put_int("GETV_FPS",         m.framerate);
    put_int("GETV_MSAA",        m.msaa);
    put_int("GETV_ANISO",       m.aniso);
    setenv("GETV_FULLSCREEN", m.fullscreen ? "1" : "0", 1);
    put_str("GETV_WINDOW", m.resolution);

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

    setenv("GETV_IMGUI", m.dev_overlay ? "1" : "0", 1);
    put_str("GETV_MODDIR", m.moddir);
}

/* GoldenEye+ is a profile over the same gates, not a fork. It turns on what this port has
 * added and verified; it does not enable anything inert. Faithful clears the same set rather
 * than merely not setting it, so switching back is symmetric and cannot leave a stray
 * enhancement behind. */
void apply_profile(Model &m)
{
    if (m.profile == 1) {
        m.fov         = (m.fov < 100) ? 100 : m.fov;
        m.msaa        = (m.msaa  < 4) ? 4 : m.msaa;
        m.aniso       = (m.aniso < 8) ? 8 : m.aniso;
        m.supersample = (m.supersample < 2) ? 2 : m.supersample;
    } else {
        m.msaa = 0;
        m.aniso = 0;
        m.supersample = 1;
        m.fov = 100;
        m.ruleset = 0;
        m.rs_custom = false;
        m.horde = false;
    }
}

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

    /* The environment must stop asking for the launcher too, or a GETV_LAUNCHER=1 set in
     * goldeneye.cfg would survive into the child and open the launcher again, forever. The
     * flag is stripped from argv below for the same reason; both routes in have to be shut,
     * not just the one that happens to have been used this time. */
    unsetenv("GETV_LAUNCHER");
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
#else
    execv(exe, nv);
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

/* A labelled slider that reads as a row rather than as an ImGui widget: name on the left,
 * value on the right, bar underneath. */
void Row(const char *label, int *v, int lo, int hi, const char *suffix)
{
    ImGui::PushID(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;

    ImGui::GetWindowDrawList()->AddText(g_fBody, 17.0f, ImVec2(p.x, p.y), kText, label);
    char val[32];
    snprintf(val, sizeof val, "%d%s", *v, suffix ? suffix : "");
    float vw = g_fBody->CalcTextSizeA(17.0f, FLT_MAX, 0.0f, val).x;
    ImGui::GetWindowDrawList()->AddText(g_fBody, 17.0f, ImVec2(p.x + w - vw, p.y),
                                        (*v == 100) ? kDim : kGoldHi, val);
    ImGui::Dummy(ImVec2(0, 22));

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 3));
    ImGui::SetNextItemWidth(w);
    ImGui::SliderInt("##s", v, lo, hi, "");
    ImGui::PopStyleVar();
    ImGui::PopID();
}

} /* namespace */

extern "C" int gePortLauncherRun(int argc, char **argv)
{
    bool wanted = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--launcher") == 0) { wanted = true; break; }
    }
    if (!wanted && env_bool("GETV_LAUNCHER", false)) wanted = true;
    /* Autoplay implies the launcher. Without this it is unreachable, because the check above
     * would return before ever looking at it -- which is exactly what happened the first time
     * it was tested, and the run looked like a pass because the environment being asserted on
     * had been set by hand for the test anyway. */
    if (!wanted && env_bool("GETV_LAUNCHER_AUTOPLAY", false)) wanted = true;
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
        model_store(am);
        printf("[getv][launcher] autoplay: profile=%s ruleset=%s%s\n",
               am.profile ? "goldeneye+" : "faithful",
               kRulesets[am.ruleset], am.horde ? " horde" : "");
        relaunch();
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("[getv][launcher] SDL_Init failed: %s\n", SDL_GetError());
        return 0;                      /* fall through to the game rather than refusing to start */
    }

    /* A plain, small, resizable window. No GL attributes are requested beyond a double
     * buffer: this context exists only to draw ImGui, and asking for depth/MSAA here would
     * be asking for the game's settings in a window that is not the game. */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window *win = SDL_CreateWindow("GoldenEye 007",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1120, 780,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI |
                                       SDL_WINDOW_RESIZABLE);
    /* The mission list needs vertical room and the two-column pages need width; below this
     * the layout starts overlapping rather than reflowing. */
    if (win != NULL) SDL_SetWindowMinimumSize(win, 960, 640);
    if (win == NULL) {
        printf("[getv][launcher] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (ctx == NULL) {
        printf("[getv][launcher] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 0;
    }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ge_apply_style();
    /* Before the first NewFrame: the backend builds the atlas texture there, and a font
     * added afterwards would not be in it. */
    ge_load_fonts();
    /* imgui_impl_opengl2, matching ge_imgui.cpp: this build takes macOS's legacy context and
     * the GL3 backend calls glGenVertexArrays, which a 2.1 context does not have. */
    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL2_Init();

    Model m;
    model_load(m);

    bool running = true;
    bool launch  = false;
    const int probe_frames = env_int("GETV_LAUNCHER_PROBE", 0);
    int probe_seen = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(win)) running = false;
        }

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        {
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2((float) ww, (float) wh));
            ImGui::Begin("GoldenEye", NULL,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

            ImGui::TextUnformatted("GoldenEye 007  --  native");
            ImGui::Separator();

            int prev_profile = m.profile;
            ImGui::TextUnformatted("Profile");
            ImGui::RadioButton("Faithful", &m.profile, 0);
            ImGui::SameLine();
            ImGui::RadioButton("GoldenEye+", &m.profile, 1);
            if (m.profile != prev_profile) apply_profile(m);
            ImGui::TextDisabled("%s", m.profile == 1
                ? "Enhancements on: wider FOV allowed, MSAA, anisotropic, supersampling."
                : "The game as shipped. Enhancements off.");

            ImGui::Spacing();
            if (ImGui::BeginTabBar("tabs")) {

                if (ImGui::BeginTabItem("Play")) {
                    ImGui::Checkbox("Start on a specific level", &m.pick_stage);
                    if (m.pick_stage) {
                        if (ImGui::BeginCombo("Level", kStages[m.stage_idx].name)) {
                            for (int i = 0; i < kStageCount; i++) {
                                bool sel = (i == m.stage_idx);
                                char label[64];
                                snprintf(label, sizeof label, "%s%s", kStages[i].name,
                                         kStages[i].mp_only ? "  (multiplayer only)" : "");
                                if (ImGui::Selectable(label, sel)) m.stage_idx = i;
                                if (sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        if (kStages[m.stage_idx].mp_only) {
                            ImGui::TextDisabled("This level has no solo setup. Started alone it "
                                                "loads geometry with no props or spawn.");
                        }
                    } else {
                        ImGui::TextDisabled("Boots to the title screen.");
                    }

                    ImGui::Spacing();
                    ImGui::TextUnformatted("Mods");
                    ImGui::InputText("Mod directory", m.moddir, sizeof m.moddir);
                    ImGui::TextDisabled("Each subdirectory with a mod.lua is loaded. "
                                        "Blank means ./mods.");
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Rules")) {
                    if (ImGui::BeginCombo("Ruleset", kRulesets[m.ruleset])) {
                        for (int i = 0; i < kRulesetCount; i++) {
                            bool sel = (i == m.ruleset);
                            if (ImGui::Selectable(kRulesets[i], sel)) m.ruleset = i;
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::Checkbox("Customise", &m.rs_custom);
                    if (m.rs_custom) {
                        ImGui::TextDisabled("Percentages. 100 is unmodified. "
                                            "These override the preset.");
                        ImGui::SliderInt("Enemy health",   &m.enemy_health,   10, 500);
                        ImGui::SliderInt("Enemy damage",   &m.enemy_damage,   10, 500);
                        ImGui::SliderInt("Enemy accuracy", &m.enemy_accuracy, 10, 500);
                        ImGui::SliderInt("Enemy reaction", &m.enemy_reaction, 10, 500);
                        ImGui::SliderInt("Player health",  &m.player_health,  10, 500);
                        ImGui::SliderInt("Player armour",  &m.player_armour,  10, 500);
                        ImGui::SliderInt("Ammo",           &m.ammo,           10, 500);
                        ImGui::SliderInt("Explosions",     &m.explosion_damage, 10, 500);
                        ImGui::SliderInt("Turrets",        &m.turret_damage,  10, 500);
                    }

                    ImGui::Spacing();
                    ImGui::Checkbox("Horde mode", &m.horde);
                    if (m.horde) {
                        ImGui::TextDisabled("When a guard dies, replacements spawn where it "
                                            "fell. Waves grow with kills.");
                        ImGui::SliderInt("Spawns per kill", &m.horde_per_kill, 0, 8);
                        ImGui::SliderInt("Per-kill cap",    &m.horde_per_kill_cap, 1, 8);
                        ImGui::SliderInt("Max alive",       &m.horde_max_alive, 1, 64);
                        ImGui::SliderInt("Kills per wave",  &m.horde_wave_kills, 1, 100);
                        ImGui::SliderInt("Growth per wave", &m.horde_growth, 0, 8);
                        ImGui::TextDisabled("The engine refuses to spawn with fewer than three "
                                            "free guard slots, so the real ceiling belongs to "
                                            "the level.");
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Cheats")) {
                    ImGui::TextDisabled("The game's own cheats. Ones marked (in-game) need "
                                        "activating from the pause menu -- their effect lives "
                                        "in the turn-on switch, which needs a player.");
                    ImGui::Spacing();
                    for (int i = 0; i < kCheatCount; i++) {
                        char label[64];
                        snprintf(label, sizeof label, "%s%s", kCheats[i].name,
                                 kCheats[i].live ? "" : "  (in-game)");
                        ImGui::Checkbox(label, &m.cheat_on[i]);
                        if ((i % 2) == 0 && i + 1 < kCheatCount) ImGui::SameLine(300);
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Video")) {
                    ImGui::InputText("Resolution", m.resolution, sizeof m.resolution);
                    ImGui::TextDisabled("WIDTHxHEIGHT, minimum 320x240.");
                    ImGui::Checkbox("Fullscreen", &m.fullscreen);
                    ImGui::SliderInt("Supersample", &m.supersample, 1, 2);
                    ImGui::SliderInt("MSAA", &m.msaa, 0, 8);
                    ImGui::SliderInt("Anisotropic", &m.aniso, 0, 16);
                    ImGui::SliderInt("Field of view", &m.fov, 50, 160);
                    ImGui::TextDisabled("Percent of the original. 100 is the N64's.");
                    ImGui::Spacing();
                    ImGui::SliderInt("Frame rate", &m.framerate, 30, 60);
                    if (m.framerate > 60) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                           "Above 60 the game runs fast: logic is tied to the "
                                           "frame step.");
                    }
                    ImGui::Spacing();
                    ImGui::Checkbox("Developer overlay", &m.dev_overlay);
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::Separator();
            if (ImGui::Button("Play", ImVec2(160, 34))) { launch = true; running = false; }
            ImGui::SameLine();
            if (ImGui::Button("Quit", ImVec2(160, 34))) { running = false; }
            ImGui::SameLine();
            ImGui::TextDisabled("Settings are applied by restarting the game with them.");

            ImGui::End();
        }

        ImGui::Render();
        {
            ImGuiIO &io = ImGui::GetIO();
            glViewport(0, 0, (int) io.DisplaySize.x, (int) io.DisplaySize.y);
            glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
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
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();

    if (!launch) {
        printf("[getv][launcher] closed without starting the game\n");
        return 1;                       /* caller exits */
    }

    model_store(m);
    printf("[getv][launcher] starting: profile=%s ruleset=%s%s%s\n",
           m.profile ? "goldeneye+" : "faithful",
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
