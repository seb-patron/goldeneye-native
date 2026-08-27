/* A body sense that gives the same answer twice.
 *
 * The acceptance test for this is STABILITY, not hit rate: the same standing position must produce
 * the same answer frame after frame. A sensor that answers differently between frames is worse
 * than one that answers wrongly and consistently, because a consistent answer can be held and an
 * inconsistent one cannot. The follower's avoidance manoeuvre was held only while the sensor kept
 * agreeing, and it did not agree, so the hold released and re-applied within a frame or two and
 * the bot rocked inside a six degree band while barely moving.
 *
 * TWO CAUSES, AND THEY TURN OUT TO BE ONE.
 *
 * geSenseAheadForBody walks gePortCanStandAt forward in steps of GE_BODY_RADIUS starting at
 * d = GE_BODY_RADIUS. That first sample sits exactly one body radius ahead of the asker, which is
 * INSIDE the asker's own body: a volume query centred there overlaps the very character asking the
 * question. It answers "a body cannot stand here" and it is right -- one already does.
 *
 * That is the 93% contact rate at a mean of 30 units, and 30 units is GE_BODY_RADIUS exactly. The
 * mean is not near the first sample, it IS the first sample: the sensor was almost always
 * reporting its own body back.
 *
 * And it is also the flicker. Whether that self-overlap registers depends on where the asker sits
 * within the sample cell, so sub-unit drift flips it. A false positive that is stable would have
 * been merely wrong; one that sits exactly on a boundary alternates.
 *
 * SO THE WALK STARTS CLEAR OF THE ASKER. The first sample goes at a full body DIAMETER, which is
 * the nearest point whose body-sized volume cannot contain the asker's own centre.
 *
 * AND THE QUERY IS QUANTISED, which is what makes the guarantee a guarantee rather than a hope.
 * Position is snapped to a cell and heading to an arc before anything is sampled, so any two
 * queries within one cell produce bit-identical inputs and therefore bit-identical output. "Same
 * standing position, same answer" then holds by construction, and so does the case that actually
 * matters: a bot drifting a unit per frame stays inside its cell and sees no change at all.
 *
 * Quantising is preferred to hysteresis or to averaging across frames because it is STATELESS. A
 * filter with memory answers differently depending on how it was approached, which is a different
 * instability wearing a smoother coat, and it cannot be unit tested without simulating a history.
 * This can: two positions in a cell, one call each, compare.
 *
 * WHAT THIS DOES NOT CHANGE. It does not touch geSenseAheadForBody, which stays as it is and stays
 * mac's. This is a wrapper, so the swap is one call site in ge_bot_route.c and reverting it is the
 * same edit backwards.
 *
 *   GETV_SENSE_CELL=<units>    position quantum, default 16
 *   GETV_SENSE_ARC=<degrees>   heading quantum, default 5
 *   GETV_SENSE_TRACE=1         report the flip rate, which is the number this exists to move
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "ge_sense_api.h"

/* The nearest sample whose body-sized volume cannot contain the asker's own centre. One radius
 * out is inside the asker; one diameter out is the first point that is not. */
#define GE_SENSE_SELF_CLEAR (2.0f * GE_BODY_RADIUS)

static float ge_cell = 16.0f;
static float ge_arc = 5.0f;
static int   ge_trace;
static int   ge_ready;

/* Flip accounting, for the acceptance test. */
static unsigned long ge_calls, ge_flips;
static int           ge_last_valid, ge_last_blocked;
static float         ge_last_qx, ge_last_qz, ge_last_qh;

static void ge_stable_init(void)
{
    const char *c = getenv("GETV_SENSE_CELL");
    const char *a = getenv("GETV_SENSE_ARC");
    ge_ready = 1;
    if (c && *c) { ge_cell = (float) atof(c); }
    if (a && *a) { ge_arc = (float) atof(a); }
    if (ge_cell <= 0.0f) { ge_cell = 16.0f; }
    if (ge_arc <= 0.0f) { ge_arc = 5.0f; }
    ge_trace = getenv("GETV_SENSE_TRACE") != NULL;
}

