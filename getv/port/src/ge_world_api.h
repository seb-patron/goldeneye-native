/* World knowledge at runtime: what a level contains and how to get around it.
 *
 * ge_player_api.h is the seam for ACTING -- post input for a slot, read that slot's state. This
 * is the seam for KNOWING: where the objectives are, which waypoints lead to them, which guards
 * are near, which room anything is in. Bots, learning agents and tooling all need it, and none
 * of them can get it from the game state alone because the game does not carry it in a form
 * anything can query.
 *
 * The data is extracted offline (tools/gen_level_*.py) and packed by tools/pack_world.py into a
 * flat fixed-width file per level. Twenty levels come to about 80KB in total, so a level's
 * knowledge is loaded once and held for the session.
 *
 * NOTHING HERE ALLOCATES AFTER LOAD and no call parses anything. Every query is a bounds check
 * and an indexed read, so it is safe to call from a per-tick bot policy.
 */
#ifndef GE_WORLD_API_H
#define GE_WORLD_API_H

#define GE_WORLD_MAGIC   0x44574547u      /* 'GEWD' little-endian */
#define GE_WORLD_VERSION 1

/* Sentinel for "this thing has no room recorded", which is different from room 0. */
#define GE_WORLD_NO_ROOM 0xFFFF

typedef struct GeWorldWaypoint {
    int   id;
    int   room;                 /* GE_WORLD_NO_ROOM if unknown */
    float x, y, z;
} GeWorldWaypoint;

typedef struct GeWorldGuard {
    int   chrnum;
    int   room;
    float x, y, z;
} GeWorldGuard;

typedef struct GeWorldObjective {
    int   index;
    int   min_difficulty;
    int   targets;              /* tagged targets; 0 means it cannot be routed to yet */
    int   steps;                /* route steps; 0 means no route was solved */
    int   first_step;           /* index into the step table */
    float tx, ty, tz;           /* the last target's position */
} GeWorldObjective;

/* One leg of a route: walk `distance` on `heading`, having turned `turn` from the previous leg.
 * `threats` is how many guards were within range of this step when the route was solved --
 * static knowledge, not live state. Live threats come from the game; this says what is usually
 * there. */
typedef struct GeWorldStep {
    int   from, to;             /* waypoint ids */
    float distance;
    float heading;              /* degrees, atan2(dx, dz), same convention as the extractor */
    float turn;                 /* degrees from the previous step's heading; 0 on the first */
    int   threats;
} GeWorldStep;

/* Load a level's knowledge. Name is the extractor's level name ("dam", "facility", ...).
 * Returns 0 if there is no data for it, which is normal and not an error -- four levels have no
 * background model and several have no routed objectives. Callers must cope with knowing
 * nothing. */
int  geWorldLoad(const char *level);
void geWorldUnload(void);
int  geWorldLoaded(void);
const char *geWorldLevel(void);

int  geWorldWaypointCount(void);
int  geWorldGuardCount(void);
int  geWorldObjectiveCount(void);
int  geWorldStepCount(void);

int  geWorldWaypoint(int i, GeWorldWaypoint *out);
int  geWorldGuard(int i, GeWorldGuard *out);
int  geWorldObjective(int i, GeWorldObjective *out);
int  geWorldStep(int i, GeWorldStep *out);

/* The two queries a bot actually makes.
 *
 * geWorldNearestWaypoint answers "where am I on the graph" from a world position, which is what
 * turns a position into something routable. geWorldRouteStep walks an objective's route without
 * the caller doing index arithmetic against first_step. */
int  geWorldNearestWaypoint(float x, float y, float z, GeWorldWaypoint *out);
int  geWorldRouteStep(int objective, int n, GeWorldStep *out);

/* Guards within `radius` of a point, nearest first, at most `max`. Static placement, not live
 * positions -- useful for "what is usually dangerous here" rather than "who is shooting". */
int  geWorldGuardsNear(float x, float y, float z, float radius,
                       GeWorldGuard *out, int max);

#endif /* GE_WORLD_API_H */
