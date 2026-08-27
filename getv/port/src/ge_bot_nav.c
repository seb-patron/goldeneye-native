/* One navigation decision per tick, ported from Rare's own chrNavTickMain (Perfect Dark's
 * chraction.c:12262, and GoldenEye's own guards run the equivalent -- chraction.c:9219 is the
 * same door-opening call this file makes). Guards do not get stuck the way this bot's earlier
 * approach did, and the reason is architectural, not a smarter obstacle test: a guard has ONE
 * mode variable and every tick runs exactly one branch of it. This project's bot had accreted
 * five -- a sensor-triggered avoid hold, a stuck/detour timer, a hard-coded no-go zone, an
 * objective standoff, and the plain route pursuit -- each added to fix one measured symptom,
 * each able to override the others' steering in the same frame. Two of them were proven, live,
 * to pull in opposite directions in the same spot and neither won. This function is the fix for
 * that shape of problem, not for any one symptom: there is exactly one state, and exactly one
 * thing decides the aim point each tick.
 *
 * THE STATES, faithful to the source (WAYMODE_* in PD's constants.h):
 *
 *   INIT / RETRY   -- can we see the real target in a straight line? Yes: aim there, go to
 *                     HAVEAIMPOS. No: go to LOST1 (from INIT) or LOST2 (from RETRY).
 *   LOST1 / LOST2  -- an obstacle broke line of sight to the target itself (not just to an
 *                     avoidance point). Ask gePortSkirt for a corner to route around; LOST2 is
 *                     the more desperate retry after LOST1 gives up. Five ticks of failure and
 *                     LOST1 falls back to RETRY; five more and LOST2 gives up and resets to INIT
 *                     with the target restored as the aim.
 *   HAVEAIMPOS     -- happily walking at the current aim point (which might be the target, or
 *                     might be a corner chosen while routing around something). Re-checked EVERY
 *                     tick against THIS aim, not the original target -- if something new is now
 *                     in the way, go to NEWOBSTACLE. If the aim was the target itself and it is
 *                     still clear, drop back to INIT, which immediately re-confirms against the
 *                     target again -- the natural way a body drifts back onto the direct line the
 *                     moment nothing is blocking it.
 *   NEWOBSTACLE    -- something appeared between here and the current aim. Route around it with
 *                     gePortSkirt and go back to INIT. Five ticks of failure and give up, reset.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO: decide whether to fight, whether to fire at an objective
 * target, or whether a target is close enough to stop pursuing and stand off from. Those are
 * separate questions with separate answers -- PD keeps door-opening as its own always-on check
 * alongside this state machine rather than a state of it, and this keeps the same shape:
 * ge_bot_route.c calls gePortOpenDoorAhead against whatever this returns, on its own ten-tick
 * timer, same as before. Combat and objective engagement are likewise the caller's problem.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_bot_nav.h"

extern int   gePortPathClearParts(float x0, float z0, float x1, float z1);
extern int   gePortSkirt(float x0, float z0, float tx, float tz, float *out_x, float *out_z);
extern int   gePortSkirtWide(float x0, float z0, float tx, float tz, float clearance_mult,
                             float *out_x, float *out_z);
extern int   gePortLocalPath(float px, float pz, float tx, float tz, float cell,
                             float *out_x, float *out_z, int max);

enum {
    GE_NAV_INIT = 0,
    GE_NAV_LOST1,
    GE_NAV_RETRY,
    GE_NAV_LOST2,
    GE_NAV_HAVEAIMPOS,
    GE_NAV_NEWOBSTACLE
};

static int ge_nav_trace(void)
{
    static int t = -1;
    if (t < 0) {
        const char *e = getenv("GETV_BOT_NAV_TRACE");
        t = (e != NULL && *e != '\0' && *e != '0');
    }
    return t;
}

/* Line-of-sight only -- the LINE half of gePortPathClearParts. Not the volume half: that half was
 * measured this session not to depend on the body at all (a fivefold sweep of the width and
 * height moved its answer by zero cells), so trusting it here would be trusting the same broken
 * instrument the rest of tonight's work spent hours getting out from under. Corner-and-clearance
 * arrival, which IS body-aware, is gePortSkirt's job below, not this one's. */
static int ge_nav_can_see(float x0, float z0, float x1, float z1)
{
    int parts = gePortPathClearParts(x0, z0, x1, z1);
    return parts >= 0 && (parts & 1) != 0;
}

/* The floor-search fallback for LOST2's last resort. Several corners, not one -- a single grid
 * cell is small enough that the nearest one is often almost where the body already stands, which
 * reads as "arrived" the instant it is adopted and produces no real progress. Skips ahead to the
 * first corner far enough away to be worth walking to. */
static int ge_nav_local_path(float px, float pz, float tx, float tz, float *out_x, float *out_z)
{
    float cx[6], cz[6];
    int got = gePortLocalPath(px, pz, tx, tz, 60.0f, cx, cz, 6);
    int i;

    for (i = 0; i < got; i++) {
        float dx = cx[i] - px, dz = cz[i] - pz;
        if (dx * dx + dz * dz > 100.0f * 100.0f) {
            *out_x = cx[i];
            *out_z = cz[i];
            return 1;
        }
    }
    return 0;
}

/* Call once per tick, per navigating body. Resets its own state when the target moves further
 * than a body-width from where it last was, which is what "a new instance chasing a new target"
 * means in practice -- callers do not need to manage a reset themselves. */
