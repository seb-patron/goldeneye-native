#ifndef GE_WALLS_H
#define GE_WALLS_H

/* The level's walls, derived from the floor mesh and loaded as data. See ge_walls.c. */

int  geWallsLoad(const char *level);
void geWallsUnload(void);
int  geWallsCount(void);

/* Does the straight line from (x0,z0) to (x1,z1) leave the walkable mesh? Runtime space. */
int  geWallsBlocked(float x0, float z0, float x1, float z1);

/* The same question for a body of the given half-width rather than a line. */
int  geWallsBlockedForBody(float x0, float z0, float x1, float z1, float halfwidth);

#endif
