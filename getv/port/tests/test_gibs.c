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
    }
}

int main(void)
{
    printf("test_gibs\n");

    set_mode(NULL);
    check("retail default is off", gePortGibsMode(), GE_GIBS_OFF);
    check("default explosion stays intact",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_EXPLOSION, 1), 0);

    set_mode("1");
    check("explosion mode resolves", gePortGibsMode(), GE_GIBS_EXPLOSIONS);
    check("new fatal explosion gibs",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_EXPLOSION, 1), 1);
    check("nonfatal explosion does not gib",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_EXPLOSION, 0), 0);
    check("bullet death is not in the initial scope",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_BULLET, 1), 0);
    check("unknown cause does not gib",
          gePortGibsShouldSpawn(GE_GIB_CAUSE_NONE, 1), 0);

    reset_characters();
    check("null character is refused", gePortGibsMarkCharacter(NULL), 0);
    check("new gibbed character is tracked", gePortGibsMarkCharacter((void *)1), 1);
    check("gibbed character stays hidden", gePortGibsIsCharacter((void *)1), 1);
    check("duplicate marker is idempotent", gePortGibsMarkCharacter((void *)1), 1);
    gePortGibsForgetCharacter((void *)1);
    check("cleaned character is forgotten", gePortGibsIsCharacter((void *)1), 0);

    set_mode("2");
    check("unimplemented raw mode fails closed", gePortGibsMode(), GE_GIBS_OFF);

    set_mode("1junk");
    check("malformed raw mode fails closed", gePortGibsMode(), GE_GIBS_OFF);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
