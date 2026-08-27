/* GoldenEye tvOS port - the host services Fast3D expects.
 *
 * sm64ex supplies these from its own platform/config/filesystem layers.
 * GoldenEye's decomp has none of that, so the port provides the minimum Fast3D
 * actually touches. Kept deliberately small: everything here is host plumbing, not
 * game behaviour.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include <SDL.h>

#include "../platform.h"
#include "../configfile.h"
#include "../fs/fs.h"
#include "../pc_main.h"
#include "../fast3d/gfx_window_manager_api.h"   /* WAPI_WIN_CENTERPOS */

/* ---- platform ---------------------------------------------------------- */

void sys_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[getv] FATAL: ");
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    /* stdout to `devicectl --console` is block-buffered, so without this flush the
     * message explaining the crash is exactly the part that gets lost. This cost
     * real time on Perfect Dark, where a SIGABRT presented as a clean exit 0. */
    fflush(stdout);
    abort();
}

/* The argument is microseconds, not seconds. sm64ex's only caller is
 * sync_framerate_with_timer() in gfx_sdl2.c, which passes
 * `remain / perf_freq * 1000000.0`. Treating it as seconds multiplies every wait by a
 * million: a 16 ms frame pace becomes SDL_Delay(16000000), about four and a half hours.
 * Frame 0 runs long enough to skip the sleep entirely, so the port appears healthy
 * until frame 1 renders quickly and hangs inside gfx_end_frame(). */
void sys_sleep(double us)
{
    if (us > 0.0) {
        SDL_Delay((Uint32)(us / 1000.0));
    }
}

/* ---- host timebase ------------------------------------------------------ */

/* The N64 count register, synthesised from the host clock, at its real 46.875 MHz
 * rate. Called only by osGetCount() under GETV_REALCLOCK=1 -- see the long note there
 * for why that is opt-in. It lives HERE rather than in port_os.c purely because
 * port_os.c includes <PR/os.h>, which cannot coexist with <string.h> (and therefore
 * not with <SDL.h>) in either order.
 *
 * Reduce against an origin before scaling. arm64 macOS's performance counter is in the
 * nanosecond domain and is already far past 2^32 at boot, so scaling the absolute value
 * loses all precision; measuring from the first call keeps the product small and exact.
 * The u32 truncation that remains is the same wrap the hardware register had. */
unsigned int gePortHostN64Count(void)
{
    static Uint64 origin = 0;
    static double freq   = 0.0;
    Uint64 now;

    if (freq == 0.0) {
        freq   = (double)SDL_GetPerformanceFrequency();
        origin = SDL_GetPerformanceCounter();
        if (freq <= 0.0) { freq = 1.0; }
    }
    now = SDL_GetPerformanceCounter();
    return (unsigned int)(unsigned long long)(((double)(now - origin) / freq) * 46875000.0);
}

/* ---- config ------------------------------------------------------------ */

/* tvOS has exactly one display mode, 1920x1080, and no windowing. These values are
 * therefore fixed rather than read from a config file.
 *
 * macOS is the one platform on this port that has a window, and that is the reason the
 * Mac target exists -- a window can be looked at, screenshotted and played. So it
 * starts windowed and resizable rather than seizing the display. Override with
 * GETV_WINDOW=WxH, or GETV_FULLSCREEN=1. */
#ifdef GE_PLATFORM_DESKTOP
ConfigWindow configWindow = {
    .x = (unsigned)WAPI_WIN_CENTERPOS, .y = (unsigned)WAPI_WIN_CENTERPOS,
    .w = 1280, .h = 960,          /* 4:3, the aspect the game was authored for */
    .vsync = true,
    .reset = false,
    .fullscreen = false,
    .exiting_fullscreen = false,
    .settings_changed = false,
};

/* Called from gePortMacWindowConfig() below, before gfx_init(). Deliberately env-driven
 * rather than a config file: this port has no settings UI and every other knob on it is
 * a GETV_* variable. */
