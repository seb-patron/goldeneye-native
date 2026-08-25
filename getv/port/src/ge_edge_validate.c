/* Ask the engine which waypoint links a body can actually walk, and write the answers down.
 *
 * WHY THIS EXISTS
 *
 * Every graph edge in this project has been an assumption. The waypoint links come from pad
 * adjacency, the spawn and door and portal links come from proximity, and the only walkability
 * test available offline is triangle intersection against exported wall polygons. That test
 * DISAGREES WITH THE GAME: it passes the Bunker 1 spawn-to-portal line that
 * bondviewTestLineUnobstructed refuses, so the router kept choosing a 1136-unit leap through a
 * wall over the 555-unit walk to the door, and the bot pressed into that wall every run.
 *
 * A route built on an edge that does not exist is the most expensive failure shape this project
 * has: every step is well formed, every distance is plausible, nothing exits non-zero, and the
 * bot walks confidently into geometry. The engine is the authority on what is walkable. This
 * asks it, once per level, and writes the verdicts where the generator can read them.
 *
 * WHAT IT MEASURES
 *
 * Every ORDERED PAIR of waypoints closer than the cutoff. Ordered, not unordered: the test is
 * seeded from a tile and is not guaranteed symmetric, and quietly assuming it is would hide
 * exactly the one-way cases -- a ledge you can drop off but not climb -- that matter most.
 * Disagreements are reported rather than averaged away.
 *
 *   GETV_EDGEVALIDATE=1              run it, then exit
 *   GETV_EDGEVALIDATE_OUT=<path>     where to write (default build/levels/<level>.edges.txt)
 *   GETV_EDGEVALIDATE_MAX=<units>    cutoff, default 1600
 *   GETV_EDGEVALIDATE_FRAME=<n>      which frame to run on, default 601
 *
 * ⚠️ It must run LATE ENOUGH that the level is fully placed. The stan tiles the test seeds from
 * are not ready at frame 1, and a run against a half-built level reports a wall everywhere --
 * which looks exactly like a correct measurement of a sealed map.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_world_api.h"

extern int gePortProbeWalkable(float from_x, float from_z, float to_x, float to_z);

static int ge_ev_done;

static int ge_ev_env_int(const char *name, int fallback)
{
    const char *e = getenv(name);
    return (e != NULL && *e != '\0') ? atoi(e) : fallback;
}

void gePortEdgeValidateFrame(int frame)
{
    const char *on = getenv("GETV_EDGEVALIDATE");
    const char *out_path;
    char def_path[512];
    float max_d;
    int want_frame, n, i, j, pairs = 0, walkable = 0, asym = 0;
    FILE *fp;

    if (ge_ev_done || on == NULL || *on == '\0' || *on == '0') { return; }

    want_frame = ge_ev_env_int("GETV_EDGEVALIDATE_FRAME", 601);
    if (frame < want_frame) { return; }
    ge_ev_done = 1;

    if (!geWorldLoaded()) {
        /* Loudly, and non-zero would be better still: a validator that silently writes nothing
         * leaves the generator using its assumptions and believing they were checked. */
        fprintf(stderr, "[getv][edges] NO WORLD LOADED -- set GETV_WORLD_DIR and "
                        "GETV_BOT_ROUTE_LEVEL. Nothing written.\n");
        return;
    }

    n = geWorldWaypointCount();
    max_d = (float) ge_ev_env_int("GETV_EDGEVALIDATE_MAX", 1600);

    out_path = getenv("GETV_EDGEVALIDATE_OUT");
    if (out_path == NULL || *out_path == '\0') {
        snprintf(def_path, sizeof def_path, "build/levels/%s.edges.txt", geWorldLevel());
        out_path = def_path;
    }

    fp = fopen(out_path, "w");
    if (fp == NULL) {
        fprintf(stderr, "[getv][edges] cannot write %s\n", out_path);
        return;
    }

    fprintf(fp, "# level=%s waypoints=%d max=%.0f frame=%d\n",
            geWorldLevel(), n, (double) max_d, frame);
    fprintf(fp, "# from to dist walkable   (measured with bondviewTestLineUnobstructed,"
                " CDTYPE_BG|CDTYPE_PATHBLOCKER)\n");

    for (i = 0; i < n; i++) {
        GeWorldWaypoint a;
        if (!geWorldWaypoint(i, &a)) { continue; }

        for (j = 0; j < n; j++) {
            GeWorldWaypoint b;
            float dx, dz, d2;
            int ab, ba;

            if (i == j || !geWorldWaypoint(j, &b)) { continue; }

            dx = b.x - a.x;
            dz = b.z - a.z;
            d2 = (dx * dx) + (dz * dz);
            if (d2 > max_d * max_d) { continue; }

            ab = gePortProbeWalkable(a.x, a.z, b.x, b.z);
            ba = gePortProbeWalkable(b.x, b.z, a.x, a.z);
            if (ab != ba) { asym++; }

            pairs++;
            if (ab) { walkable++; }

            fprintf(fp, "%d %d %.0f %d\n", a.id, b.id, (double) (d2 > 0.0f ? sqrtf(d2) : 0.0f), ab);
        }
    }

    fclose(fp);
    printf("[getv][edges] %s: %d pair(s) within %.0f units, %d walkable (%d%%), "
           "%d asymmetric -> %s\n",
           geWorldLevel(), pairs, (double) max_d, walkable,
           pairs ? (walkable * 100 / pairs) : 0, asym, out_path);
    fflush(stdout);
}
