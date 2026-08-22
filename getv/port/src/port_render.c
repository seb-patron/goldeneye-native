/* GoldenEye tvOS port — the bridge from the game's display list to Fast3D.
 *
 * Kept in its own translation unit deliberately. port_os.c is compiled against the N64
 * SDK headers (PR/*.h) and must not pull in Fast3D's; this file is the one place the
 * two meet.
 *
 * The game builds a display list every frame in lvlRender(), terminates it, and hands
 * it to rspGfxTaskStart(), which on the N64 gives it to the video scheduler for the
 * RSP/RDP to execute. Here it goes to Fast3D instead.
 *
 * GoldenEye runs a custom microcode (gsp3D), not stock F3D. Fast3D's parser handles the
 * standard F3D command set, which covers the geometry/texture/combiner commands the
 * game shares with SM64, but GoldenEye has commands of its own. Unknown opcodes are the
 * expected failure mode here, not a port bug -- see the ROADMAP.
 */
#include <stdint.h>   /* gfx_pc.h uses uint32_t but does not include it */
#include <stdio.h>

#include <PR/gbi.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL.h>

#include "gfx_pc.h"
#include "ge_sky_rdp.h"

/* ---- GETV_SKYDUMP: prove the sky RDP triangles are recoverable -----------------
 *
 * This lives here rather than in gfx_pc.c because it is a diagnostic, not a renderer
 * change. Walking the display list a second time costs one pass over a few thousand
 * commands on the frames we ask for, and it lets the decode in ge_sky_rdp.c be
 * validated against the real stream without touching the renderer.
 *
 * Bounded on purpose, in both command count and recursion depth. This walks raw game
 * memory; an unrecognised branch target would otherwise run off into the arena and the
 * crash would look like a renderer bug. Off unless GETV_SKYDUMP is set.
 *
 * F3D opcodes, u8-truncated: G_DL = 0x06, G_ENDDL = G_IMMFIRST-7 = 0xB8. */
#define GE_SKYDUMP_MAX_CMDS  200000
#define GE_SKYDUMP_MAX_DEPTH 12

static int geSkyDumpLevel(void)
{
    static int lv = -1;
    if (lv < 0) {
        const char *e = getenv("GETV_SKYDUMP");
        lv = (e != NULL && *e != '\0') ? atoi(e) : 0;
    }
    return lv;
}

static void geSkyDumpWalk(const Gfx *gdl, int depth, unsigned long *budget, int *shown)
{
    if (gdl == NULL || depth > GE_SKYDUMP_MAX_DEPTH) {
        return;
    }
    for (; *budget > 0; gdl++) {
        uintptr_t w0 = (uintptr_t)gdl->words.w0;
        uintptr_t w1 = (uintptr_t)gdl->words.w1;
        unsigned op = (unsigned)((w0 >> 24) & 0xFFu);
        struct GeSkyTri tri;

        (*budget)--;

        if (op == 0xB8u) {                 /* G_ENDDL */
            return;
        }
        if (op == 0x06u) {                 /* G_DL: nested (push) or branch (nopush) */
            geSkyDumpWalk((const Gfx *)w1, depth + 1, budget, shown);
            if (((w0 >> 16) & 0xFFu) != 0) {   /* G_DL_NOPUSH -> branch, do not return */
                return;
            }
            continue;
        }
        if (geSkyRdpFeed(op, (uint32_t)w1, &tri)) {
            if (*shown < 12) {
                (*shown)++;
                printf("[getv][skytri] cmd=0x%02X %s tile=%d  "
                       "v0=(%.2f,%.2f) v1=(%.2f,%.2f) v2=(%.2f,%.2f)\n",
                       tri.cmd, tri.textured ? "SHADE_TXTR" : "FILL", tri.tile,
                       tri.x[0], tri.y[0], tri.x[1], tri.y[1], tri.x[2], tri.y[2]);
            }
        }
    }
}

static void geSkyDump(void *firstGdl, int frame)
{
    unsigned long budget = GE_SKYDUMP_MAX_CMDS;
    int shown = 0;

    if (geSkyDumpLevel() <= 0 || firstGdl == NULL) {
        return;
    }
    /* One frame only by default -- the stream is identical every frame and 61 copies of
     * it buries everything else in the log. GETV_SKYDUMP=2 dumps every frame. */
    if (geSkyDumpLevel() < 2 && frame != 1) {
        return;
    }
    geSkyRdpReset();
    geSkyRdpTris = geSkyRdpHalves = geSkyRdpAborted = 0;
    geSkyDumpWalk((const Gfx *)firstGdl, 0, &budget, &shown);
    printf("[getv][skydump] frame %d: decoded %lu triangles from %lu halves, "
           "%lu runs abandoned (walked %lu commands)\n",
           frame, geSkyRdpTris, geSkyRdpHalves, geSkyRdpAborted,
           GE_SKYDUMP_MAX_CMDS - budget);
    fflush(stdout);
}

