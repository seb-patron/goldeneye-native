/* Ask the engine whether each step of a route is walkable in a straight line.
 *
 * A route is a chain of waypoints the extractor believes are connected. The follower assumes the
 * straight line between consecutive ones is clear, and when it is not the bot presses into
 * geometry with its target dead ahead and its heading correct, which reads as a steering fault
 * and is not one.
 *
 * gePortPathClear is the engine's own test: stanTestLineUnobstructed for the run, then
 * stanTestVolume at the far end for the body's width. Running it over the route says which steps
 * are impossible before any policy is blamed for failing them.
 *
 *   GETV_ROUTE_AUDIT=1   audit the route the bot would walk, then carry on
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "ge_world_api.h"

extern int gePortPathClear(float x0, float z0, float x1, float z1);
extern int gePortPathClearParts(float x0, float z0, float x1, float z1);
extern int gePortCanStandAt(float x, float z);
extern int gePortPlayerPos(int idx, float *out);

static int ge_ra_done;

static int ge_ra_pos(int id, float *x, float *z)
{
    GeWorldWaypoint wp;
    int i;

    for (i = 0; i < geWorldWaypointCount(); i++) {
        if (geWorldWaypoint(i, &wp) && wp.id == id) {
            *x = wp.x;
            *z = wp.z;
            return 1;
        }
    }
    return 0;
}

void gePortRouteAuditFrame(int frame)
{
    const char *on;
    GeWorldStep step;
    int i, blocked = 0, tested = 0, unknown = 0, asym = 0, wall = 0, noroom = 0;
    int obj;

    if (ge_ra_done) { return; }
    on = getenv("GETV_ROUTE_AUDIT");
    if (on == NULL || *on == '\0' || *on == '0') { ge_ra_done = 1; return; }

    /* Wait for the world to be loaded by whoever loads it, rather than loading a second copy. */
    if (!geWorldLoaded()) { return; }

    /* And wait for the LEVEL to be live, which is a different thing entirely. geWorldLoaded()
     * only says the route JSON has been parsed, and that happens while the title screen is still
     * up. Every stan query asked before the player exists is asked of a level that has not been
     * set up, which is how this audit came to report steps blocked that the bot walks daily.
     * The player having a position is the cheapest proof that the world is real. */
    {
        float p[3];
        if (!gePortPlayerPos(0, p)) { return; }
    }
    ge_ra_done = 1;
    (void) frame;

    {
        /* Control question, asked before anything else is believed: can the player stand where
         * the player is standing? The audit reports steps blocked that the bot walks, so either
         * the world disagrees with itself or the test is being asked wrongly, and only one of
         * those two is worth chasing. */
        float p[3];
        if (gePortPlayerPos(0, p)) {
            printf("[getv][route] control: player is at (%.0f %.0f); the engine says a body %s "
                   "stand there\n", (double) p[0], (double) p[2],
                   gePortCanStandAt(p[0], p[2]) ? "CAN" : "CANNOT");
        }
    }

    for (obj = 0; obj < geWorldObjectiveCount(); obj++) {
        GeWorldObjective ob;
        if (!geWorldObjective(obj, &ob) || ob.steps <= 0) { continue; }

        printf("[getv][route] objective %d: %d step(s)\n", obj, ob.steps);
        for (i = 0; i < ob.steps; i++) {
            float ax, az, bx, bz;
            int clear, parts;

            if (!geWorldRouteStep(obj, i, &step)) { continue; }
            if (!ge_ra_pos(step.from, &ax, &az) || !ge_ra_pos(step.to, &bx, &bz)) {
                unknown++;
                continue;
            }
            tested++;
            clear = gePortPathClear(ax, az, bx, bz);
            parts = gePortPathClearParts(ax, az, bx, bz);
            {
                /* Both directions. stanTestLineUnobstructed walks from a seeded tile, so the
                 * answer can depend on which end it started from -- a known weakness of this
                 * family of test. A step that is clear one way and blocked the other is telling
                 * us about the seed rather than about the geometry. */
                int back = gePortPathClear(bx, bz, ax, az);
                if (clear != back) { asym++; }
            }
            if (clear == 0) {
                const char *why;
                blocked++;
                /* Which half objected. A wall between the points is a real obstruction; a
                 * destination a body does not fit in is a badly placed waypoint, and the bot
                 * walks straight through several steps reported that way. */
                if ((parts & 1) == 0 && (parts & 2) == 0) { why = "wall, and no room at the far end"; wall++; }
                else if ((parts & 1) == 0)                { why = "wall between the two points";     wall++; }
                else                                      { why = "no room to stand at the far end"; noroom++; }
                printf("[getv][route]   step %2d  %d -> %d  BLOCKED  (%.0f %.0f) to (%.0f %.0f)"
                       "  %.0f units  -- %s\n",
                       i, step.from, step.to, (double) ax, (double) az, (double) bx, (double) bz,
                       (double) sqrt((double) ((bx-ax)*(bx-ax) + (bz-az)*(bz-az))), why);

                /* Across the corridor rather than along it.
                 *
                 * The waypoints run down z=-133 while the player walks at z=-60, and a step can
                 * only be called impossible once the rest of the width has been asked too. If
                 * the same step is clear a little to one side, the route is not blocked -- it is
                 * drawn through the furniture, and the fix is a lane offset rather than a
                 * different path. */
                {
                    float off;
                    int first = 1;
                    for (off = -200.0f; off <= 200.0f; off += 40.0f) {
                        if (gePortPathClear(ax, az + off, bx, bz + off) == 1) {
                            if (first) { printf("[getv][route]            clear at z offset:"); first = 0; }
                            printf(" %+.0f", (double) off);
                        }
                    }
                    if (!first) { printf("\n"); }
                    else        { printf("[getv][route]            no clear lane within 200 units either side\n"); }
                }
            } else if (clear < 0) {
                unknown++;
            }
        }
        printf("[getv][route] objective %d: %d of %d steps blocked (%d by a wall, %d by no room "
               "at the far end), %d unanswerable, %d disagree by direction\n",
               obj, blocked, tested, wall, noroom, unknown, asym);
        blocked = tested = unknown = asym = wall = noroom = 0;
    }
    fflush(stdout);
}
