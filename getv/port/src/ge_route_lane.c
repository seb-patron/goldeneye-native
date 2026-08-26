/* Nudge a route target sideways onto ground a body can actually walk.
 *
 * The route's waypoints are the game's own nav pads, and on Train they run down the middle of a
 * line that is not the middle of the walkable floor: the pads sit at z=-133 while the player
 * walks at z=-60. Asking the engine whether each route step is walkable says no for 18 of
 * Train's 46 steps -- yet the bot demonstrably walks several of them, because it is a body with
 * width and it drifts to whichever side has room.
 *
 * Asking the same question again a little to each side answers it properly. 17 of those 18 steps
 * are clear within 200 units of the pad line, so the route is not blocked; it is drawn through
 * the furniture. This finds the smallest sideways offset that opens the step and aims there
 * instead, which is what a person does without thinking about it.
 *
 * The offset is perpendicular to the direction of travel, so it works on a corridor running any
 * way, not just Train's east-west one.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern int gePortPathClear(float x0, float z0, float x1, float z1);
extern int gePortCanStandAt(float x, float z);

#define GE_LANE_STEP    40.0f   /* a fifth of a body width; finer than this just costs queries */
#define GE_LANE_MAX    240.0f   /* past this it is a different route, not the same one offset */

static int ge_lane_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GETV_ROUTE_LANE");
        on = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    return on;
}

static int ge_lane_trace(void)
{
    static int t = -1;
    if (t < 0) {
        const char *e = getenv("GETV_ROUTE_LANE_TRACE");
        t = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    return t;
}

/* Rewrites (*tx, *tz) to a reachable aim point and returns the offset applied, or 0 when the
 * target needed no help -- which is the common case and costs exactly one query. */
float gePortLaneOffset(float px, float pz, float *tx, float *tz)
{
    float dx, dz, len, nx, nz, mag;

    if (tx == NULL || tz == NULL) { return 0.0f; }

    if (gePortPathClear(px, pz, *tx, *tz) == 1) { return 0.0f; }

    dx = *tx - px;
    dz = *tz - pz;
    len = (float) sqrt((double) (dx * dx + dz * dz));
    if (len < 1.0f) { return 0.0f; }
    nx = -dz / len;                       /* left-hand normal to the direction of travel */
    nz =  dx / len;

    /* Outward from the pad, alternating sides, so the smallest usable correction wins and the
     * bot keeps hugging the route rather than swinging wide on the first thing that works. */
    for (mag = GE_LANE_STEP; mag <= GE_LANE_MAX; mag += GE_LANE_STEP) {
        int side;
        for (side = 0; side < 2; side++) {
            float m  = side ? -mag : mag;
            float ax = *tx + nx * m;
            float az = *tz + nz * m;

            if (gePortPathClear(px, pz, ax, az) != 1) { continue; }
            if (!gePortCanStandAt(ax, az))            { continue; }

            if (ge_lane_trace()) {
                printf("[getv][lane] target (%.0f %.0f) unreachable, aiming (%.0f %.0f) instead"
                       " -- %.0f units to the %s\n",
                       (double) *tx, (double) *tz, (double) ax, (double) az,
                       (double) mag, side ? "right" : "left");
                fflush(stdout);
            }
            *tx = ax;
            *tz = az;
            return m;
        }
    }

    if (ge_lane_trace()) {
        printf("[getv][lane] target (%.0f %.0f) unreachable and no lane within %.0f units\n",
               (double) *tx, (double) *tz, (double) GE_LANE_MAX);
        fflush(stdout);
    }
    return 0.0f;
}


/* The same answer, but only when the route bot has been told to use it.
 *
 * The report always wants to know the way past -- that is what a report is for -- while changing
 * how the bot steers is a behaviour change that has to be switchable to be measurable. Same
 * computation, two callers, one of them gated.
 */
float gePortRouteLane(float px, float pz, float *tx, float *tz)
{
    if (!ge_lane_on()) { return 0.0f; }
    return gePortLaneOffset(px, pz, tx, tz);
}
