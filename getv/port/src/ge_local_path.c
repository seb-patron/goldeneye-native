/* A route through ground the body actually fits on, searched rather than looked up.
 *
 * The level's own pad graph is what the route bot follows, and on Train it is wrong in a way that
 * matters: the pads run about seventy units off the walkable centre, and the engine's walkability
 * test refuses 18 of the level's 46 route steps. Worse, the pads say nothing about the row of
 * crates at x=373 that blocks the first carriage, because a crate is not a navigation feature --
 * it is scenery that happens to be in the way.
 *
 * So ask the floor instead. Sample standability on a grid around the player, breadth-first from
 * where the body is to whichever reachable cell gets nearest the objective, and hand back the
 * corners. It is a local search -- a few thousand cells -- and it is deliberately not a
 * replacement for the pad graph over a whole level. It is the answer to one question the pad
 * graph cannot answer: how do I get round the thing in front of me.
 *
 * The cost is one stan query per cell, paid only when asked.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int gePortCanStandAt(float x, float z);

/* The props the world pack knows about. The floor test answers "is there ground here" and knows
 * nothing about what is STANDING on the ground -- so a search built on it alone routes straight
 * through the row of crates across Train's first carriage, which the report has been naming, with
 * positions, the whole time. Reading them here is the difference between a route a body can walk
 * and a route a ghost can. */
typedef struct { int kind, room, tag, nav_node; float x, y, z; float pad[8]; float hx, hz, radius; } GeLpProp;
extern int geWorldPropCount(void);
extern int geWorldProp(int index, void *out);

/* 30, not 40. The search is one standability query per cell, so the cost is quadratic in this
 * number: 40 is 6,561 cells against 30's 3,721, and the driver measurably lost ground when the
 * search got slower -- it reached 9.5m into the first carriage instead of 13.6m, at 29% health
 * instead of 94%. A wider view is worth nothing if the player is standing still while it is
 * computed. */
#define GE_LP_HALF   30                       /* cells either side of the player            */
#define GE_LP_DIM    (GE_LP_HALF * 2 + 1)
#define GE_LP_CELLS  (GE_LP_DIM * GE_LP_DIM)

static unsigned char ge_lp_open[GE_LP_CELLS];
static short         ge_lp_from[GE_LP_CELLS];   /* -1 unvisited, else the cell we came from */
static short         ge_lp_queue[GE_LP_CELLS];

/* Fills out_x/out_z with up to max corners of a route from (px,pz) towards (tx,tz), and returns
 * how many it wrote. Zero means nothing reachable made progress, which is a real answer: it says
 * the way on is not within reach of a body from here, rather than that the search failed. */