void gePortNavTick(GeNavState *nav, float px, float pz, float target_x, float target_z,
                   float *out_aim_x, float *out_aim_z)
{
    if (nav == NULL || out_aim_x == NULL || out_aim_z == NULL) { return; }

    if (!nav->have_target
        || fabsf(target_x - nav->target_x) > 40.0f
        || fabsf(target_z - nav->target_z) > 40.0f) {
        nav->have_target = 1;
        nav->target_x = target_x;
        nav->target_z = target_z;
        nav->aim_x = target_x;
        nav->aim_z = target_z;
        nav->mode = GE_NAV_INIT;
        nav->iter = 0;
    }

    switch (nav->mode) {
    case GE_NAV_INIT:
    case GE_NAV_RETRY: {
        int was_init = (nav->mode == GE_NAV_INIT);
        if (ge_nav_can_see(px, pz, nav->target_x, nav->target_z)) {
            nav->aim_x = nav->target_x;
            nav->aim_z = nav->target_z;
            nav->mode = GE_NAV_HAVEAIMPOS;
        } else {
            nav->mode = was_init ? GE_NAV_LOST1 : GE_NAV_LOST2;
            nav->iter = 0;
        }
        break;
    }

    case GE_NAV_LOST1:
    case GE_NAV_LOST2: {
        /* LOST2 tries a wider clearance than LOST1, matching PD's own escalation from a single
         * character's width to something that can clear an obstacle several times that size --
         * Train's crates (radius ~89) need about 4x a body's own clearance (25 units) before a
         * tangent point actually lands clear of the row rather than inside the next one along. */
        float sx, sz;
        int side = (nav->mode == GE_NAV_LOST1)
                 ? gePortSkirt(px, pz, nav->target_x, nav->target_z, &sx, &sz)
                 : gePortSkirtWide(px, pz, nav->target_x, nav->target_z, 4.0f, &sx, &sz);
        if (side > 0) {
            nav->aim_x = sx;
            nav->aim_z = sz;
            nav->mode = GE_NAV_HAVEAIMPOS;
        } else {
            nav->iter++;
            if (nav->iter > 5) {
                if (nav->mode == GE_NAV_LOST1) {
                    nav->mode = GE_NAV_RETRY;
                } else {
                    /* LOST2 giving up is where PD's own design runs out of road: chrNavTryObstacle
                     * is built to step around ONE obstacle, at up to a few times a body's own
                     * width. A barricade -- several large props in a contiguous row, which is what
                     * stopped this project for days on Train -- is not that shape of problem, and
                     * escalating the SAME tool's clearance further does not change what it is.
                     * gePortLocalPath is the other tool this project already has for exactly this:
                     * a short breadth-first search over the floor with prop footprints closed,
                     * proven to route around this exact row. Reached for here, once, as the last
                     * thing tried before actually giving up -- not a parallel system running
                     * alongside the state machine, its own place within it. */
                    float lx, lz;
                    if (ge_nav_local_path(px, pz, nav->target_x, nav->target_z, &lx, &lz)) {
                        nav->aim_x = lx;
                        nav->aim_z = lz;
                        nav->mode = GE_NAV_HAVEAIMPOS;
                        nav->iter = 0;
                        break;
                    }
                    nav->aim_x = nav->target_x;
                    nav->aim_z = nav->target_z;
                    nav->mode = GE_NAV_INIT;
                }
                nav->iter = 0;
            }
        }
        break;
    }

    case GE_NAV_HAVEAIMPOS: {
        /* Against the CURRENT aim, which is not always the target -- this is what lets the body
         * notice something new blocking the corner it already committed to. */
        if (ge_nav_can_see(px, pz, nav->aim_x, nav->aim_z)) {
            nav->mode = GE_NAV_INIT;   /* re-confirm the direct line to the real target next tick */
        } else {
            nav->mode = GE_NAV_NEWOBSTACLE;
            nav->iter = 0;
        }
        break;
    }

    case GE_NAV_NEWOBSTACLE: {
        /* Same escalation as LOST2, against the current aim rather than the original target: a
         * new obstacle discovered mid-manoeuvre gets one attempt at normal clearance before
         * trying wider. */
        float sx, sz;
        int side = (nav->iter == 0)
                 ? gePortSkirt(px, pz, nav->aim_x, nav->aim_z, &sx, &sz)
                 : gePortSkirtWide(px, pz, nav->aim_x, nav->aim_z, 4.0f, &sx, &sz);
        if (side > 0) {
            nav->aim_x = sx;
            nav->aim_z = sz;
            nav->mode = GE_NAV_INIT;
        } else {
            nav->iter++;
            if (nav->iter > 5) {
                nav->aim_x = nav->target_x;
                nav->aim_z = nav->target_z;
                nav->mode = GE_NAV_INIT;
                nav->iter = 0;
            }
        }
        break;
    }

    default:
        nav->mode = GE_NAV_INIT;
        break;
    }

    if (ge_nav_trace()) {
        static const char *names[] = { "INIT", "LOST1", "RETRY", "LOST2", "HAVEAIMPOS", "NEWOBSTACLE" };
        printf("[getv][nav] pos=(%.0f %.0f) mode=%-11s aim=(%.0f %.0f) target=(%.0f %.0f) iter=%d\n",
               (double) px, (double) pz, names[nav->mode], (double) nav->aim_x, (double) nav->aim_z,
               (double) nav->target_x, (double) nav->target_z, nav->iter);
        fflush(stdout);
    }

    *out_aim_x = nav->aim_x;
    *out_aim_z = nav->aim_z;
}
