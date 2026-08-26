/* Exercises ge_enemy_api against a fake source.
 *
 * The whole reason the source is installed rather than linked is that it makes this file possible:
 * a fake enemy population, on a machine with no game running, testing the parts a bot's decisions
 * actually rest on -- the health inversion, the nearest-first ordering, and the difference between
 * "enemies near here" and "enemies who think their target is here".
 *
 * That last one is the API's reason to exist, so it is the one with the most assertions.
 */

#include <stdio.h>
#include <string.h>

#include "ge_enemy_api.c"

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %-46s %d\n", what, got);
    } else {
        printf("  FAIL  %-46s got %d want %d\n", what, got, want);
        failures++;
    }
}

static void checkf(const char *what, float got, float want)
{
    float d = got - want;
    if (d < 0) { d = -d; }
    if (d < 0.01f) {
        printf("  ok    %-46s %.1f\n", what, (double) got);
    } else {
        printf("  FAIL  %-46s got %.2f want %.2f\n", what, (double) got, (double) want);
        failures++;
    }
}

/* ---------------------------------------------------------------- the fake population */

struct FakeChr {
    float x, z;
    float damage, maxdamage;
    float alertness;
    float bx, bz;        /* believed target position */
    int   id;
    int   alive;
    int   mask;          /* what this row claims to provide */
};

static struct FakeChr fake[8];
static int            fake_n;

static int fake_count(void) { return fake_n; }

static int fake_at(int index, float *out, int count)
{
    struct FakeChr *c;

    if (index < 0 || index >= fake_n)   { return 0; }
    if (count < GE_ENEMY_FIELD_COUNT)   { return 0; }   /* refuse, never overrun */

    c = &fake[index];
    if (c->mask == 0) { return 0; }                     /* empty slot */

    out[GE_ENEMY_F_X]         = c->x;
    out[GE_ENEMY_F_Y]         = 0.0f;
    out[GE_ENEMY_F_Z]         = c->z;
    out[GE_ENEMY_F_DAMAGE]    = c->damage;
    out[GE_ENEMY_F_MAXDAMAGE] = c->maxdamage;
    out[GE_ENEMY_F_ALERTNESS] = c->alertness;
    out[GE_ENEMY_F_HEARING]   = 1.0f;
    out[GE_ENEMY_F_BELIEF_X]  = c->bx;
    out[GE_ENEMY_F_BELIEF_Y]  = 0.0f;
    out[GE_ENEMY_F_BELIEF_Z]  = c->bz;
    out[GE_ENEMY_F_SAW_AGO]   = 30.0f;
    out[GE_ENEMY_F_HEARD_AGO] = 90.0f;
    out[GE_ENEMY_F_ID]        = (float) c->id;
    out[GE_ENEMY_F_ALIVE]     = c->alive ? 1.0f : 0.0f;
    return c->mask;
}

#define ALL (GE_EN_POSITION | GE_EN_HEALTH | GE_EN_ALERT | GE_EN_BELIEF)

static void add(float x, float z, float dmg, float maxdmg, int id, int alive,
                float bx, float bz, int mask)
{
    struct FakeChr *c = &fake[fake_n++];
    memset(c, 0, sizeof(*c));
    c->x = x; c->z = z;
    c->damage = dmg; c->maxdamage = maxdmg;
    c->id = id; c->alive = alive;
    c->bx = bx; c->bz = bz;
    c->alertness = 100.0f;
    c->mask = mask;
}