void gePortMacWindowConfig(void)
{
    const char *w = getenv("GETV_WINDOW");
    const char *f = getenv("GETV_FULLSCREEN");
    if (w != NULL && *w != '\0') {
        unsigned ww = 0, hh = 0;
        if (sscanf(w, "%ux%u", &ww, &hh) == 2 && ww >= 320 && hh >= 240) {
            configWindow.w = ww;
            configWindow.h = hh;
        }
    }
    if (f != NULL && *f == '1') {
        configWindow.fullscreen = true;
    }

    /* Clamp the default to a window that fits. 1280x960 is bigger than the usable area
     * of a 13" laptop panel once the menu bar and Dock are subtracted, and an oversized
     * SDL window on macOS comes up with its title bar under the menu bar and its bottom
     * off-screen, leaving a window that cannot be moved. Shrink by whole 4:3 steps so
     * the aspect the game was authored for is preserved rather than letterboxed.
     *
     * Only the default is clamped. An explicit GETV_WINDOW is honoured as given:
     * measurement runs deliberately ask for sizes larger than the panel, and `drawn` is
     * resolution-sensitive on Mac (659 at <=1280x960, 672/673 at >=1600x1200), so
     * silently resizing a requested size would corrupt a comparison. */
    if ((w == NULL || *w == '\0') && !configWindow.fullscreen) {
        SDL_Rect usable;
        /* Idempotent: gfx_sdl_init() calls SDL_Init(SDL_INIT_VIDEO) again and SDL
         * reference-counts subsystems, so this does not disturb the normal path. */
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0 &&
            SDL_GetDisplayUsableBounds(0, &usable) == 0 &&
            usable.w > 0 && usable.h > 0) {
            static const unsigned steps[][2] = {
                { 1280, 960 }, { 1024, 768 }, { 800, 600 }, { 640, 480 }
            };
            size_t i;
            for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
                /* Leave room for the title bar; SDL's usable bounds do not subtract it. */
                if (steps[i][0] <= (unsigned)usable.w &&
                    steps[i][1] + 28u <= (unsigned)usable.h) {
                    break;
                }
            }
            if (i >= sizeof(steps) / sizeof(steps[0])) { i = sizeof(steps) / sizeof(steps[0]) - 1; }
            if (steps[i][0] != configWindow.w || steps[i][1] != configWindow.h) {
                printf("[getv] window: %ux%u would not fit usable %dx%d -- using %ux%u\n",
                       configWindow.w, configWindow.h, usable.w, usable.h,
                       steps[i][0], steps[i][1]);
            }
            configWindow.w = steps[i][0];
            configWindow.h = steps[i][1];
        }
    }

    printf("[getv] window: %ux%u %s, resizable; fullscreen toggle = F11 / Cmd-F / Alt-Enter\n",
           configWindow.w, configWindow.h,
           configWindow.fullscreen ? "fullscreen" : "windowed");
    fflush(stdout);
}
#else
ConfigWindow configWindow = {
    .x = 0, .y = 0, .w = 1920, .h = 1080,
    .vsync = true,
    .reset = false,
    .fullscreen = true,
    .exiting_fullscreen = false,
    .settings_changed = false,
};
#endif

/* 0 = nearest, 1 = bilinear, 2 = three-point. Three-point is what the N64 actually
 * did and what looked right on Perfect Dark.
 *
 * GETV_FILTERING=0|1|2 overrides the compiled-in default below, read once via a GCC/clang
 * constructor rather than a lazy-static check at the read sites -- gfx_opengl.c and gfx_pc.c
 * both read this global directly with no accessor to hang a check off, and both run before
 * any explicit port-init call this file could otherwise piggyback on. Constructors run before
 * main() on every platform this project targets, so "before the first read" is guaranteed
 * without needing to find or disturb an existing init sequence. */
unsigned int configFiltering = 2;

