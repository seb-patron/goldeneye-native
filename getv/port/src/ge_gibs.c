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

struct GeGibHitRecord {
    const void *character;
    int cause;
    float damage;
    float impulse_x;
    float impulse_y;
    float impulse_z;
};

static struct GeGibHitRecord ge_gib_hit_records[GE_GIBBED_CHARACTER_CAPACITY];

int gePortGibsMode(void)
{
    const char *value;

    if (ge_gibs_mode >= 0) {
        return ge_gibs_mode;
    }

    value = getenv("GETV_GIBS");
    ge_gibs_mode = GE_GIBS_OFF;

    if (value != NULL) {
        if (strcmp(value, "1") == 0 || strcmp(value, "on") == 0 ||
            strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
            strcmp(value, "explosion") == 0 || strcmp(value, "explosions") == 0) {
            ge_gibs_mode = GE_GIBS_EXPLOSIONS;
        } else if (strcmp(value, "high_damage") == 0 ||
                   strcmp(value, "high-damage") == 0 ||
                   strcmp(value, "highdamage") == 0) {
            ge_gibs_mode = GE_GIBS_HIGH_DAMAGE;
        } else if (strcmp(value, "always") == 0) {
            ge_gibs_mode = GE_GIBS_ALWAYS;
        }
    }

    if (ge_gibs_mode == GE_GIBS_EXPLOSIONS) {
        printf("[getv][gibs] policy=explosions, persistent physics chunks enabled\n");
    } else if (ge_gibs_mode == GE_GIBS_HIGH_DAMAGE) {
        printf("[getv][gibs] policy=high_damage (threshold %.1f), persistent physics chunks enabled\n",
               (double)GE_GIB_HIGH_DAMAGE_THRESHOLD);
    } else if (ge_gibs_mode == GE_GIBS_ALWAYS) {
        printf("[getv][gibs] policy=always, persistent physics chunks enabled\n");
    }

    return ge_gibs_mode;
}

int gePortGibsShouldSpawn(int cause, int newly_dead, float hit_damage)
{
    int mode;

    if (!newly_dead) {
        return 0;
    }

    mode = gePortGibsMode();
    if (mode == GE_GIBS_ALWAYS) {
        return 1;
    }
    if (mode == GE_GIBS_EXPLOSIONS) {
        return cause == GE_GIB_CAUSE_EXPLOSION;
    }
    if (mode == GE_GIBS_HIGH_DAMAGE) {
        return hit_damage >= GE_GIB_HIGH_DAMAGE_THRESHOLD;
    }
    return 0;
}

void gePortGibsRecordHit(const void *character, int cause, float damage,
                         float impulse_x, float impulse_y, float impulse_z)
{
    int i;
    int free_slot = -1;

    if (character == NULL) {
        return;
    }

    for (i = 0; i < GE_GIBBED_CHARACTER_CAPACITY; i++) {
        if (ge_gib_hit_records[i].character == character) {
            free_slot = i;
            break;
        }
        if (free_slot < 0 && ge_gib_hit_records[i].character == NULL) {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        ge_gib_hit_records[free_slot].character = character;
        ge_gib_hit_records[free_slot].cause = cause;
        ge_gib_hit_records[free_slot].damage = damage;
        ge_gib_hit_records[free_slot].impulse_x = impulse_x;
        ge_gib_hit_records[free_slot].impulse_y = impulse_y;
        ge_gib_hit_records[free_slot].impulse_z = impulse_z;
    }
}

int gePortGibsGetLastHit(const void *character, int *cause, float *damage,
                         float *impulse_x, float *impulse_y, float *impulse_z)
{
    int i;

    for (i = 0; character != NULL && i < GE_GIBBED_CHARACTER_CAPACITY; i++) {
        if (ge_gib_hit_records[i].character == character) {
            if (cause != NULL) *cause = ge_gib_hit_records[i].cause;
            if (damage != NULL) *damage = ge_gib_hit_records[i].damage;
            if (impulse_x != NULL) *impulse_x = ge_gib_hit_records[i].impulse_x;
            if (impulse_y != NULL) *impulse_y = ge_gib_hit_records[i].impulse_y;
            if (impulse_z != NULL) *impulse_z = ge_gib_hit_records[i].impulse_z;
            return 1;
        }
    }
    return 0;
}

int gePortGibsMarkCharacter(const void *character)
{
    int i;
    int free_slot = -1;

    if (character == NULL) {
        return 0;
    }

    for (i = 0; i < GE_GIBBED_CHARACTER_CAPACITY; i++) {
        if (ge_gibbed_characters[i] == character) {
            return 0;
        }
        if (free_slot < 0 && ge_gibbed_characters[i] == NULL) {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        ge_gibbed_characters[free_slot] = character;
        return 1;
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
        }
        if (ge_gib_hit_records[i].character == character) {
            ge_gib_hit_records[i].character = NULL;
        }
    }
}
