/* Optional enemy gib policy.
 *
 * The game owns death, rendering and particle creation. This small port-side module owns only
 * user policy so future causes can be added without scattering getenv calls through the
 * decompilation patch. The default is deliberately retail behaviour: no gibs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_gibs.h"

static int ge_gibs_mode = -1;

/* Character slots are recycled after their ordinary death cleanup. Keep the visual marker out
 * of ChrRecord so this port feature does not claim one of the decompilation's unknown flag bits.
 * The game clears a marker both when cleanup completes and before reusing a slot. */
#define GE_GIBBED_CHARACTER_CAPACITY 256
static const void *ge_gibbed_characters[GE_GIBBED_CHARACTER_CAPACITY];

int gePortGibsMode(void)
{
    const char *value;

    if (ge_gibs_mode >= 0) {
        return ge_gibs_mode;
    }

    value = getenv("GETV_GIBS");
    ge_gibs_mode = (value != NULL && strcmp(value, "1") == 0)
        ? GE_GIBS_EXPLOSIONS
        : GE_GIBS_OFF;

    if (ge_gibs_mode == GE_GIBS_EXPLOSIONS) {
        printf("[getv][gibs] fatal explosions gib enemies\n");
    }

    return ge_gibs_mode;
}

int gePortGibsShouldSpawn(int cause, int newly_dead)
{
    if (!newly_dead) {
        return 0;
    }

    return gePortGibsMode() == GE_GIBS_EXPLOSIONS && cause == GE_GIB_CAUSE_EXPLOSION;
}

int gePortGibsMarkCharacter(const void *character)
{
    int i;

    if (character == NULL) {
        return 0;
    }

    for (i = 0; i < GE_GIBBED_CHARACTER_CAPACITY; i++) {
        if (ge_gibbed_characters[i] == character) {
            return 1;
        }
        if (ge_gibbed_characters[i] == NULL) {
            ge_gibbed_characters[i] = character;
            return 1;
        }
    }

    return 0;
}

int gePortGibsIsCharacter(const void *character)
{
    int i;

    for (i = 0; character != NULL && i < GE_GIBBED_CHARACTER_CAPACITY; i++) {
        if (ge_gibbed_characters[i] == character) {
            return 1;
        }
    }

    return 0;
}

void gePortGibsForgetCharacter(const void *character)
{
    int i;

    for (i = 0; character != NULL && i < GE_GIBBED_CHARACTER_CAPACITY; i++) {
        if (ge_gibbed_characters[i] == character) {
            ge_gibbed_characters[i] = NULL;
            return;
        }
    }
}