__attribute__((constructor))
static void ge_filtering_env_init(void)
{
    const char *e = getenv("GETV_FILTERING");
    if (e && *e >= '0' && *e <= '2' && e[1] == '\0') {
        configFiltering = (unsigned int) (*e - '0');
    }
}

/* 1 = the rendered scene fills the real window at its real aspect; 0 = retail behaviour,
 * pillarboxed/letterboxed to the game's native 4:3 (gfx_pc.c's ge_scale()/ge_offset_*()
 * picking the smaller of the two axis scales). Defaults on: the pillarbox was never a
 * deliberate user-facing choice, just what happens when nothing corrects for a non-4:3
 * window, and it is the thing GETV_WIDESCREEN=0 is for undoing on request.
 *
 * Same env-var-via-constructor approach as configFiltering above, for the same reason:
 * gfx_pc.c reads this global directly at multiple call sites with no accessor to hang a
 * lazy check off. */
unsigned int configWidescreen = 1;

__attribute__((constructor))
static void ge_widescreen_env_init(void)
{
    const char *e = getenv("GETV_WIDESCREEN");
    if (e && *e >= '0' && *e <= '1' && e[1] == '\0') {
        configWidescreen = (unsigned int) (*e - '0');
    }
}

/* GETV_CROSSHAIR_COLOR=RRGGBB -- gunfire.c's gunDrawSight() passes this straight through as
 * the RDP primitive colour it multiplies crosshairimage's decoded texels by, in place of the
 * retail 0xFF,0xFF,0xFF ("show the texture's own colour unmodified"). Defaults to white,
 * byte-for-byte the current behaviour, so an unset variable changes nothing.
 *
 * How cleanly it recolours depends on the baked N64 asset under the tint, which has not been
 * confirmed here: a white or grey source recolours cleanly under a multiply, a source with its
 * own baked hue only partially. This ships the mechanism the 1997 code already exposed at that
 * call site rather than blocking on resolving it, and the launcher's colour picker makes the
 * real answer visible immediately, which is the more direct check anyway. */
unsigned char ge_crosshair_r = 0xFF;
unsigned char ge_crosshair_g = 0xFF;
unsigned char ge_crosshair_b = 0xFF;

__attribute__((constructor))
static void ge_crosshair_color_env_init(void)
{
    const char *e = getenv("GETV_CROSSHAIR_COLOR");
    if (e && *e) {
        unsigned int r, g, b;
        if (sscanf(e, "%2x%2x%2x", &r, &g, &b) == 3) {
            ge_crosshair_r = (unsigned char) r;
            ge_crosshair_g = (unsigned char) g;
            ge_crosshair_b = (unsigned char) b;
        }
    }
}

/* 1 = ge_upload_texture() (gfx_pc.c) checks GETV_TEXPACK for an override of every texture
 * before uploading the N64 decoder's own output; 0 = never checks, byte-for-byte the
 * current behaviour. Defaults OFF, unlike configFiltering/configWidescreen above -- both
 * of those were verified by tracing the exact call order and, for widescreen, by working
 * through the arithmetic that proves ge_scale() collapses to a single uniform factor. This
 * one has had no such verification pass, because there has been no compiler available to
 * run one against: it was written and reasoned through, not measured. An empty
 * GETV_TEXPACK makes it a no-op regardless, so turning it on costs one failed file lookup
 * per unique texture rather than anything worse -- but "costs little if wrong" is not the
 * same claim as "verified correct", and this stays opt-in until it has actually been run. */
unsigned int configHDTextures = 0;

__attribute__((constructor))
static void ge_hdtextures_env_init(void)
{
    const char *e = getenv("GETV_HD_TEXTURES");
    if (e && *e >= '0' && *e <= '1' && e[1] == '\0') {
        configHDTextures = (unsigned int) (*e - '0');
    }
}

/* ---- filesystem -------------------------------------------------------- */