int main(void)
{
    GeEnemy e, near[4];
    int n;

    printf("ge_enemy_api against a fake source\n\n");

    /* With NO source installed, everything must be safe and report an empty world. A bot written
     * against this API has to run unchanged on a build whose game-side shim has not landed. */
    check("no source: installed",     geEnemySourceInstalled(), 0);
    check("no source: count",         geEnemyCount(), 0);
    check("no source: geEnemy",       geEnemy(0, &e), 0);
    check("no source: byId",          geEnemyById(7, &e), 0);
    check("no source: near",          geEnemiesNear(0, 0, 0, 1000, near, 4), 0);
    check("no source: threatAt",      geEnemyThreatAt(0, 0, 0, 1000), 0);

    /* A half-installed source must be refused outright rather than reporting a count it cannot
     * describe. */
    geEnemySourceInstall(fake_count, NULL);
    check("half install refused",     geEnemySourceInstalled(), 0);

    /*        x      z    dmg  maxdmg  id  alive   belief      mask   */
    add(   100.0f,  0.0f,  20.0f, 100.0f,  11, 1,   0.0f,   0.0f, ALL);  /* near, believes origin */
    add(   300.0f,  0.0f,  90.0f, 100.0f,  12, 1, 900.0f, 900.0f, ALL);  /* hurt, looking away   */
    add(    50.0f,  0.0f,   0.0f, 100.0f,  13, 0,   0.0f,   0.0f, ALL);  /* DEAD, closest        */
    add(   200.0f,  0.0f,  10.0f, 100.0f,  14, 1,   0.0f,   0.0f, ALL);  /* believes origin      */
    add(  9000.0f,  0.0f,   0.0f, 100.0f,  15, 1,   0.0f,   0.0f, ALL);  /* far away, believes origin */
    add(   150.0f,  0.0f,   0.0f, 100.0f,  16, 1,   0.0f,   0.0f, GE_EN_POSITION); /* position only */

    geEnemySourceInstall(fake_count, fake_at);
    check("installed",                geEnemySourceInstalled(), 1);
    check("count",                    geEnemyCount(), 6);

    /* Health IS inverted AT the boundary. The game stores damage taken; a caller asking how hurt
     * a guard is must not have to know that, and getting it backwards reads a dying guard as
     * healthy. */
    check("byId(12) found",           geEnemyById(12, &e), 1);
    checkf("byId(12) health",         e.health, 10.0f);
    checkf("byId(12) max_health",     e.max_health, 100.0f);
    check("byId(99) absent",          geEnemyById(99, &e), 0);

    /* Partial data stays partial. Slot 5 provides position only, so health must be reported as
     * ABSENT rather than as zero -- a guard whose health cannot be read is not a dead guard. */
    check("byId(16) found",           geEnemyById(16, &e), 1);
    check("byId(16) has position",    (e.fields & GE_EN_POSITION) != 0, 1);
    check("byId(16) health absent",   (e.fields & GE_EN_HEALTH) != 0, 0);

    /* Nearest first, dead excluded, radius respected. The dead one at 50 is the closest of all and
     * must not appear; every caller would otherwise have to filter corpses and exactly one would
     * forget. */
    n = geEnemiesNear(0.0f, 0.0f, 0.0f, 1000.0f, near, 4);
    check("near: count within 1000",  n, 4);
    check("near[0] is id 11 @100",    near[0].id, 11);
    check("near[1] is id 16 @150",    near[1].id, 16);
    check("near[2] is id 14 @200",    near[2].id, 14);
    check("near[3] is id 12 @300",    near[3].id, 12);
    checkf("near[0] distance",        near[0].distance, 100.0f);
    check("near: dead 13 excluded",   near[0].id != 13 && near[1].id != 13 &&
                                      near[2].id != 13 && near[3].id != 13, 1);

    /* The far one at 9000 must be outside a 1000 radius. */
    n = geEnemiesNear(0.0f, 0.0f, 0.0f, 120.0f, near, 4);
    check("near: tight radius",       n, 1);
    check("near: tight is id 11",     near[0].id, 11);

    /* max is a hard cap and must keep the NEAREST, not the first found. */
    n = geEnemiesNear(0.0f, 0.0f, 0.0f, 1000.0f, near, 2);
    check("near: max=2 count",        n, 2);
    check("near: max=2 keeps closest", near[0].id, 11);
    check("near: max=2 second",       near[1].id, 16);

    /* Threat IS about belief, not proximity. This is the whole point of the API.
     *
     * The origin has NO living enemy standing on it, but three living enemies believe their target
     * is there: 11, 14, and 15 -- 15 being 9000 units away, which is precisely the guard that is
     * about to arrive and the one a proximity query would miss. Meanwhile (900,900) has nobody
     * near it and exactly one enemy converging on it.
     *
     * NOT counted, and both exclusions matter:
     *   13 believes the origin too, but is dead.
     *   16 is alive and believes nothing -- it reports GE_EN_POSITION only. An enemy whose belief
     *      cannot be read must not be counted as holding one. This assertion said 4 when it was
     *      written, because I counted 16 among the believers; the implementation was right and the
     *      expectation was wrong, which is the correct way round for a test to fail. */
    check("threat at origin",         geEnemyThreatAt(0.0f, 0.0f, 0.0f, 50.0f), 3);
    check("threat at (900,900)",      geEnemyThreatAt(900.0f, 0.0f, 900.0f, 50.0f), 1);
    check("threat somewhere quiet",   geEnemyThreatAt(-5000.0f, 0.0f, -5000.0f, 50.0f), 0);

    /* And the contrast that makes it worth having: only one living enemy is actually NEAR the
     * origin within 120 units, but three are converging on it. A bot choosing a waypoint on
     * proximity alone would call the origin safe. */
    n = geEnemiesNear(0.0f, 0.0f, 0.0f, 120.0f, near, 4);
    check("proximity says 1, belief says 3", n, 1);

    /* Uninstall must take effect -- a source outliving its level hands out positions of
     * characters that no longer exist, and they look perfectly plausible. */
    geEnemySourceInstall(NULL, NULL);
    check("uninstalled: count",       geEnemyCount(), 0);
    check("uninstalled: threatAt",    geEnemyThreatAt(0, 0, 0, 1000), 0);

    printf("\n%s -- %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