/* Set once the GL context and Fast3D are up; the harness calls gfx_init() before it
 * enters the game's boot path, but a display list arriving before that would be fatal. */
static int ge_render_ready = 0;

void gePortRenderReady(void)
{
    ge_render_ready = 1;
}

void gePortRenderDisplayList(void *firstGdl)
{
    static int rendered = 0;

    if (!ge_render_ready || firstGdl == NULL) {
        return;
    }

    /* Time each stage separately for the first few frames. "It stalls after frame 1" is
     * not actionable; "gfx_end_frame took 20 seconds" is. */
    if (rendered < 5) {
        /* Announce before each stage, not after. Printing a summary afterwards tells you
         * nothing when a stage never returns -- which is exactly what frame 1 does. */
        Uint32 t0 = SDL_GetTicks(), t1, t2, t3;
        printf("[getv] frame %d: -> gfx_start_frame\n", rendered); fflush(stdout);
        gfx_start_frame();  t1 = SDL_GetTicks();
        printf("[getv] frame %d: -> gfx_run (%ums)\n", rendered, t1 - t0); fflush(stdout);
        gfx_run((Gfx *)firstGdl); t2 = SDL_GetTicks();
        printf("[getv] frame %d: -> gfx_end_frame (%ums)\n", rendered, t2 - t1); fflush(stdout);
        gfx_end_frame();    t3 = SDL_GetTicks();
        printf("[getv] frame %d: DONE start=%ums run=%ums end=%ums\n",
               rendered, t1 - t0, t2 - t1, t3 - t2);
        fflush(stdout);
    } else {
        gfx_start_frame();
        gfx_run((Gfx *)firstGdl);
        gfx_end_frame();
    }
    rendered++;

    /* ---- GETV_PACETRACE=1: the two frame deltas, side by side, per frame -----------
     *
     * The probe exists to show the two deltas stay separate (GE_RETAIL_BEHAVIOUR §2.6).
     * `speedgraphframes` is the presentation delta and keeps counting while the watch
     * is up; `g_ClockTimer` is the world delta and `lv.c:1040-1047` forces it to 0 on
     * pause. Collapsing them breaks pause in both directions -- either the world keeps
     * moving under the watch, or the mission clock freezes with it -- so verifying a
     * pacing change means watching both, plus one consumer of each, across a pause and
     * an un-pause.
     *
     * Consumers printed:
     *   g_GlobalTimer  += g_ClockTimer        (lv.c)             -> world, must stall when paused
     *   watch_time_0   += speedgraphframes    (bondview2.c:8185) -> presentation, must keep rising
     *
     * These are read as plain externs from the port layer on purpose: no game file is
     * touched to obtain the measurement, so the measurement cannot itself be the change. */
    /* ---- GETV_PAUSETEST=<from>:<to>[:lock|paused] --------------------------------
     *
     * A harness, not a behaviour change. It asserts one of GoldenEye's own two pause
     * causes for a window of frames so the freeze can be observed in both directions
     * even on a build whose watch model faults in modelUpdateMatrices
     * (`bondviewRenderWatch -> process_02_position`, reproducible at 30 fps and 60 fps
     * alike, so not a pacing regression).
     *
     *   lock    g_ControlsLockedFlag -- cutscenes and level transitions (lv.c:1036)
     *   paused  g_pausedFlag         -- the watch / MP menu (lv.c:1040 via checkGamePaused)
     *
     * Both land in the same `g_ClockTimer = 0` in lvlManageMpGame, which is the whole
     * freeze. Nothing else writes either flag after init (`lvlSetControlsLockedFlag`,
     * lv.c:1770, is the only other writer), so the window is clean.
     *
     * Set at render time, so the flag is observed by the next frame's lvlManageMpGame.
     * Expect the clock edge one frame after the window boundary. */
    {
        static int ptFrom = -1, ptTo = -1, ptMode = 0;   /* 0 = lock, 1 = paused */
        if (ptFrom == -1) {
            const char *e = getenv("GETV_PAUSETEST");
            ptFrom = 0; ptTo = 0;
            if (e != NULL && *e != '\0') {
                char buf[64]; int a = 0, b = 0;
                snprintf(buf, sizeof(buf), "%s", e);
                if (sscanf(buf, "%d:%d", &a, &b) == 2 && b > a) {
                    ptFrom = a; ptTo = b;
                    ptMode = (strstr(buf, "paused") != NULL) ? 1 : 0;
                    printf("[getv][pause] TEST: assert %s for frames %d..%d\n",
                           ptMode ? "g_pausedFlag" : "g_ControlsLockedFlag", a, b);
                    fflush(stdout);
                }
            }
        }
        if (ptTo > 0) {
            extern int g_ControlsLockedFlag;
            extern int g_pausedFlag;
            int on = (rendered >= ptFrom && rendered < ptTo);
            if (ptMode) { g_pausedFlag          = on; }
            else        { g_ControlsLockedFlag  = on; }
        }
    }

    {
        static int trace = -1;
        if (trace == -1) {
            const char *e = getenv("GETV_PACETRACE");
            trace = (e != NULL && *e == '1') ? 1 : 0;
        }
        if (trace) {
            extern int g_ClockTimer;          /* s32 */
            extern int g_GlobalTimer;         /* s32 */
            extern int speedgraphframes;      /* s32 */
            extern int watch_time_0;          /* s32 on VERSION_US (bondview.h:2723) */
            extern int g_pausedFlag;          /* s32, mpmenu.c:44 */
            printf("[getv][pace] f=%d sgf=%d clock=%d global=%d watch=%d paused=%d\n",
                   rendered, speedgraphframes, g_ClockTimer, g_GlobalTimer,
                   watch_time_0, g_pausedFlag);
            fflush(stdout);
        }
    }

    geSkyDump(firstGdl, rendered);

    /* ---- GETV_VITRACE: the projection inputs the portal clipper actually reads ------
     * DEFAULT (stage 0) can flip between two modes across launches of one binary. The
     * first divergence between a good and a bad log is bg.c's sub_GAME_7F0B5528
     * returning pointcount=0 (all portal points classified "behind") instead of 4. That
     * test is `-zrange[1] * 0.9f < point->z`, whose inputs are viGetZRange() (i.e.
     * g_ViBackData->znear/zfar), room_data_float2, mCurrentLevelVisibilityScale and
     * camGetWorldToScreenMtxf(). None of those are printed anywhere else, which is why
     * the two logs are byte-identical right up to the consequence.
     *
     * viSetZRange() is called only from front.c (the front-end menus), so on a
     * GETV_STAGE boot znear/zfar are whatever viInitVideoSettings left behind. */
    if (rendered < 4 && getenv("GETV_VITRACE") != NULL) {
        extern void  viGetZRange(float *);
        extern float room_data_float2;
        extern float mCurrentLevelVisibilityScale;
        extern void *camGetWorldToScreenMtxf(void);
        float zr[2] = { 0.0f, 0.0f };
        const float *m;
        int k;
        viGetZRange(zr);
        printf("[getv][vi] f=%d znear=%.6f zfar=%.6f room_scale=%.6f vis_scale=%.6f\n",
               rendered, (double)zr[0], (double)zr[1],
               (double)room_data_float2, (double)mCurrentLevelVisibilityScale);
        m = (const float *)camGetWorldToScreenMtxf();
        printf("[getv][vi] f=%d w2s=%p", rendered, (const void *)m);
        if (m != NULL) { for (k = 0; k < 16; k++) { printf(" %.4f", (double)m[k]); } }
        printf("\n");
        fflush(stdout);
    }

    /* ---- GETV_EXIT_FRAME: fixed-FRAME termination, not fixed-WALL-CLOCK ------------
     *
     * This is what makes triangle counts comparable across launches.
     *
     * gfx_end_frame() prints `tris submitted=/drawn=` every 60 frames, and those
     * counters are reset each frame, so each line is an instantaneous sample rather
     * than a total. Taking the last such line (as a wall-clock-bounded run does)
     * samples whichever 60-frame checkpoint happened to fall before the timeout, and
     * how many frames fit in that wall clock depends entirely on host load -- a frame
     * costs roughly 0.6-4 s on the software GL simulator.
     *
     * Levels also change their triangle count partway through their own scripted
     * opening. In one run of RUNWAY, frames 1..361 report submitted=296..298 drawn=6..8
     * during the intro, and frame 421 reports submitted=1194 drawn=301 once the world
     * comes up: a 4x step in `submitted` and a 37x step in `drawn` inside a single
     * deterministic run. Sampling that under a load-dependent timeout looks like
     * launch-to-launch nondeterminism even though the run itself is deterministic.
     *
     * Setting GETV_EXIT_FRAME=N ends the run after N rendered frames, so the last
     * checkpoint is the same frame on every launch and every host, and comparisons
     * become frame-for-frame instead of second-for-second. Pick N = k*60 + 1 so the run
     * ends just after a checkpoint.
     *
     * Opt-in: with the variable unset, behaviour is unchanged. */
    {
        static int limit = -1;
        if (limit == -1) {
            const char *e = getenv("GETV_EXIT_FRAME");
            limit = (e && *e) ? atoi(e) : 0;
            if (limit > 0) {
                printf("[getv] GETV_EXIT_FRAME=%d -- run ends after %d rendered frames\n",
                       limit, limit);
                fflush(stdout);
            }
        }
        if (limit > 0 && rendered >= limit) {
            { extern unsigned long ge_mipcrop_hits, ge_mipcrop_seen;
              extern unsigned long ge_paloff_applied, ge_paloff_last;
              extern unsigned long ge_tex4b_clamped;
              printf("[getv][fix] mipcrop %lu/%lu uploads trimmed | paloff applied %lu "
                     "(last=%lu bytes) | 4b_src_clamped %lu\n",
                     ge_mipcrop_hits, ge_mipcrop_seen, ge_paloff_applied,
                     ge_paloff_last, ge_tex4b_clamped); }
            { extern unsigned long ge_texfmt_census[8][4];
              static const char *fn[8] = {"RGBA","YUV","CI","IA","I","f5","f6","f7"};
              static const char *sn[4] = {"4b","8b","16b","32b"};
              int f, z;
              printf("[getv][texfmt] live fmt/siz at import_texture:\n");
              for (f = 0; f < 8; f++)
                for (z = 0; z < 4; z++)
                  if (ge_texfmt_census[f][z])
                    printf("[getv][texfmt]   %-4s %-3s : %lu\n", fn[f], sn[z], ge_texfmt_census[f][z]);
            }
            { extern unsigned long ge_tex4b_loads;
              extern unsigned long ge_sky_emitted;
              printf("[getv][fp] 4bit_texture_loads=%lu sky_tris=%lu\n",
                     ge_tex4b_loads, ge_sky_emitted); }
            printf("[getv][fp] exit_frame reached: frames=%d\n", rendered);
            fflush(NULL);
            /* _exit, not exit: the normal shutdown path already hangs (every timed run
             * prints "game_exit requested" and is then killed by the timeout), and a
             * teardown hang would reintroduce exactly the wall-clock dependence this
             * flag exists to remove. Everything we measure is already flushed. */
            _exit(0);
        }
    }

    /* ---- what the N64's retrace handler used to do ------------------------------
     *
     * src/sched.c's __scHandleRetrace() ran once per vertical retrace and drove three
     * subsystems. sched.c does not compile (it is the N64 video/audio scheduler), so
     * nothing was calling them, and each failure is silent:
     *
     *   joyPoll()        the input producer. joy.c's read path is gated on it, and
     *                    with no caller the linker dead-strips the path entirely --
     *                    osContGetReadData appears neither defined nor undefined in
     *                    the binary. Missing input looks exactly like working input.
     *   musicFadeTick()  the music fade envelope. Without it a fade-in never
     *                    progresses past its first step and a fade-out never completes.
     *
     * Not carried over, deliberately: viVsyncRelated() programs VI registers that no
     * longer exist, and speedgraphMarkerUpdate() is the debug profiler.
     *
     * Audio rides the frame the same way audi.c's amMain() thread woke on the retrace
     * message. gePortAudioFrame() belongs after the frame is submitted -- the sequence
     * and sound players run inside alAudioFrame(), so whatever the game's frame did to
     * them must have happened first. joyPoll() is a producer feeding a sample ring the
     * game drains next iteration via joyConsumeSamplesWrapper(), so its position
     * relative to the frame does not matter; it matters only that it runs. */
    { extern void joyPoll(void);        joyPoll(); }
    { extern void musicFadeTick(void);  musicFadeTick(); }
    { extern void gePortAudioFrame(void); gePortAudioFrame(); }
}