/* Pack format, chosen: a flat directory of override texture files, named by content
 * hash -- ge_texhash() in gfx_pc.c hashes the raw N64 texel bytes plus fmt/siz, so the
 * name is stable across runs and independent of where the ROM data happens to sit in
 * memory. GETV_TEXPACK_DUMP=<dir> (also gfx_pc.c; not GETV_TEXDUMP, which is already
 * image.c's own unrelated byte-count debug gate) writes a same-named .ppm baseline the
 * first time each texture is decoded, so a pack starts life as a copy of a dump with
 * individual files replaced by an upscaler, not as a guessing game over what to name things.
 *
 * GETV_TEXPACK names the root explicitly; "hdtextures" is the default so a pack folder
 * dropped next to the executable with no configuration at all is picked up. Same
 * GETV_EXEDIR fallback as gePortLuaInit() (ge_lua.c) and ge_config.c's own config-file
 * search, and for the same reason: a relative path is tried against the working directory
 * first (an explicit relative GETV_TEXPACK should mean what the user typed, standing in
 * the right place), then against the executable's own directory, which is where a
 * distributed folder's pack actually lives when launched from a shortcut or from
 * somewhere else entirely. No pack directory is the normal case -- most players have not
 * installed one -- and stays silent rather than logging on every missed lookup. */
static const char *ge_texpack_dir(void)
{
    static char resolved[1024];
    static const char *dir;
    static int done;
    struct stat st;

    if (done) return dir;
    done = 1;

    dir = getenv("GETV_TEXPACK");
    if (dir == NULL || *dir == '\0') dir = "hdtextures";

    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return dir;

    if (dir[0] != '/' && dir[0] != '\\' && !(dir[0] != '\0' && dir[1] == ':')) {
        const char *exedir = getenv("GETV_EXEDIR");
        if (exedir != NULL && *exedir != '\0') {
            snprintf(resolved, sizeof(resolved), "%s/%s", exedir, dir);
            if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
                dir = resolved;
                return dir;
            }
        }
    }

    dir = NULL;
    return dir;
}

/* Fast3D's own EXTERNAL_DATA path asks for FS_TEXTUREDIR ("gfx") specifically and expects
 * a walk to recurse into it; this pack has no such structure, it is one flat folder of
 * <hash>.<ext> files, so a `base` naming any subdirectory legitimately finds nothing. */
fs_walk_result_t fs_walk(const char *base, walk_fn_t walkfn, void *user, const bool recur)
{
    const char *root = ge_texpack_dir();
    char path[1280];
    DIR *d;
    struct dirent *e;
    fs_walk_result_t result = FS_WALK_SUCCESS;

    (void)recur;   /* flat directory, nothing to recurse into */
    if (root == NULL) return FS_WALK_NOTFOUND;

    if (base != NULL && *base != '\0' && strcmp(base, ".") != 0) {
        snprintf(path, sizeof(path), "%s/%s", root, base);
    } else {
        snprintf(path, sizeof(path), "%s", root);
    }

    d = opendir(path);
    if (d == NULL) return FS_WALK_NOTFOUND;

    while ((e = readdir(d)) != NULL) {
        char full[1536];
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (!walkfn(user, full)) {
            result = FS_WALK_INTERRUPTED;
            break;
        }
    }
    closedir(d);
    return result;
}

