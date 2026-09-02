/* Gib policy is tested without a ROM or renderer. The game-side hook calls this only at the
 * transition to ACT_DIE; keeping that one-shot decision here prevents a later per-frame death
 * observer from emitting the same effect repeatedly. */

#include <stdio.h>
#include <stdlib.h>

#include "ge_gibs.c"

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: got %d want %d\n", what, got, want);
        failures++;
    }
}

static void set_mode(const char *value)
{
    if (value == NULL) {
        unsetenv("GETV_GIBS");
    } else {
        setenv("GETV_GIBS", value, 1);
    }
    ge_gibs_mode = -1;
}

static void reset_characters(void)
{
    int i;

    for (i = 0; i < GE_GIBBED_CHARACTER_CAPACITY; i++) {
        ge_gibbed_characters[i] = NULL;
        ge_gib_hit_records[i].character = NULL;
    }
}

int main(void)
{
    printf("test_gibs\n");

    set_mode(NULL);
    check("retail default is off", gePortGibsMode(), GE_GIBS_OFF);
    check("default explosion stays intact",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_EXPLOSION, 1, 4.0f), 0);

    set_mode("1");
    check("explosion mode resolves", gePortGibsMode(), GE_GIBS_EXPLOSIONS);
    check("new fatal explosion gibs",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_EXPLOSION, 1, 1.0f), 1);
    check("nonfatal explosion does not gib",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_EXPLOSION, 0, 100.0f), 0);
    check("ordinary hit death stays intact in explosion policy",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_HIT, 1, 100.0f), 0);
    check("unknown cause does not gib",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_NONE, 1, 0.0f), 0);

    set_mode("high_damage");
    check("high damage mode resolves", gePortGibsMode(), GE_GIBS_HIGH_DAMAGE);
    check("damage below threshold stays intact",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_HIT, 1, 3.99f), 0);
    check("damage at threshold gibs",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_HIT, 1, 4.0f), 1);
    check("large explosion gibs in high damage mode",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_EXPLOSION, 1, 4.0f), 1);

    set_mode("always");
    check("always mode resolves", gePortGibsMode(), GE_GIBS_ALWAYS);
    check("scripted death gibs in always mode",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_NONE, 1, 0.0f), 1);
    check("always still requires a new death",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_NONE, 0, 100.0f), 0);

    set_mode("explosions");
    check("runtime setter accepts always", gePortGibsSetMode(GE_GIBS_ALWAYS), 1);
    check("runtime setter replaces cached environment policy",
          gePortGibsMode(), GE_GIBS_ALWAYS);
    setenv("GETV_GIBS", "off", 1);
    check("environment cannot overwrite a runtime policy", gePortGibsMode(), GE_GIBS_ALWAYS);
    check("runtime setter rejects values below the enum", gePortGibsSetMode(-1), 0);
    check("rejected lower value leaves policy unchanged", gePortGibsMode(), GE_GIBS_ALWAYS);
    check("runtime setter rejects values above the enum", gePortGibsSetMode(4), 0);
    check("rejected upper value leaves policy unchanged", gePortGibsMode(), GE_GIBS_ALWAYS);
    check("runtime setter can restore retail policy", gePortGibsSetMode(GE_GIBS_OFF), 1);
    check("retail policy is active immediately", gePortGibsMode(), GE_GIBS_OFF);

    reset_characters();
    check("null character is refused", gePortGibsMarkCharacter(NULL), 0);
    check("new gibbed character is tracked", gePortGibsMarkCharacter((void *)1), 1);
    check("gibbed character stays hidden", gePortGibsIsCharacter((void *)1), 1);
    check("duplicate marker does not emit twice", gePortGibsMarkCharacter((void *)1), 0);
    check("second character is tracked", gePortGibsMarkCharacter((void *)2), 1);
    gePortGibsForgetCharacter((void *)1);
    check("a registry gap cannot duplicate a later marker",
          gePortGibsMarkCharacter((void *)2), 0);
    check("forgotten character can be tracked again", gePortGibsMarkCharacter((void *)1), 1);

    gePortGibsRecordHit((void *)1, GE_GIB_CAUSE_HIT, 6.0f, 1.0f, 2.0f, 3.0f);
    check("policy changes preserve character markers", gePortGibsSetMode(GE_GIBS_EXPLOSIONS), 1);
    check("character marker survives a policy change", gePortGibsIsCharacter((void *)1), 1);
    {
        int cause = 0;
        float damage = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        check("last hit is found",
              gePortGibsGetLastHit((void *)1, &cause, &damage, &x, &y, &z), 1);
        check("last hit cause is retained", cause, GE_GIB_CAUSE_HIT);
        check("last hit damage is retained", damage == 6.0f, 1);
        check("last hit impulse is retained", x == 1.0f && y == 2.0f && z == 3.0f, 1);
    }
    gePortGibsForgetCharacter((void *)1);
    check("cleaned character is forgotten", gePortGibsIsCharacter((void *)1), 0);
    check("cleaned character loses hit context",
          gePortGibsGetLastHit((void *)1, NULL, NULL, NULL, NULL, NULL), 0);

    set_mode("2");
    check("raw numeric future mode fails closed", gePortGibsMode(), GE_GIBS_OFF);

    set_mode("1junk");
    check("malformed raw mode fails closed", gePortGibsMode(), GE_GIBS_OFF);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
