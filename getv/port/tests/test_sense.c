/* The sensing API's port-side logic.
 *
 * Every function here rests on game-side accessors that this machine does not have, so all of it
 * is driven through stubs. That is not a weaker test than the others in this directory -- the
 * arithmetic being checked is entirely port-side: which of three parallel lines wins, whether a
 * bearing falls inside a cone, whether "commanded to move and did not" holds over a window.
 *
 * The cases that matter are the ones where a plausible implementation is wrong:
 *
 *   a body is not a ray -- the CENTRE line being clear is not enough
 *   sight is not attention -- a line to a guard facing away is not being seen
 *   absent facing is not facing-away -- they lead to opposite behaviour
 *   standing still on purpose is not being stuck
 */

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- the fake world */

/* Blocking segments, as vertical planes at a given x. A ray from x0 to x1 is blocked if it crosses
 * one. Crude on purpose: the geometry under test is the SWEEP, not the line test. */
#define FAKE_BLOCKERS 4
static struct { float at_x; float z_lo, z_hi; unsigned int what; } blocker[FAKE_BLOCKERS];
static int n_blockers;

static int fake_line_hits(float fx, float fz, float tx, float tz, unsigned int *what)
{
    int i;
    for (i = 0; i < n_blockers; i++) {
        float bx = blocker[i].at_x;
        if ((fx < bx) == (tx < bx)) { continue; }           /* both sides: no crossing */
        /* Where the segment crosses that plane, in z. */
        {
            float t = (bx - fx) / (tx - fx);
            float cz = fz + (tz - fz) * t;
            if (cz >= blocker[i].z_lo && cz <= blocker[i].z_hi) {
                if (what) { *what = blocker[i].what; }
                return 1;
            }
        }
    }
    return 0;
}

int gePortSenseLine(float fx, float fz, float tx, float tz)
{
    unsigned int what = 0;
    return fake_line_hits(fx, fz, tx, tz, &what) ? (int) what : 0;
}

static int fake_visible = 1;
int gePortSenseVisibleTo(int chr_index, int player_index)
{
    (void) chr_index; (void) player_index;
    return fake_visible;
}

/* Facing accessor: `fake_facing_ok` models a build that does not have it. */
static int   fake_facing_ok = 1;
static float fake_facing_deg;
int gePortEnemyFacing(int chr_index, float *out_deg)
{
    (void) chr_index;
    if (!fake_facing_ok) { return 0; }
    if (out_deg) { *out_deg = fake_facing_deg; }
    return 1;
}

#define FAKE_USABLES 4
static struct { float x, y, z, kind, prop; } usable[FAKE_USABLES];
static int n_usables;
int gePortUsableCount(void) { return n_usables; }
int gePortUsableAt(int index, float *out)
{
    if (index < 0 || index >= n_usables || out == NULL) { return 0; }
    out[0] = usable[index].x; out[1] = usable[index].y; out[2] = usable[index].z;
    out[3] = usable[index].kind; out[4] = usable[index].prop;
    return 1;
}

/* Player and enemy state. */
static float fake_px, fake_pz;
static int   fake_present = 1;

/* No forward declaration here: GePlayerState is not in scope until the unit under test pulls in
 * ge_player_api.h, and that header already declares gePlayerStateGet. Defining it below the
 * include is enough. */
#include "ge_sense_api.c"

/* Defined after the include so GePlayerState and GeEnemy are in scope. */
int gePlayerStateGet(int slot, GePlayerState *out)
{
    if (slot != 0 || out == NULL) { return 0; }
    memset(out, 0, sizeof *out);
    if (!fake_present) { return 1; }
    out->present = 1;
    out->fields = GE_ST_POSITION;
    out->x = fake_px; out->z = fake_pz;
    return 1;
}

static float fake_ex, fake_ez;
static int   fake_alertness = 100;
static int   fake_enemy_alive = 1;

int geEnemySourceInstalled(void) { return 1; }
int geEnemyCount(void) { return 1; }
int geEnemy(int index, GeEnemy *out)
{
    if (index != 0 || out == NULL) { return 0; }
    memset(out, 0, sizeof *out);
    out->id = 7;
    out->alive = fake_enemy_alive;
    out->fields = GE_EN_POSITION | GE_EN_ALERT;
    out->x = fake_ex; out->z = fake_ez;
    out->alertness = fake_alertness;
    return 1;
}
int geEnemyById(int id, GeEnemy *out) { (void) id; return geEnemy(0, out); }
int geEnemiesNear(float x, float y, float z, float r, GeEnemy *o, int m)
{ (void) x; (void) y; (void) z; (void) r; (void) o; (void) m; return 0; }
int geEnemyThreatAt(float x, float y, float z, float r)
{ (void) x; (void) y; (void) z; (void) r; return 0; }
void geEnemySourceInstall(GeEnemyCountFn c, GeEnemyAtFn a) { (void) c; (void) a; }

