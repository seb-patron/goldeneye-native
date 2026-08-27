/* Does the arbiter reproduce the behaviour the measurements ask for?
 *
 * The policy was derived from three recorded runs, so the test encodes those runs as cases rather
 * than testing the code against itself. Each case names the measurement it comes from.
 *
 * Testing offline instead of by watching a bot is the point. A bot that reaches waypoint 4 has
 * been steered by the route, the arbiter, the sensor and the engine's own movement, and watching
 * it cannot say which of them lost the six waypoints. This isolates the decision.
 */
#include <stdio.h>
#include <math.h>

#include "../src/ge_bot_arbiter.h"
#include "../src/ge_bot_arbiter.c"

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
    else       { printf("  ok  : %s\n", what); }
}

static GeBotSituation base(void)
{
    GeBotSituation s;
    s.route_heading = 90.0f;
    s.has_target = 0;
    s.target_bearing = 0.0f;
    s.target_lateral = 0.0f;
    s.distance = 1000.0f;
    s.health = 1.0f;
    s.has_los = 1;             /* clear by default; only the LOS cases below turn it off */
    return s;
}

int main(void)
{
    GeBotSituation s;
    GeBotAction a, b;
    int i;

    printf("routing alone: waypoint 10, alive -- the baseline must be unchanged\n");
    s = base();
    a = geBotArbitrate(&s);
    check(a.heading == s.route_heading, "with no target the heading is the route's");
    check(a.advance == 1 && a.fire == 0, "and it advances without firing");

    printf("\nthe fix: a target inside the auto-aim cone costs nothing\n");
    s = base();
    s.has_target = 1;
    /* 10 degrees, inside the derived 15-degree cone (GE_ARB_FIRE_CONE). This case used
     * to read 20, correct against the FIRST unsourced estimate of the cone and silently
     * wrong against the corrected one -- caught by running the suite after fixing the
     * cone, not by inspection. */
    s.target_bearing = 90.0f + 10.0f;
    s.distance = 900.0f;
    a = geBotArbitrate(&s);
    check(a.heading == s.route_heading, "the heading is STILL the route's -- no turn is spent");
    check(a.fire == 1, "and it fires, because auto-aim closes the last 10 degrees");
    check(a.advance == 1, "and it keeps advancing, so no waypoints are lost");
    check(a.reason == GE_ARB_FIRING_ON_ROUTE, "reported as firing on route");

    printf("\noutside the cone: not worth a turn\n");
    s = base();
    s.has_target = 1;
    s.target_bearing = 90.0f + 80.0f;
    s.distance = 900.0f;
    a = geBotArbitrate(&s);
    check(a.heading == s.route_heading, "still no turn");
    check(a.fire == 0, "and no shot, since auto-aim cannot close 80 degrees");

    printf("\nfiring on the move died: never advance on an aim-owned heading\n");
    s = base();
    s.has_target = 1;
    s.target_bearing = 200.0f;
    s.distance = 150.0f;                  /* inside panic range */
    a = geBotArbitrate(&s);
    check(a.advance == 0, "cornered: it holds instead of walking into the guard");
    check(a.fire == 1, "but it does fight back");
    check(a.reason == GE_ARB_CORNERED, "reported as cornered");
    check(a.retreat == 0, "full health: holds and fights, does not back away");

    printf("\nthe recorded health fall 1.00 .88 .69 .39 .11 dead\n");
    s = base();
    s.has_target = 1;
    s.target_bearing = 90.0f + 10.0f;
    s.distance = 2000.0f;                 /* far away: only health should trigger this */
    s.health = 0.30f;
    a = geBotArbitrate(&s);
    check(a.reason == GE_ARB_SURVIVING, "below 0.40 health survival outranks progress");
    check(a.advance == 0, "and it stops spending health on waypoints");
    check(a.retreat == 1, "measured live: standing still still lost 1.00 -> dead, so it backs away too");

    s.health = 0.80f;
    a = geBotArbitrate(&s);
    check(a.reason == GE_ARB_FIRING_ON_ROUTE, "healthy at the same bearing, it presses on");
    check(a.retreat == 0, "and does not back away from something it is not losing to");

    printf("\nthe seventh latch: a target dead behind must not oscillate\n");

    /* -180 is where the wrapped bearing error flips sign on the smallest change. Seventeen
     * consecutive frames at -180 froze the heading last time. Walk the target through it. */
    s = base();
    s.has_target = 1;
    s.distance = 150.0f;                  /* the branch that DOES turn, so the risk is real */
    s.target_lateral = -1.0f;             /* consistently a hair to the left, frame after frame */
    s.target_bearing = 90.0f + 179.6f;
    a = geBotArbitrate(&s);
    for (i = 1; i <= 12; i++) {
        s.target_bearing = 90.0f + 179.6f + (float) i * 0.05f;   /* creeps through 180 */
        b = geBotArbitrate(&s);
        if ((b.heading > s.route_heading) != (a.heading > s.route_heading)) {
            printf("  FAIL: turn direction flipped at step %d (%.2f -> %.2f)\n",
                   i, a.heading, b.heading);
            fails++;
            break;
        }
    }
    if (i > 12) {
        printf("  ok  : 12 steps across the 180 boundary kept one turn direction\n");
    }

    /* And the ordinary routing path never turns at all, so it cannot have the bug. */
    s = base();
    s.has_target = 1;
    s.distance = 2000.0f;
    s.target_bearing = 90.0f - 180.0f;
    a = geBotArbitrate(&s);
    check(a.heading == s.route_heading,
          "at range, a target dead behind leaves the heading untouched");

    printf("\nno confirmed line of sight: the trigger is gated, the defensive turn is not\n");
    s = base();
    s.has_target = 1;
    s.target_bearing = 90.0f + 5.0f;      /* well inside the cone */
    s.distance = 900.0f;
    s.has_los = 0;
    a = geBotArbitrate(&s);
    check(a.fire == 0, "in the cone but LOS unconfirmed: no shot");
    check(a.heading == s.route_heading, "and still no turn spent, since routing never turns");

    s = base();
    s.has_target = 1;
    s.distance = 150.0f;                  /* panic range: this branch DOES turn */
    s.target_lateral = 1.0f;
    s.target_bearing = 90.0f + 90.0f;
    s.has_los = 0;
    a = geBotArbitrate(&s);
    check(a.fire == 0, "cornered without LOS: still no shot fired blind");
    check(a.heading != s.route_heading, "but the defensive turn still happens");

    printf("\nthe corrected cone: 15 degrees from the screen-space acceptance box\n");
    s = base();
    s.has_target = 1;
    s.target_bearing = 90.0f + 14.0f;     /* inside 15 */
    s.distance = 900.0f;
    a = geBotArbitrate(&s);
    check(a.fire == 1, "14 degrees off: inside the derived cone, fires");

    s.target_bearing = 90.0f + 16.0f;     /* outside 15 */
    a = geBotArbitrate(&s);
    check(a.fire == 0, "16 degrees off: outside the derived cone, holds fire");

    printf("\n%s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
