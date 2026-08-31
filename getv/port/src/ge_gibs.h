#ifndef GE_GIBS_H
#define GE_GIBS_H

enum GeGibCause {
    GE_GIB_CAUSE_NONE = 0,
    GE_GIB_CAUSE_EXPLOSION = 1,
    GE_GIB_CAUSE_BULLET = 2
};

enum GeGibMode {
    GE_GIBS_OFF = 0,
    GE_GIBS_EXPLOSIONS = 1
};

int gePortGibsMode(void);
int gePortGibsShouldSpawn(int cause, int newly_dead);
int gePortGibsMarkCharacter(const void *character);
int gePortGibsIsCharacter(const void *character);
void gePortGibsForgetCharacter(const void *character);

#endif