void *fs_load_file(const char *vpath, uint64_t *outsize)
{
    const char *root = ge_texpack_dir();
    char path[1280];
    FILE *f;
    long size;
    void *buf;

    if (outsize) *outsize = 0;
    if (root == NULL || vpath == NULL || *vpath == '\0') return NULL;

    /* fs_walk() above hands its callback full paths under the resolved root; a caller
     * that turns around and loads one of those (rather than a bare "<hash>.png") is
     * passing an already-rooted path back in, so an absolute vpath is used as-is instead
     * of being joined onto root a second time. */
    if (vpath[0] == '/' || vpath[0] == '\\' || (vpath[0] != '\0' && vpath[1] == ':')) {
        snprintf(path, sizeof(path), "%s", vpath);
    } else {
        snprintf(path, sizeof(path), "%s/%s", root, vpath);
    }

    f = fopen(path, "rb");
    if (f == NULL) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    buf = malloc((size_t) size);
    if (buf == NULL) { fclose(f); return NULL; }

    if (size > 0 && fread(buf, 1, (size_t) size, f) != (size_t) size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    if (outsize) *outsize = (uint64_t) size;
    return buf;
}

/* ---- shutdown ---------------------------------------------------------- */

/* Fast3D's SDL backend calls these when the OS asks the app to quit. There is no
 * game state to tear down yet -- the decomp's own boot path is not wired -- so the
 * harness just leaves cleanly. */
void game_deinit(void) { }

void game_exit(void)
{
    printf("[getv] game_exit requested\n");
    fflush(stdout);
    exit(0);
}

/* ---- input ------------------------------------------------------------- */

/* The Apple TV has no keyboard; Fast3D's SDL backend registers these regardless.
 * Returning false means "not handled", which is correct here. Gamepad input goes
 * through SDL's joystick/GameController path when the game is wired up. */
bool keyboard_on_key_down(int scancode) { (void)scancode; return false; }
bool keyboard_on_key_up(int scancode)   { (void)scancode; return false; }
void keyboard_on_all_keys_up(void)      { }

/* ---- boot tracing (temporary) ------------------------------------------- */

/* Called from bossInitMainthreadData() before each init step so the device console
 * shows exactly how far startup gets. Remove along with the calls in boss.c once the
 * boot path completes. */
/* Per-tag print budget for gePortBootMark(); see the note there. Tags are string
 * literals, so comparing by pointer is both correct and cheap. */
const char *ge_last_mark = "(none)";
unsigned long ge_mark_seq = 0;

#define GE_MARK_REPEATS 3
#define GE_MARK_MAX     512

static int ge_mark_should_print(const char *what)
{
    static const char *tags[GE_MARK_MAX];
    static int hits[GE_MARK_MAX];
    static int ntags = 0;
    int i;

    for (i = 0; i < ntags; i++) {
        if (tags[i] == what) {
            if (hits[i] < GE_MARK_REPEATS) {
                hits[i]++;
                if (hits[i] == GE_MARK_REPEATS) {
                    printf("[getv] (further '%s' marks suppressed)\n", what);
                }
                return 1;
            }
            return 0;
        }
    }
    if (ntags < GE_MARK_MAX) {
        tags[ntags] = what;
        hits[ntags] = 1;
        ntags++;
    }
    return 1;
}

void gePortBootMark(const char *what)
{
    /* Checking the stub canaries here is what makes a stub overflow findable. The
     * g_Props overrun corrupted the memory-pool bank table and only surfaced, three
     * subsystems later, as a silent `while(1);` inside mempAllocBytesInBank. Checking
     * on every boot mark names both the offending symbol and the step that did it. */
    extern const char *gePortStubCheck(void);
    static int reported = 0;
    const char *bad;

    /* Rate-limited. Unconditional marks print on every frame once the boot path reaches
     * the frame loop, which produced multi-gigabyte logs from runs of a couple of
     * minutes. Each distinct tag prints its first GE_MARK_REPEATS occurrences and is
     * then suppressed, keeping the one-shot boot trace intact while making a
     * steady-state loop cost nothing.
     *
     * Every mark is still recorded, even a suppressed one. Rate-limiting keeps the log
     * small but would otherwise hide where a crash happened once the frame loop is
     * running, since the last thing printed would be several iterations stale. The
     * crash handler prints ge_last_mark instead, so suppression costs nothing
     * diagnostically. */
    ge_last_mark = what;
    ge_mark_seq++;

    if (ge_mark_should_print(what)) {
        printf("[getv] boot-> %s\n", what);
    }

    /* Same idea as the stub canary, for the memory-pool bank table: report the first
     * boot step after which any pool has end < start. */
    {
        extern int gePortMempSane(void);
        static int memp_reported = 0;
        if (!memp_reported && !gePortMempSane()) {
            memp_reported = 1;
            printf("[getv] *** POOL TABLE CORRUPTED during '%s' (mark #%lu)\n",
                   what, ge_mark_seq);
        }
    }

    if (!reported && (bad = gePortStubCheck()) != NULL) {
        reported = 1;   /* once: after the first overrun every later check also trips */
        printf("[getv] *** STUB OVERFLOW: %s overran its %d-byte storage, detected at "
               "'%s'. Its real size is larger -- give it a real definition.\n",
               bad, 256 * 1024, what);
    }
    fflush(stdout);
}

/* ---- osSyncPrintf ------------------------------------------------------- */

/* libultra's debug print. On the N64 it went to the host over the debug port; here it
 * goes to the device console like everything else. Implemented for real rather than
 * stubbed because it is the decomp's OWN diagnostic channel -- a lot of the game's
 * error paths report through it, and a stub silently discards all of them. */
/* Non-static so `nm` shows the buffering decision actually took, the same reasoning as
 * ge_eeprom_flushes: a static would inline away and leave no evidence which mode a run used. */
int ge_log_flush_each = 0;
void ge_log_flush_now(void);

/* the fflush here was the frame rate, and gating the callers would have been the wrong fix.
 *
 * 516 decomp call sites funnel through this function and every one of them forced a flush.
 * Measured on this box a flushed stdout line costs about 24 ms when stdout is redirected to a
 * file, so the ~1,660 lines a Train run emits cost roughly 40 seconds of a 50-second run. With
 * stdout discarded entirely the same run does 124 fps against 18.
 *
 * The obvious response -- put each chatty diagnostic behind its own env gate -- throws away
 * information to buy speed, and the comment above says why that is a bad trade here: this is the
 * decomp's OWN error channel and a lot of the game's failure paths report through it. The cost
 * was never the MESSAGES, it was the flush. So: keep every line, stop syncing after each one.
 *
 * stdout is given a real buffer and flushed at exit instead. Output still arrives, still in
 * order, and a normal run pays for a handful of writes rather than sixteen hundred.
 *
 * The flush existed for A reason, so it is still available. A hard crash can lose whatever sits
 * in the buffer, which is exactly when a debug channel matters most -- GETV_LOGFLUSH=1 restores
 * per-line flushing for chasing a hang or a fault. Defaulting it OFF is the right way round
 * because an unreproducible crash is rare and a 7x slowdown is every single run, but the choice
 * has to stay available or this becomes a fix that costs someone a day later.
 */
static void ge_log_setup(void)
{
    static int done = 0;
    if (done) { return; }
    done = 1;
    {
        const char *e = getenv("GETV_LOGFLUSH");
        ge_log_flush_each = (e != NULL && *e == '1');
    }
    if (!ge_log_flush_each) {
        /* 64 KB: about forty of this project's longer diagnostic lines per write. Allocated by
         * the CRT rather than a static of ours, so nothing here has to outlive exit(). */
        setvbuf(stdout, NULL, _IOFBF, 64 * 1024);
        /* Buffered output that is never flushed is output that was thrown away, and a run that
         * ends by exit() rather than by returning from main is the normal case here
         * (GETV_EXIT_FRAME). Registering the flush is what makes "keep every line" true. */
        atexit(ge_log_flush_now);
    }
}

void ge_log_flush_now(void)
{
    fflush(stdout);
}

/* GETV_LOADTRACE -- the per-model asset chatter, off by default.
 *
 * Distinct from the buffering above, and worth keeping distinct. Buffering made the log CHEAP;
 * this makes it SHORT. They solve different problems and neither replaces the other: a cheap log
 * still buries the one line you care about under sixteen hundred you do not, and a short log that
 * synced after every line would still cost the frame rate.
 *
 * What it covers is one category -- diagnostics emitted once per model or per asset as it loads:
 * modelconv, vtxswap, texrow, modeltex, initrw, gdltex/gdltexscale/gdlops/gdlnoop, MEMP big,
 * bgLoad and bggdl. Sixteen call sites.
 *
 * MEASURED, not estimated: a 900-frame Train run goes from 1,662 lines to 1,016 -- 646 lines,
 * about 39%. An earlier draft of this comment guessed "about 1,500 of ~1,660" before anyone
 * counted, which would have been a threefold overstatement sitting in the tree as documentation.
 * The rest of the log is genuinely varied: portals, doors, boot steps, the intro records and the
 * periodic runtime censuses, each a handful of lines from a different place, with no single
 * category left worth gating.
 *
 * The frame-rate effect is NOT measurable on this box and no figure is claimed for it. With
 * stdout already buffered these lines cost almost nothing, and the run-to-run spread here is
 * larger than any gain -- two identical configurations measured 63 and 95 fps. This gate makes
 * the log SHORT, which is a readability win; the SPEED came from the buffering above. Presenting
 * a noisy delta as a speedup is how a placebo gets committed.
 *
 * IT deliberately does not cover error paths. osSyncPrintf has 516 call sites and most of them
 * are the decomp reporting that something went wrong; gating those wholesale is how a failure
 * becomes invisible. Only the sites that report SUCCESSFUL, ROUTINE work are wrapped, and each
 * one was picked by reading it rather than by matching a prefix.
 */
int gePortLoadTrace(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("GETV_LOADTRACE"); on = (e != NULL && *e == '1'); }
    return on;
}