/* Snap to the centre of a cell. Centres rather than edges so the represented point is inside the
 * cell it stands for; snapping to an edge puts the sample on the boundary between two cells, which
 * is the position most likely to be ambiguous and exactly what this is trying to avoid. */
float geSenseQuantise(float v, float cell)
{
    float n = (float) floor((double) v / (double) cell);
    return (n + 0.5f) * cell;
}

/* Same, on a circle. Kept separate because a heading wraps and a coordinate does not: 359 and 1
 * are two degrees apart and must land in the same bucket when the arc spans them. */
float geSenseQuantiseAngle(float deg, float arc)
{
    float w = (float) fmod((double) deg, 360.0);
    float n;
    if (w < 0.0f) { w += 360.0f; }
    n = (float) floor((double) w / (double) arc);
    w = (n + 0.5f) * arc;
    if (w >= 360.0f) { w -= 360.0f; }
    return w;
}

int geSenseAheadForBodyStable(float x, float z, float heading_deg, float reach,
                              GeSenseContact *out)
{
    float qx, qz, qh;
    int blocked, moved;

    if (out == NULL) { return 0; }
    if (!ge_ready) { ge_stable_init(); }

    qx = geSenseQuantise(x, ge_cell);
    qz = geSenseQuantise(z, ge_cell);
    qh = geSenseQuantiseAngle(heading_deg, ge_arc);

    /* Push the origin forward so the walk begins outside the asker's own volume, rather than
     * starting the walk at a radius and calling the asker an obstacle. Doing it by moving the
     * ORIGIN rather than by skipping the first sample keeps every reported distance measured from
     * where the caller actually stands, so a caller comparing distance against its own reach does
     * not have to know this happened. */
    {
        float rad = (float) (qh * 3.14159265358979323846 / 180.0);
        float fx = (float) sin((double) rad);
        float fz = (float) cos((double) rad);
        float ox = qx + fx * GE_SENSE_SELF_CLEAR;
        float oz = qz + fz * GE_SENSE_SELF_CLEAR;
        float r = reach - GE_SENSE_SELF_CLEAR;

        if (r <= 0.0f) {
            /* Asked about a reach shorter than the asker's own body. Nothing can be said about it
             * that is not just the asker, so report clear rather than reporting itself. */
            out->what = GE_SENSE_CLEAR;
            out->distance = reach;
            out->x = x;
            out->z = z;
            return 1;
        }

        geSenseAheadForBody(ox, oz, qh, r, out);
        /* Restore the caller's frame: the walk started a diameter out, so every distance it
         * reports is short by that much. */
        out->distance += GE_SENSE_SELF_CLEAR;
    }

    blocked = (out->what & GE_SENSE_SOLID) != 0;
    moved = (ge_last_qx != qx) || (ge_last_qz != qz) || (ge_last_qh != qh);

    /* A FLIP IS ONLY A FLIP WHEN THE QUERY DID NOT CHANGE. If the bot moved to another cell the
     * world genuinely differs and a different answer is correct, not unstable. Counting those
     * would report movement as noise and make the figure meaningless. */
    ge_calls++;
    if (ge_last_valid && !moved && blocked != ge_last_blocked) {
        ge_flips++;
        if (ge_trace) {
            printf("[getv][sense] FLIP at the same query: cell (%.0f,%.0f) arc %.0f -> %s\n",
                   qx, qz, qh, blocked ? "blocked" : "clear");
            fflush(stdout);
        }
    }
    ge_last_valid = 1;
    ge_last_blocked = blocked;
    ge_last_qx = qx;
    ge_last_qz = qz;
    ge_last_qh = qh;

    if (ge_trace && (ge_calls % 600ul) == 0ul) {
        printf("[getv][sense] %lu calls, %lu flips at an unchanged query (%.2f%%)\n",
               ge_calls, ge_flips, 100.0 * (double) ge_flips / (double) ge_calls);
        fflush(stdout);
    }
    return 1;
}

/* For the harness: the flip rate is the acceptance test, so it has to be readable from outside. */
void geSenseStableStats(unsigned long *calls, unsigned long *flips)
{
    if (calls) { *calls = ge_calls; }
    if (flips) { *flips = ge_flips; }
}
