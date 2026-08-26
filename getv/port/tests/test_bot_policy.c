/* The route bot's threat-hold policy.
 *
 * Three lines of logic, and worth a test for one reason: the failure mode is a bot that stands
 * still forever. "Wait until the route clears" sounds prudent and deadlocks, because nothing about
 * waiting makes a guard change its mind. A frozen bot is indistinguishable from a crashed one, and
 * the last time this project produced one it cost eleven hundred frames of staring at a wall.
 *
 * So the property under test is not "does it hold" but "does it ALWAYS eventually advance".
 */

#include <stdio.h>

#include "ge_bot_route.c"

/* ge_bot_route.c reaches for the player, world and enemy APIs. Only the policy is under test, so
 * these are stubs -- but they must exist for the unit to link, and they come AFTER the include
 * because their parameter types are declared by the headers it pulls in.
 *
 * They all report "nothing here". That is the right default for a policy test: any stub that
 * returned plausible data would be inventing a world, and the policy would then be tested against
 * my guess at the game rather than against its own logic. */
unsigned long gePlayerTick(void) { return 0; }
int gePlayerPost(int slot, unsigned long tick, const GePlayerInput *in, int hold)
{ (void) slot; (void) tick; (void) in; (void) hold; return 0; }
int gePlayerStateGet(int slot, GePlayerState *out) { (void) slot; (void) out; return 0; }
int gePlayerControlType(int slot) { (void) slot; return 0; }
int gePlayerSlotIsDrivable(int slot) { (void) slot; return 1; }
int bossGetStageNum(void) { return 0; }

int geWorldLoad(const char *level) { (void) level; return 0; }
int geWorldLoaded(void) { return 0; }
const char *geWorldLevel(void) { return "none"; }
int geWorldObjectiveCount(void) { return 0; }
int geWorldObjective(int i, GeWorldObjective *out) { (void) i; (void) out; return 0; }
int geWorldRouteStep(int obj, int n, GeWorldStep *out) { (void) obj; (void) n; (void) out; return 0; }
int geWorldWaypointById(int id, GeWorldWaypoint *out) { (void) id; (void) out; return 0; }

int geEnemyThreatAt(float x, float y, float z, float radius)
{ (void) x; (void) y; (void) z; (void) radius; return 0; }

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
    int held;

    printf("route bot threat-hold policy\n\n");

    /* An uncontested waypoint is walked to immediately, whatever the history. */
    check("no threat, never held -> advance",   ge_br_should_advance(0, 0), 1);
    check("no threat, long held  -> advance",   ge_br_should_advance(0, 999), 1);
    check("negative threat treated as none",    ge_br_should_advance(-1, 0), 1);

    /* A contested one is waited on -- but only for a while. */
    check("threat, just arrived  -> hold",      ge_br_should_advance(1, 0), 0);
    check("threat, half patience -> hold",      ge_br_should_advance(1, GE_BR_MAX_HOLD / 2), 0);
    check("threat, one shy       -> hold",      ge_br_should_advance(3, GE_BR_MAX_HOLD - 1), 0);

    /* The property that matters. At the cap it commits, and it stays committed however long the
     * contest lasts and however many enemies are converging. */
    check("threat, at cap        -> advance",   ge_br_should_advance(1, GE_BR_MAX_HOLD), 1);
    check("threat, past cap      -> advance",   ge_br_should_advance(9, GE_BR_MAX_HOLD + 500), 1);

    /* Simulated: hold under permanent threat and confirm it breaks out, rather than trusting the
     * two boundary assertions above to imply it. A loop that never terminates is exactly the bug
     * this file exists to prevent, so the loop is bounded and the bound is the assertion. */
    held = 0;
    while (held < GE_BR_MAX_HOLD * 4 && !ge_br_should_advance(2, held)) { held++; }
    check("permanent threat still advances",    held < GE_BR_MAX_HOLD * 4, 1);
    check("and does so exactly at the cap",     held, GE_BR_MAX_HOLD);

    /* The cap is three seconds at 60Hz. If someone retunes it, this says so out loud rather than
     * letting a bot quietly acquire a thirty-second stare. */
    check("cap is a sane number of seconds",    (GE_BR_MAX_HOLD / 60) <= 5, 1);

    printf("\n%s -- %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
