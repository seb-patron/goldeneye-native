/* The derived event bus.
 *
 * Nothing in the game posts these. gePortEventFrame watches the player and world APIs each frame
 * and emits the DIFFERENCES -- which is why it can report a room change on a build where nothing
 * knows what a room change is, and why every one of its bugs is a silent one. An edge detector
 * that stops detecting emits nothing, and a mod subscribed to it cannot tell that apart from a
 * quiet level.
 *
 * The star of this file is the guard hysteresis. NEAR is 700 units and CLEAR is 850, and the gap
 * exists so a guard hovering on the boundary does not emit a near/clear pair every single frame.
 * Collapse the two radii and nothing fails: you get a working bus that floods. So the band is
 * asserted directly rather than trusted.
 *
 * Every input is a stubbed function, so no game and no level are needed.
 */

#include <stdio.h>
#include <string.h>

#include "ge_event.c"

/* ---------------------------------------------------------------- the fake world */

static int   fake_stage = 1;
static int   fake_present = 1;
static float fake_px, fake_py, fake_pz;

static int   fake_world_loaded = 1;
static int   fake_wp_id = 10, fake_wp_room = 3;
static int   fake_have_wp = 1;

#define FAKE_GUARDS 4
static struct { int chrnum; float x, y, z; int alive; } fake_guard[FAKE_GUARDS];
static int fake_nguards;

int bossGetStageNum(void) { return fake_stage; }

/* The derive now feeds the contact detector, which is the fix for it having shipped with storage,
 * a query and no source -- geSenseIsStuck returned false forever and the atlas printed "moving
 * freely" for a stationary player. Recorded rather than ignored so the wiring is asserted: a stub
 * that swallows the call would let the same gap reappear silently. */
static int   contact_calls;
static int   contact_slot;
static float contact_x, contact_z;
static int   contact_commanded;
static int   fake_commanded = 1;

int gePlayerCommandedMove(int slot) { (void) slot; return fake_commanded; }

void geSenseContactUpdate(int player_slot, float x, float z, int commanded_move)
{
    contact_calls++;
    contact_slot = player_slot;
    contact_x = x;
    contact_z = z;
    contact_commanded = commanded_move;
}

int gePlayerStateGet(int slot, GePlayerState *out)
{
    if (slot != 0 || out == NULL) { return 0; }
    memset(out, 0, sizeof *out);
    if (!fake_present) { return 1; }        /* reachable slot, no player in it */
    out->present = 1;
    out->fields  = GE_ST_POSITION;
    out->x = fake_px; out->y = fake_py; out->z = fake_pz;
    return 1;
}

int geWorldLoaded(void) { return fake_world_loaded; }

int geWorldNearestWaypoint(float x, float y, float z, GeWorldWaypoint *out)
{
    (void) x; (void) y; (void) z;
    if (!fake_have_wp || out == NULL) { return 0; }
    memset(out, 0, sizeof *out);
    out->id = fake_wp_id;
    out->room = fake_wp_room;
    return 1;
}

/* Filters the fake population by the radius it is handed, which is what makes the hysteresis
 * band real: the derive calls this once at NEAR and again at CLEAR. */
int geWorldGuardsNear(float x, float y, float z, float radius, GeWorldGuard *out, int max)
{
    int i, n = 0;
    for (i = 0; i < fake_nguards && n < max; i++) {
        float dx = fake_guard[i].x - x, dy = fake_guard[i].y - y, dz = fake_guard[i].z - z;
        double d = sqrt((double)(dx * dx + dy * dy + dz * dz));
        if (!fake_guard[i].alive) { continue; }
        if (d > radius) { continue; }
        memset(&out[n], 0, sizeof out[n]);
        out[n].chrnum = fake_guard[i].chrnum;
        out[n].x = fake_guard[i].x; out[n].y = fake_guard[i].y; out[n].z = fake_guard[i].z;
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------- capture */

#define CAP_MAX 64
static struct { GeEventType t; int a, b, c; } cap[CAP_MAX];
static int cap_n;

static void on_event(GeEventType t, int a, int b, int c, void *user)
{
    (void) user;
    if (cap_n < CAP_MAX) { cap[cap_n].t = t; cap[cap_n].a = a; cap[cap_n].b = b;
                           cap[cap_n].c = c; cap_n++; }
}

static int count_of(GeEventType t)
{
    int i, n = 0;
    for (i = 0; i < cap_n; i++) { if (cap[i].t == t) { n++; } }
    return n;
}

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %-50s %d\n", what, got);
    } else {
        printf("  FAIL  %-50s got %d want %d\n", what, got, want);
        failures++;
    }
}

static void tick(void) { cap_n = 0; gePortEventFrame(0); }