void osSyncPrintf(const char *fmt, ...)
{
    va_list ap;
    ge_log_setup();
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (ge_log_flush_each) { fflush(stdout); }
}

/* --- c_item_entries corruption canary ---------------------------------------
 * The cast intro faults reading c_item_entries[head].header with an address whose
 * LOW word is correct and whose HIGH word holds a small integer. That is the
 * signature of a 32-bit, N64-offset write landing on the upper half of what is now
 * a 64-bit pointer -- bug family #3 (cast-based layout contract). Scanning the whole
 * table at a few checkpoints turns "something corrupts it eventually" into a named
 * call site, the same way the g_ModelHitEntries stride bug was pinned. */
void gePortCheckItemEntries(const char *where)
{
    /* Local mirror of ChrModelFileRecord -- port_support.c deliberately does not pull
     * in the game headers. Only the two leading pointers matter here; the trailing
     * floats/flags pad the record to its natural 32-byte 64-bit size. */
    struct ge_item_rec { void *header; char *filename; float scale, pov;
                         unsigned char isMale, hasHead, pad1, pad2; };
    extern struct ge_item_rec c_item_entries[];
    int i;
    for (i = 0; i < 80; i++) {
        uintptr_t h = (uintptr_t) c_item_entries[i].header;
        uintptr_t f = (uintptr_t) c_item_entries[i].filename;
        if (h != 0 && (h >> 32) != 0x1) {
            osSyncPrintf("[getv] ITEM ENTRY CORRUPT @%s: [%d].header=%p (high=0x%lx)\n",
                         where, i, (void *) h, (unsigned long) (h >> 32));
            return;
        }
        if (f != 0 && (f >> 32) != 0x1) {
            osSyncPrintf("[getv] ITEM ENTRY CORRUPT @%s: [%d].filename=%p (high=0x%lx)\n",
                         where, i, (void *) f, (unsigned long) (f >> 32));
            return;
        }
    }
}

/* Milliseconds of real time since the first call. Used by the clock diagnostic in
 * frametiming.c, which needs a timebase that is definitely not the game's own. */
unsigned int gePortHostMillis(void)
{
    static Uint64 origin = 0;
    static double freq = 0.0;

    if (freq == 0.0) {
        freq = (double) SDL_GetPerformanceFrequency();
        origin = SDL_GetPerformanceCounter();
        if (freq <= 0.0) { freq = 1.0; }
    }
    return (unsigned int) (((double) (SDL_GetPerformanceCounter() - origin) / freq) * 1000.0);
}
