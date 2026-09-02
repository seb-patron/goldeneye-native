#ifndef GE_GIBS_H
#define GE_GIBS_H

enum GeGibCause {
    GE_GIB_CAUSE_NONE = 0,
    GE_GIB_CAUSE_EXPLOSION = 1,
    GE_GIB_CAUSE_HIT = 2
};

enum GeGibMode {
    GE_GIBS_OFF = 0,
    GE_GIBS_EXPLOSIONS = 1,
    GE_GIBS_HIGH_DAMAGE = 2,
    GE_GIBS_ALWAYS = 3
};

#define GE_GIB_HIGH_DAMAGE_THRESHOLD 4.0f

int gePortGibsMode(void);
/* Validate and replace the cached policy without rewriting GETV_GIBS. Returns 1 when `mode` is
 * accepted and 0 without changing state for an unknown value. Safe to call on the game thread
 * after startup; existing character/hit records are preserved. */
int gePortGibsSetMode(int mode);
int gePortGibsShouldSpawn(int cause, int newly_dead, float hit_damage);
void gePortGibsRecordHit(const void *character, int cause, float damage,
                         float impulse_x, float impulse_y, float impulse_z);
int gePortGibsGetLastHit(const void *character, int *cause, float *damage,
                         float *impulse_x, float *impulse_y, float *impulse_z);
int gePortGibsMarkCharacter(const void *character);
int gePortGibsIsCharacter(const void *character);
void gePortGibsForgetCharacter(const void *character);

#endif