/* ---------------------------------------------------------------- harness */

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %-52s %d\n", what, got);
    } else {
        printf("  FAIL  %-52s got %d want %d\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    GeSenseContact c;
    GeUsable u[4];
    unsigned int m;
    int n, i;

    printf("sensing API\n\n");

    /* ---------------- 1c: a body is not a ray ----------------
     *
     * A gap the centre line passes through cleanly but a body cannot fit. Two blockers leave a
     * 40-unit slot at z=0; the body radius is 30, so the shoulders are at +/-30 and both hit.
     * A ray-only test calls this clear, which is the bug the sweep exists for. */
    n_blockers = 2;
    blocker[0].at_x = 100.0f; blocker[0].z_lo =  20.0f; blocker[0].z_hi = 500.0f;
    blocker[0].what = GE_SENSE_WALL;
    blocker[1].at_x = 100.0f; blocker[1].z_lo = -500.0f; blocker[1].z_hi = -20.0f;
    blocker[1].what = GE_SENSE_WALL;

    /* Heading +90 degrees is +x under atan2(x, z). */
    geSenseAhead(0.0f, 0.0f, 90.0f, 300.0f, &c);
    check("ray alone calls the gap clear",     c.what == GE_SENSE_CLEAR, 1);
    geSenseAheadForBody(0.0f, 0.0f, 90.0f, 300.0f, &c);
    check("BODY sweep refuses the gap",        c.what != GE_SENSE_CLEAR, 1);

    /* Widen the gap past the body and it must open. Without this the sweep could be refusing
     * everything and still pass the case above. */
    blocker[0].z_lo =  60.0f;
    blocker[1].z_hi = -60.0f;
    geSenseAheadForBody(0.0f, 0.0f, 90.0f, 300.0f, &c);
    check("wide gap passes the body sweep",    c.what == GE_SENSE_CLEAR, 1);

    n_blockers = 0;
    geSenseAheadForBody(0.0f, 0.0f, 90.0f, 300.0f, &c);
    check("open space is clear",               c.what == GE_SENSE_CLEAR, 1);

    /* ---------------- 1a: sight is not attention ---------------- */

    fake_px = 0.0f;   fake_pz = 0.0f;      /* player at the origin  */
    fake_ex = 100.0f; fake_ez = 0.0f;      /* enemy 100 units at +x */
    fake_visible = 1;
    fake_facing_ok = 1;
    fake_alertness = 100;
    fake_enemy_alive = 1;

    /* The enemy is at +x, so the player is at bearing -90 from it. Facing -90 looks straight at
     * the player; facing +90 looks directly away. */
    fake_facing_deg = -90.0f;
    m = geSenseNoticedBy(0, 0);
    check("looking at me: LINE",               (m & GE_NOTICE_LINE) != 0, 1);
    check("looking at me: FACING",             (m & GE_NOTICE_FACING) != 0, 1);
    check("looking at me: ALERT",              (m & GE_NOTICE_ALERT) != 0, 1);
    check("looking at me: SEEN",               (m & GE_NOTICE_SEEN) == GE_NOTICE_SEEN, 1);

    fake_facing_deg = 90.0f;
    m = geSenseNoticedBy(0, 0);
    check("facing away: still has a LINE",     (m & GE_NOTICE_LINE) != 0, 1);
    check("facing away: NOT facing",           (m & GE_NOTICE_FACING) != 0, 0);
    check("facing away: NOT seen",             (m & GE_NOTICE_SEEN) == GE_NOTICE_SEEN, 0);

    /* Just inside and just outside the cone, so the boundary is asserted rather than assumed. */
    fake_facing_deg = -90.0f + (GE_NOTICE_CONE_DEG - 5.0f);
    check("just inside the cone",              (geSenseNoticedBy(0, 0) & GE_NOTICE_FACING) != 0, 1);
    fake_facing_deg = -90.0f + (GE_NOTICE_CONE_DEG + 5.0f);
    check("just outside the cone",             (geSenseNoticedBy(0, 0) & GE_NOTICE_FACING) != 0, 0);

    /* Blocked line, still facing: must not read as seen, and must still report FACING so a caller
     * knows not to step out. */
    fake_facing_deg = -90.0f;
    fake_visible = 0;
    m = geSenseNoticedBy(0, 0);
    check("blocked: no LINE",                  (m & GE_NOTICE_LINE) != 0, 0);
    check("blocked: still FACING",             (m & GE_NOTICE_FACING) != 0, 1);
    check("blocked: not seen",                 (m & GE_NOTICE_SEEN) == GE_NOTICE_SEEN, 0);
    fake_visible = 1;

    /* Unalerted: a line and a facing but nobody home. */
    fake_alertness = 0;
    m = geSenseNoticedBy(0, 0);
    check("alertness 0: not ALERT",            (m & GE_NOTICE_ALERT) != 0, 0);
    check("alertness 0: not seen",             (m & GE_NOTICE_SEEN) == GE_NOTICE_SEEN, 0);
    fake_alertness = 100;

    /* THE ONE THAT MATTERS MOST. With no facing accessor, absent must not read as facing-away:
     * they lead to opposite behaviour, and a build that reports "nobody is looking" because it
     * cannot tell is the dangerous failure. */
    fake_facing_ok = 0;
    m = geSenseNoticedBy(0, 0);
    check("no accessor: FACE_UNKNOWN set",     (m & GE_NOTICE_FACE_UNKNOWN) != 0, 1);
    check("no accessor: FACING not claimed",   (m & GE_NOTICE_FACING) != 0, 0);
    check("no accessor: not reported as seen", (m & GE_NOTICE_SEEN) == GE_NOTICE_SEEN, 0);
    fake_facing_ok = 1;

    /* A dead enemy notices nothing. */
    fake_enemy_alive = 0;
    check("dead enemy notices nothing",        geSenseNoticedBy(0, 0), GE_NOTICE_NONE);
    fake_enemy_alive = 1;

    /* watchers counts lines; noticing counts all three. */
    fake_facing_deg = 90.0f;                   /* facing away */
    check("watchers sees the line",            geSenseWatchers(0), 1);
    check("noticing does not",                 geSenseNoticing(0), 0);
    fake_facing_deg = -90.0f;
    check("both agree when looking",           geSenseNoticing(0), 1);

    /* ---------------- 1b: contact, not prediction ---------------- */

    /* Commanded to move, and did not: stuck. */
    for (i = 0; i < 10; i++) { geSenseContactUpdate(0, 5.0f, 5.0f, 1); }
    check("commanded and motionless: STUCK",   geSenseIsStuck(0, 8), 1);

    /* Moving freely is never stuck, however long. */
    for (i = 0; i < 10; i++) { geSenseContactUpdate(0, 5.0f + (float) i * 20.0f, 5.0f, 1); }
    check("moving freely: not stuck",          geSenseIsStuck(0, 8), 0);
    check("recent travel is nonzero",          geSenseRecentTravel(0) > 0.0f, 1);

    /* Standing still ON PURPOSE is not stuck. This is the half a naive implementation drops, and
     * dropping it makes every idle bot report itself jammed. */
    for (i = 0; i < 10; i++) { geSenseContactUpdate(0, 9.0f, 9.0f, 0); }
    check("still on purpose: not stuck",       geSenseIsStuck(0, 8), 0);

    check("bad slot is not stuck",             geSenseIsStuck(99, 8), 0);

    /* ---------------- 1d: what can I act on ---------------- */

    n_usables = 3;
    usable[0].x = 500.0f; usable[0].z = 0.0f; usable[0].kind = (float) GE_USABLE_DOOR;
    usable[0].prop = 11.0f;                                    /* out of reach */
    usable[1].x = 150.0f; usable[1].z = 0.0f; usable[1].kind = (float) GE_USABLE_PICKUP;
    usable[1].prop = 22.0f;
    usable[2].x =  40.0f; usable[2].z = 0.0f; usable[2].kind = (float) GE_USABLE_DOOR;
    usable[2].prop = 33.0f;

    n = geSenseUsable(0.0f, 0.0f, 0.0f, u, 4);
    check("two within reach, one not",         n, 2);
    check("nearest first",                     u[0].prop, 33);
    check("  then the further one",            u[1].prop, 22);
    check("kind survives the round trip",      (u[0].kind & GE_USABLE_DOOR) != 0, 1);
    check("distance is filled",                (int) u[0].distance, 40);

    n_usables = 0;
    check("nothing in reach",                  geSenseUsable(0.0f, 0.0f, 0.0f, u, 4), 0);

    printf("\n%s -- %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