int gePortLocalPath(float px, float pz, float tx, float tz, float cell,
                    float *out_x, float *out_z, int max)
{
    int head = 0, tail = 0, i, best = -1, n = 0;
    float best_d = 0.0f;
    short chain[GE_LP_CELLS];
    int chain_n = 0;

    if (cell < 10.0f) { cell = 60.0f; }
    if (out_x == NULL || out_z == NULL || max <= 0) { return 0; }

    for (i = 0; i < GE_LP_CELLS; i++) { ge_lp_from[i] = -1; }

    /* Sample first, search second. Doing both at once re-queries cells the search revisits, and
     * the query is the expensive half. */
    for (i = 0; i < GE_LP_CELLS; i++) {
        int gx = (i % GE_LP_DIM) - GE_LP_HALF;
        int gz = (i / GE_LP_DIM) - GE_LP_HALF;
        ge_lp_open[i] = (unsigned char) gePortCanStandAt(px + (float) gx * cell,
                                                         pz + (float) gz * cell);
    }

    /* Close the cells the props occupy. Radius is the CIRCUMRADIUS, so it over-reserves for a
     * square footprint by about 41% -- the right direction to be wrong in when the alternative is
     * walking into the corner of a crate. A prop with no radius is left alone rather than guessed
     * at: guards have no model box, and closing a cell per guard would wall the level in. */
    {
        extern float gePortPropFootprint(int index, float *x, float *z);
        int pi, pn = geWorldPropCount();
        for (pi = 0; pi < pn; pi++) {
            float ox, oz, r = gePortPropFootprint(pi, &ox, &oz);
            int gx0, gx1, gz0, gz1, gx, gz;

            if (r <= 0.0f) { continue; }
            gx0 = (int) ((ox - r - px) / cell) - 1 + GE_LP_HALF;
            gx1 = (int) ((ox + r - px) / cell) + 1 + GE_LP_HALF;
            gz0 = (int) ((oz - r - pz) / cell) - 1 + GE_LP_HALF;
            gz1 = (int) ((oz + r - pz) / cell) + 1 + GE_LP_HALF;
            if (gx1 < 0 || gz1 < 0 || gx0 >= GE_LP_DIM || gz0 >= GE_LP_DIM) { continue; }
            if (gx0 < 0) { gx0 = 0; }
            if (gz0 < 0) { gz0 = 0; }
            if (gx1 >= GE_LP_DIM) { gx1 = GE_LP_DIM - 1; }
            if (gz1 >= GE_LP_DIM) { gz1 = GE_LP_DIM - 1; }

            for (gz = gz0; gz <= gz1; gz++) {
                for (gx = gx0; gx <= gx1; gx++) {
                    float cx = px + (float) (gx - GE_LP_HALF) * cell;
                    float cz = pz + (float) (gz - GE_LP_HALF) * cell;
                    float dx2 = cx - ox, dz2 = cz - oz;
                    /* Never close the ground under the player's feet or immediately around it.
                     * A brake unit is a prop, and the player has to stand next to one to shoot
                     * it -- closing that ring walled the body in against the thing it had just
                     * destroyed, and the search then reported, correctly and uselessly, that
                     * nothing was reachable. */
                    if (gx >= GE_LP_HALF - 1 && gx <= GE_LP_HALF + 1
                        && gz >= GE_LP_HALF - 1 && gz <= GE_LP_HALF + 1) { continue; }
                    if (dx2 * dx2 + dz2 * dz2 <= r * r) { ge_lp_open[gz * GE_LP_DIM + gx] = 0; }
                }
            }
        }
    }

    i = GE_LP_HALF * GE_LP_DIM + GE_LP_HALF;    /* the player's own cell */
    ge_lp_open[i] = 1;                          /* the body is standing there, whatever the grid says */
    ge_lp_from[i] = i;
    ge_lp_queue[tail++] = (short) i;

    while (head < tail) {
        int cur = ge_lp_queue[head++];
        int cx = cur % GE_LP_DIM, cz = cur / GE_LP_DIM;
        float wx = px + (float) (cx - GE_LP_HALF) * cell;
        float wz = pz + (float) (cz - GE_LP_HALF) * cell;
        float d = (float) sqrt((double) ((tx - wx) * (tx - wx) + (tz - wz) * (tz - wz)));
        int k;
        static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        static const int dz[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

        if (best < 0 || d < best_d) { best_d = d; best = cur; }

        for (k = 0; k < 8; k++) {
            int nx = cx + dx[k], nz = cz + dz[k], ni;
            if (nx < 0 || nx >= GE_LP_DIM || nz < 0 || nz >= GE_LP_DIM) { continue; }
            ni = nz * GE_LP_DIM + nx;
            if (!ge_lp_open[ni] || ge_lp_from[ni] >= 0) { continue; }
            /* No cutting corners: a diagonal is only walkable if both of its sides are, or the
             * route clips the corner of whatever made the diagonal necessary. */
            if (dx[k] != 0 && dz[k] != 0) {
                if (!ge_lp_open[cz * GE_LP_DIM + nx] || !ge_lp_open[nz * GE_LP_DIM + cx]) { continue; }
            }
            ge_lp_from[ni] = (short) cur;
            ge_lp_queue[tail++] = (short) ni;
        }
    }

    if (best < 0 || best == GE_LP_HALF * GE_LP_DIM + GE_LP_HALF) { return 0; }

    /* Walk the chain back to the player, then read it forwards. */
    for (i = best; i != ge_lp_from[i] && chain_n < GE_LP_CELLS; i = ge_lp_from[i]) {
        chain[chain_n++] = (short) i;
    }

    /* Corners only. Every cell of a straight run is the same instruction repeated, and a caller
     * given three hundred of them cannot see the shape of the route. */
    for (i = chain_n - 1; i >= 0 && n < max; i--) {
        int cur = chain[i];
        int keep = (i == 0);

        if (!keep && i < chain_n - 1) {
            int prev = chain[i + 1], next = chain[i - 1];
            int ax = (cur % GE_LP_DIM) - (prev % GE_LP_DIM);
            int az = (cur / GE_LP_DIM) - (prev / GE_LP_DIM);
            int bx = (next % GE_LP_DIM) - (cur % GE_LP_DIM);
            int bz = (next / GE_LP_DIM) - (cur / GE_LP_DIM);
            keep = (ax != bx || az != bz);
        }
        if (!keep) { continue; }

        out_x[n] = px + (float) ((cur % GE_LP_DIM) - GE_LP_HALF) * cell;
        out_z[n] = pz + (float) ((cur / GE_LP_DIM) - GE_LP_HALF) * cell;
        n++;
    }
    return n;
}
