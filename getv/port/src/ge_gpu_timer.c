/* GPU-side frame timing. See ge_gpu_timer.h for what question this answers and why. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GL headers follow the same rule as gfx_opengl.c: GLEW where it is used to load entry points,
 * the platform's own headers otherwise. Including <GL/glew.h> unconditionally builds on Windows
 * and fails everywhere else, which is how this file arrived. */
#if defined(_WIN32) || defined(WIN32) || defined(OSX_BUILD)
# define GLEW_STATIC
# include <GL/glew.h>
#else
# define GL_GLEXT_PROTOTYPES 1
# if defined(__APPLE__)
#  include <OpenGL/gl3.h>
#  include <OpenGL/gl3ext.h>
# else
#  include <GL/gl.h>
#  include <GL/glext.h>
# endif
#endif

/* GLEW exposes extension availability as a flag; without GLEW there is no equivalent to test at
 * compile time. The Mac and Linux builds ask SDL for a GL 2.0/3.0 context, which does not
 * guarantee ARB_timer_query, so the honest answer off Windows is that the timer is unavailable
 * and the existing disable path handles it. Reporting it as present would compile and then fail
 * at the first glQueryCounter. */
#ifndef GLEW_ARB_timer_query
# define GLEW_ARB_timer_query 0
#endif
#include <SDL.h>

#include "ge_gpu_timer.h"

/* Ring depth. Three is the smallest that reliably has a finished result to collect while two are
 * still in flight; four gives a frame of slack on a driver that runs further ahead. It is NOT a
 * count of simultaneously active queries -- GL permits exactly one GL_TIME_ELAPSED query at a
 * time, so the ring exists purely so a FINISHED result can be read without waiting on a live one. */
#define GE_GT_RING 4

/* How often to print. 120 frames is roughly one to two seconds here, which is long enough that
 * per-frame jitter averages out and short enough to watch a level change move the numbers. */
#define GE_GT_REPORT 120

static int    ge_gt_on = -1;        /* -1 = not yet resolved */
static GLuint ge_gt_q[GE_GT_RING];
static int    ge_gt_issued[GE_GT_RING];   /* has slot i ever been given a query to carry? */
static int    ge_gt_frame;
static int    ge_gt_active;         /* a query is currently open; guards an unbalanced end */

/* Accumulators, reset every report. Sums rather than running averages: a running average hides
 * the one 40 ms frame that is the whole problem, and the max is kept for that reason. */
static double ge_gt_gpu_ms, ge_gt_gpu_max;
static double ge_gt_swap_ms, ge_gt_swap_max;
static int    ge_gt_gpu_n, ge_gt_swap_n;

int geGpuTimerEnabled(void)
{
    if (ge_gt_on < 0) {
        const char *e = getenv("GETV_GPUTIME");
        ge_gt_on = (e != NULL && *e == '1');
        if (ge_gt_on) {
            /* Check the extension rather than the GL version. Timer queries are core from GL
             * 3.3, but a driver may still expose a context that reports 4.3 and refuse the entry
             * points -- and on Windows those resolve through GLEW, where a missing one is a NULL
             * pointer and calling it is a crash, not an error. Intel's 2015-era HD 4400 driver is
             * exactly the sort of thing this guard is for. */
            if (!GLEW_ARB_timer_query) {
                printf("[getv][gputime] driver has no ARB_timer_query -- GPU timing unavailable\n");
                fflush(stdout);
                ge_gt_on = 0;
                return ge_gt_on;
            }
            /* Drain the error queue first. glGetError reports and clears one error per call
             * from a queue that persists until read, so anything earlier in the frame that left
             * an error pending gets attributed to the next call that checks -- which is this one.
             *
             * That is not hypothetical: the first version of this code reported "could not
             * allocate timer queries" on a driver where the allocation had in fact succeeded. It
             * was reading somebody else's error. A check that inherits state is not a check.
             *
             * Anything drained here belongs to code that ran before us and is reported rather
             * than swallowed, because a GL error nobody is looking at is worth knowing about. */
            {
                GLenum stale;
                int n = 0;
                while ((stale = glGetError()) != GL_NO_ERROR && n < 16) {
                    printf("[getv][gputime] draining pre-existing GL error 0x%04x "
                           "(not ours -- raised before timer init)\n", (unsigned) stale);
                    n++;
                }
                if (n) { fflush(stdout); }
            }

            memset(ge_gt_q, 0, sizeof ge_gt_q);
            glGenQueries(GE_GT_RING, ge_gt_q);
            {
                GLenum err = glGetError();
                if (err != GL_NO_ERROR || ge_gt_q[0] == 0) {
                    /* Report the CODE and the name. "could not allocate" sent me looking at the
                     * allocation when the real answer was a stale error from elsewhere, and a
                     * diagnostic that does not name what went wrong costs more than it saves. */
                    printf("[getv][gputime] glGenQueries failed: err=0x%04x q[0]=%u -- disabled\n",
                           (unsigned) err, (unsigned) ge_gt_q[0]);
                    fflush(stdout);
                    ge_gt_on = 0;
                    return ge_gt_on;
                }
            }
            printf("[getv][gputime] on: GPU busy vs CPU-in-swap, reported every %d frames\n",
                   GE_GT_REPORT);
            fflush(stdout);
        }
    }
    return ge_gt_on;
}