int main(void)
{
    printf("derived event bus\n\n");

    geEventSubscribe(on_event, NULL);

    /* ---------------- level change and spawn ---------------- */
    fake_px = fake_py = fake_pz = 0.0f;
    fake_nguards = 0;
    tick();
    check("first frame: LEVEL_CHANGE",   count_of(GE_EV_LEVEL_CHANGE), 1);
    check("first frame: PLAYER_SPAWN",   count_of(GE_EV_PLAYER_SPAWN), 1);
    check("first frame: WAYPOINT",       count_of(GE_EV_WAYPOINT), 1);
    check("first frame: ROOM_CHANGE",    count_of(GE_EV_ROOM_CHANGE), 1);

    /* A steady state emits NOTHING. An edge detector that re-reports its state every frame is
     * indistinguishable from one that works until a mod tries to count anything. */
    tick();
    check("steady state is silent",      cap_n, 0);

    /* the contact detector is fed. It shipped with storage, a query and no source, so is_stuck
     * answered false forever and read as a measurement. This asserts the source exists, carries
     * the right slot and position, and passes the commanded flag through -- all four, because a
     * call with the wrong slot or a dropped flag would still make the linker happy. */
    contact_calls = 0;
    fake_px = 11.0f; fake_pz = 22.0f;
    fake_commanded = 1;
    tick();
    check("contact fed once per present slot", contact_calls, 1);
    check("  with the right slot",             contact_slot, 0);
    check("  and the position",                (int) contact_x, 11);
    check("  and the commanded flag",          contact_commanded, 1);

    fake_commanded = 0;
    tick();
    check("commanded flag passes through 0",   contact_commanded, 0);
    fake_commanded = 1;
    fake_px = 0.0f; fake_pz = 0.0f;
    tick();

    /* ---------------- guard enters the NEAR radius ---------------- */
    fake_nguards = 1;
    fake_guard[0].chrnum = 77; fake_guard[0].alive = 1;
    fake_guard[0].x = 500.0f; fake_guard[0].y = 0.0f; fake_guard[0].z = 0.0f;   /* inside 700 */
    tick();
    check("guard at 500: GUARD_NEAR",    count_of(GE_EV_GUARD_NEAR), 1);
    check("  reports the chrnum",        cap[0].b, 77);
    check("  and the distance",          cap[0].c, 500);

    tick();
    check("still near: no repeat",       count_of(GE_EV_GUARD_NEAR), 0);

    /* ---------------- the hysteresis band ----------------
     *
     * 780 is outside NEAR (700) and inside CLEAR (850). The guard has left the near set but has
     * not left the band, so nothing should fire. Collapse the two radii and this is where the
     * flood starts -- and a flood still looks like a working bus. */
    fake_guard[0].x = 780.0f;
    tick();
    check("in the band: no GUARD_CLEAR", count_of(GE_EV_GUARD_CLEAR), 0);
    check("in the band: nothing at all", cap_n, 0);

    /* Back inside without ever having cleared: still no new NEAR, because it never left. */
    fake_guard[0].x = 600.0f;
    tick();
    check("band -> near: no re-NEAR",    count_of(GE_EV_GUARD_NEAR), 0);

    /* ---------------- properly gone ---------------- */
    fake_guard[0].x = 900.0f;                                   /* beyond CLEAR */
    tick();
    check("beyond 850: GUARD_CLEAR",     count_of(GE_EV_GUARD_CLEAR), 1);
    check("  reports the chrnum",        cap[0].b, 77);

    /* Having cleared, coming back is a fresh NEAR. */
    fake_guard[0].x = 400.0f;
    tick();
    check("returning: GUARD_NEAR again", count_of(GE_EV_GUARD_NEAR), 1);

    /* ---------------- room and waypoint ---------------- */
    fake_wp_id = 11;
    tick();
    check("waypoint change fires",       count_of(GE_EV_WAYPOINT), 1);
    check("  same room: no ROOM_CHANGE", count_of(GE_EV_ROOM_CHANGE), 0);

    fake_wp_room = 4;
    tick();
    check("room change fires",           count_of(GE_EV_ROOM_CHANGE), 1);
    check("  carries the new room",      cap[0].b, 4);
    check("  and the old one",           cap[0].c, 3);

    /* ---------------- the player goes ---------------- */
    fake_present = 0;
    tick();
    check("player gone fires",           count_of(GE_EV_PLAYER_GONE), 1);
    check("  and nothing else",          cap_n, 1);

    fake_present = 1;
    tick();
    check("respawn fires SPAWN",         count_of(GE_EV_PLAYER_SPAWN), 1);
    /* State was reset on GONE, so the room is reported fresh rather than suppressed as unchanged.
     * A bot that missed this would think it was still in the room it died in. */
    check("  and re-reports the room",   count_of(GE_EV_ROOM_CHANGE), 1);

    /* ---------------- a new level ----------------
     *
     * Level change is handled BEFORE the per-slot comparisons precisely so the first frame of a
     * new level does not report a room change from the old level's room. */
    fake_stage = 2;
    fake_wp_room = 9;
    tick();
    check("new stage: LEVEL_CHANGE",     count_of(GE_EV_LEVEL_CHANGE), 1);
    check("  new stage in a",            cap[0].a, 2);
    check("  old stage in b",            cap[0].b, 1);

    /* ---------------- no world knowledge ----------------
     *
     * Four levels have none. Room and waypoint must simply not fire, rather than firing with
     * garbage or refusing to run the rest of the derive. */
    fake_world_loaded = 0;
    fake_stage = 3;
    tick();
    cap_n = 0;
    fake_guard[0].x = 9999.0f;
    tick();
    check("no world: no WAYPOINT",       count_of(GE_EV_WAYPOINT), 0);
    check("no world: no ROOM_CHANGE",    count_of(GE_EV_ROOM_CHANGE), 0);

    /* ---------------- unsubscribe ---------------- */
    geEventUnsubscribe(on_event, NULL);
    fake_world_loaded = 1;
    fake_stage = 4;
    tick();
    check("unsubscribed: silent",        cap_n, 0);

    printf("\n%s -- %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
