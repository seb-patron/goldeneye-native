/* Does the stable sense actually guarantee what it claims?
 *
 * The acceptance test is that the same standing position gives the same answer. That guarantee
 * lives entirely in the quantisation, so it can be checked here with no engine, no level and no
 * running frame: if two positions map to the same query, the sampling below them is handed
 * identical inputs and cannot disagree.
 *
 * Testing it this way rather than by watching a bot is deliberate. Watching would measure the
 * quantiser, the sampler, the level and the bot's own movement at once, and a flicker could come
 * from any of them. This isolates the one property the wrapper is responsible for.
 *
 * geSenseAheadForBody is stubbed. What it returns is irrelevant to the question -- the claim is
 * that it is CALLED with the same arguments, so the stub records its arguments and the test
 * compares those.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../src/ge_sense_api.h"

/* Stub: record what the wrapper asked for. */
static float g_last_x, g_last_z, g_last_h, g_last_reach;
static int   g_calls;

int geSenseAheadForBody(float x, float z, float heading_deg, float reach, GeSenseContact *out)
{
    g_last_x = x; g_last_z = z; g_last_h = heading_deg; g_last_reach = reach;
    g_calls++;
    if (out) {
        out->what = GE_SENSE_CLEAR;
        out->distance = reach;
        out->x = x;
        out->z = z;
    }
    return 1;
}

int gePortCanStandAt(float x, float z) { (void) x; (void) z; return 1; }

#include "../src/ge_sense_stable.c"

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
    else       { printf("  ok  : %s\n", what); }
}

int main(void)
{
    GeSenseContact c;
    float ax, az, ah, bx, bz, bh;
    int i;

    printf("quantisation, the property the guarantee rests on\n");

    /* A cell maps to one value everywhere inside it. */
    check(geSenseQuantise(0.0f, 16.0f) == geSenseQuantise(15.9f, 16.0f),
          "0.0 and 15.9 fall in the same 16-unit cell");
    check(geSenseQuantise(0.0f, 16.0f) != geSenseQuantise(16.1f, 16.0f),
          "16.1 falls in the next cell");
    check(geSenseQuantise(-1.0f, 16.0f) == geSenseQuantise(-15.9f, 16.0f),
          "negative coordinates quantise consistently");

    /* The represented point is inside the cell it stands for, not on its boundary. */
    {
        float q = geSenseQuantise(40.0f, 16.0f);
        check(q > 32.0f && q < 48.0f, "the cell's representative sits inside the cell");
    }

    /* Angles wrap. */
    check(geSenseQuantiseAngle(359.0f, 5.0f) == geSenseQuantiseAngle(-1.0f, 5.0f),
          "359 and -1 are the same heading");
    check(geSenseQuantiseAngle(0.5f, 5.0f) == geSenseQuantiseAngle(4.4f, 5.0f),
          "0.5 and 4.4 fall in the same 5-degree arc");
    check(geSenseQuantiseAngle(722.0f, 5.0f) == geSenseQuantiseAngle(2.0f, 5.0f),
          "headings beyond a full turn wrap");

    printf("\nthe guarantee itself: jitter inside a cell must not change the query\n");

    /* THIS IS THE ACCEPTANCE TEST, and the first version of it asserted something false.
     *
     * It started at x=1000 and drifted 0.9 per step, expecting fifteen steps to leave the query
     * unchanged. 1000 sits in the cell spanning 992 to 1008, so step nine reached 1008.1 and
     * crossed into the next one. That is the quantiser working, not failing: cells have
     * boundaries, and any drift long enough will cross one.
     *
     * SO THE GUARANTEE IS NARROWER THAN "DRIFT IS STABLE" AND HAS TO BE STATED THAT WAY. What
     * holds is that two positions IN THE SAME CELL are the same query. A bot crossing a boundary
     * still sees the answer change, but at most once per crossing and bounded by how fast it
     * moves, instead of alternating every frame. That is the difference between an avoidance
     * hold that can be held and one that cannot.
     *
     * Tested by staying inside one cell deliberately rather than by hoping a drift does. */
    geSenseAheadForBodyStable(1000.0f, 2000.0f, 90.0f, 400.0f, &c);
    ax = g_last_x; az = g_last_z; ah = g_last_h;

    for (i = 1; i <= 15; i++) {
        geSenseAheadForBodyStable(996.0f + (float) i * 0.5f,
                                  2000.0f + (float) i * 0.4f,
                                  90.5f + (float) i * 0.1f, 400.0f, &c);
        bx = g_last_x; bz = g_last_z; bh = g_last_h;
        if (bx != ax || bz != az || bh != ah) {
            printf("  FAIL: step %d moved the query inside one cell\n", i);
            fails++;
            break;
        }
    }
    if (i > 15) { printf("  ok  : 15 positions inside one cell gave one query\n"); }

    /* The boundary case, stated rather than hidden: crossing a cell DOES change the query, and
     * that is the residual instability a caller has to live with. */
    geSenseAheadForBodyStable(1007.9f, 2000.0f, 90.0f, 400.0f, &c);
    ax = g_last_x;
    geSenseAheadForBodyStable(1008.1f, 2000.0f, 90.0f, 400.0f, &c);
    check(g_last_x != ax, "crossing a cell boundary does change the query, as it must");

    /* And a real move must still change it, or the sensor has simply gone deaf. */
    geSenseAheadForBodyStable(1000.0f + 64.0f, 2000.0f, 90.0f, 400.0f, &c);
    check(g_last_x != ax, "a move of four cells does change the query");

    printf("\nself-clearance: the walk must not start inside the asker\n");
    geSenseAheadForBodyStable(0.0f, 0.0f, 0.0f, 400.0f, &c);
    /* Measured as a DISTANCE, not along one axis. The heading quantises to the middle of its
     * arc -- 2.5 degrees for heading 0 -- so the offset is not purely along z, and comparing the
     * z component alone fails by cos(2.5 degrees) for reasons that have nothing to do with
     * whether the clearance is right. */
    {
        float dx = g_last_x - geSenseQuantise(0.0f, 16.0f);
        float dz = g_last_z - geSenseQuantise(0.0f, 16.0f);
        double d = sqrt((double) (dx * dx + dz * dz));
        check(fabs(d - 60.0) < 0.01, "the walk begins a body diameter out, not one radius");
    }
    check(fabs((double) (g_last_reach - (400.0f - 60.0f))) < 0.001,
          "reach is reduced by the same offset so the far end is unchanged");
    check(fabs((double) (c.distance - 400.0f)) < 0.001,
          "reported distance is back in the caller's frame");

    printf("\nreach shorter than the asker's own body\n");
    g_calls = 0;
    geSenseAheadForBodyStable(0.0f, 0.0f, 0.0f, 40.0f, &c);
    check(g_calls == 0, "no sample is taken when the whole reach is inside the asker");
    check((c.what & GE_SENSE_SOLID) == 0, "and it reports clear rather than reporting itself");

    printf("\n%s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