void geGpuTimerFrameBegin(void)
{
    int slot;
    if (!geGpuTimerEnabled() || ge_gt_active) { return; }
    slot = ge_gt_frame % GE_GT_RING;

    /* Collect the OLDEST slot before reusing it. It is GE_GT_RING-1 frames old, so it has had
     * that many frames to finish -- but it is still checked rather than assumed, because a driver
     * that is running far ahead can leave it pending and GL_QUERY_RESULT would then BLOCK. A
     * skipped sample is a gap in a statistic; a blocking read is a lie about the thing being
     * measured. */
    if (ge_gt_issued[slot]) {
        GLint ready = 0;
        glGetQueryObjectiv(ge_gt_q[slot], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(ge_gt_q[slot], GL_QUERY_RESULT, &ns);
            {
                double ms = (double) ns / 1.0e6;
                ge_gt_gpu_ms += ms;
                ge_gt_gpu_n++;
                if (ms > ge_gt_gpu_max) { ge_gt_gpu_max = ms; }
            }
        }
        ge_gt_issued[slot] = 0;
    }

    glBeginQuery(GL_TIME_ELAPSED, ge_gt_q[slot]);
    ge_gt_active = 1;
}

void geGpuTimerFrameEnd(void)
{
    if (!geGpuTimerEnabled() || !ge_gt_active) { return; }
    glEndQuery(GL_TIME_ELAPSED);
    ge_gt_issued[ge_gt_frame % GE_GT_RING] = 1;
    ge_gt_active = 0;
    ge_gt_frame++;

    if ((ge_gt_frame % GE_GT_REPORT) == 0) {
        double gpu  = ge_gt_gpu_n  ? ge_gt_gpu_ms  / ge_gt_gpu_n  : 0.0;
        double swap = ge_gt_swap_n ? ge_gt_swap_ms / ge_gt_swap_n : 0.0;

        /* Both numbers on one line, with the interpretation spelled out, because the whole point
         * is the RELATIONSHIP between them and a reader should not have to remember which way
         * round it goes at 2am:
         *
         *   gpu high, swap low   -> the GPU is genuinely busy; the work is real
         *   gpu low,  swap high  -> the GPU is idle and we are being stalled by driver or
         *                           compositor; look at present path, not at draw calls
         *   both low             -> the time is somewhere else entirely and neither is the cause
         */
        /* "GPU timeline", not "GPU busy", and the distinction IS not pedantry.
         * GL_TIME_ELAPSED measures wall time between two markers ON THE GPU'S TIMELINE. If the
         * GPU spends part of that window waiting -- for a buffer to free, for the compositor, for
         * work to arrive -- that waiting is inside the number. So a large value proves the time is
         * spent GPU-SIDE rather than in our thread; it does NOT prove the hardware is saturated.
         *
         * Measured here: 6 ms at 1280x960 and 6 ms at 320x240, twelve times fewer pixels. A
         * genuinely fill-rate-saturated GPU cannot be indifferent to that, so on this machine the
         * figure is dominated by something fixed per frame rather than by shading work. Calling
         * that "GPU-BOUND" would send the next reader to optimise draw calls and shaders, which
         * this measurement gives no reason to touch. */
        printf("[getv][gputime] f%d | GPU timeline %.2f ms (max %.2f, n=%d) | CPU in swap %.2f ms "
               "(max %.2f, n=%d) | %s\n",
               ge_gt_frame, gpu, ge_gt_gpu_max, ge_gt_gpu_n, swap, ge_gt_swap_max, ge_gt_swap_n,
               (gpu > swap * 2.0)  ? "time is GPU-SIDE (not blocked in swap)" :
               (swap > gpu * 2.0)  ? "STALLED IN SWAP (GPU idle)" :
                                     "neither dominates");
        fflush(stdout);

        ge_gt_gpu_ms = ge_gt_gpu_max = 0.0; ge_gt_gpu_n = 0;
        ge_gt_swap_ms = ge_gt_swap_max = 0.0; ge_gt_swap_n = 0;
    }
}

void geGpuTimerRecordSwap(double ms)
{
    if (!geGpuTimerEnabled()) { return; }
    ge_gt_swap_ms += ms;
    ge_gt_swap_n++;
    if (ms > ge_gt_swap_max) { ge_gt_swap_max = ms; }
}
