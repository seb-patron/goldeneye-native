/* ge_enemy_api -- LIVE enemy state, as opposed to where enemies were placed.
 *
 * ge_world_api answers "what does this level contain": objectives, waypoints, routes, and guard
 * SPAWN POINTS from the extraction. That is static knowledge, true before the level starts and
 * unchanged by anything that happens in it. A bot steering by it alone is navigating a map of a
 * room it is not looking at.
 *
 * This is the other half: who is actually here right now, how hurt they are, whether they have
 * Noticed anyone, and -- the part that matters most for deciding to retreat -- where they think
 * THEIR TARGET IS. The game already tracks all of it per character; none of it was reachable from
 * a bot or a mod.
 *
 * Why the belief fields are the interesting ones
 *
 * `believed_*` is the enemy's own record of where it last knew its target to be, and `saw_*_ago`
 * says how stale that is. The gap between what an enemy believes and what is true is the entire
 * basis for deciding whether to break contact:
 *
 *   - belief close to your real position, seen recently  -> you are being tracked; retreating in a
 *     straight line away from the enemy is the worst option, because that is where it is aiming
 *   - belief far from you, or badly stale                -> you have broken contact and any move
 *     that does not re-enter its view keeps it broken
 *   - several enemies believing the same wrong place     -> that place is where the fight is; it
 *     is a location to avoid, not an enemy to fight
 *
 * A bot that only knows enemy POSITIONS can fight. A bot that knows what enemies believe can
 * disengage, flank, and bait -- and those are the behaviours that read as intelligent.
 *
 * The source IS installed, not linked
 *
 * The live data lives in ChrRecord (bondtypes.h:2454-2591), and the port layer is compiled
 * without the decomp's include path, so it cannot name that type. The established bridge in this
 * codebase is a flat accessor implemented game-side -- gePortPlayerPos is exactly that.
 *
 * Rather than declare an extern the port cannot satisfy (which would turn a missing shim into a
 * link failure and break the build for everyone), the source is REGISTERED at boot, the way
 * joySetPlaybackFunc registers input playback. With nothing registered every query here reports
 * zero enemies and geEnemySourceInstalled() returns 0, so the absence is a readable runtime state
 * instead of a build error.
 */
#ifndef GE_ENEMY_API_H
#define GE_ENEMY_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- the flat wire layout
 *
 * One float array per enemy, because that is the shape gePortPlayerPos established and because a
 * shared struct would need a header both the port and the decomp can include -- which is the
 * coupling this layout exists to avoid.
 *
 * The count is passed with the array and checked, so adding a field later cannot silently shift
 * every reader by one slot. That failure mode is quiet and total: every field reads as its
 * neighbour and nothing errors.
 */
#define GE_ENEMY_F_X          0   /* prop->pos.x                                          */
#define GE_ENEMY_F_Y          1
#define GE_ENEMY_F_Z          2
#define GE_ENEMY_F_DAMAGE     3   /* chr->damage -- accumulated, NOT remaining health   */
#define GE_ENEMY_F_MAXDAMAGE  4   /* chr->maxdamage -- what it takes to kill them          */
#define GE_ENEMY_F_ALERTNESS  5   /* chr->alertness, 0..255                                */
#define GE_ENEMY_F_HEARING    6   /* chr->hearingscale -- rises when shot at               */
#define GE_ENEMY_F_BELIEF_X   7   /* chr->lastknowntargetpos                               */
#define GE_ENEMY_F_BELIEF_Y   8
#define GE_ENEMY_F_BELIEF_Z   9
#define GE_ENEMY_F_SAW_AGO   10   /* chr->lastseetarget60                                  */
#define GE_ENEMY_F_HEARD_AGO 11   /* chr->lastheartarget60                                 */
#define GE_ENEMY_F_ID        12   /* chr->chrnum                                           */
#define GE_ENEMY_F_ALIVE     13   /* chr->actiontype != ACT_DEAD                           */
#define GE_ENEMY_FIELD_COUNT 14

/* Which fields the source actually filled. Absent is not zero: an enemy at full health and an
 * enemy whose health could not be read are different facts, and a bot that treats the second as
 * the first will walk into it. Same reasoning as GePlayerState.fields. */
#define GE_EN_POSITION  (1u << 0)
#define GE_EN_HEALTH    (1u << 1)
#define GE_EN_ALERT     (1u << 2)
#define GE_EN_BELIEF    (1u << 3)

typedef struct GeEnemy {
    int      id;              /* chrnum; stable for the life of the character              */
    unsigned fields;          /* GE_EN_* -- test before reading anything below             */

    float    x, y, z;

    /* Remaining health, already converted from the game's accumulated-damage form. A caller
     * asking "how hurt is this guard" should not have to know the field is stored inverted. */
    float    health;
    float    max_health;
    int      alive;

    int      alertness;       /* 0..255                                                     */
    float    hearing_scale;   /* rises when shot at, so it doubles as "recently under fire" */

    /* Where this enemy believes its target is, and how stale that belief is. See the header
     * comment -- this is the retreat signal. */
    float    believed_x, believed_y, believed_z;
    int      saw_target_ago;   /* frames at 60Hz; larger means longer since it had eyes on  */
    int      heard_target_ago;

    /* Filled by geEnemiesNear only; zero elsewhere. Horizontal distance, matching the rest of
     * the navigation layer, which treats the world as a floor plan. */
    float    distance;
} GeEnemy;

/* ---------------------------------------------------------------- the source seam */

/* How many character slots exist. Slots may be empty; this is the loop bound, not a population. */
typedef int (*GeEnemyCountFn)(void);

/* Fill `out` (GE_ENEMY_FIELD_COUNT floats) for one slot.
 *
 * RETURNS A GE_EN_* MASK of the fields it actually wrote, or 0 for an empty or invalid slot. It
 * returns a mask rather than a bool so that partial data stays distinguishable from zero data --
 * a build whose shim can reach position but not alertness reports GE_EN_POSITION, and a bot can
 * see that it is flying blind on awareness instead of reading every guard as oblivious.
 *
 * `count` is the caller's array length. An implementation must refuse if it is smaller than it
 * expects rather than writing past it. */
typedef int (*GeEnemyAtFn)(int index, float *out, int count);

/* Register the game-side accessors. Passing NULL for either uninstalls, which is what a level
 * teardown should do -- a stale source outliving its level hands out positions of characters that
 * no longer exist, and those read as perfectly plausible enemies. */
void geEnemySourceInstall(GeEnemyCountFn count_fn, GeEnemyAtFn at_fn);
int  geEnemySourceInstalled(void);

/* ---------------------------------------------------------------- queries
 *
 * All of these report zero/absent with no source installed rather than failing, so a bot written
 * against this API runs unchanged on a build whose shim has not landed. It simply sees an empty
 * world, which is the honest answer.
 */

int geEnemyCount(void);

/* By slot index, 0..geEnemyCount()-1. Returns 0 for an empty or invalid slot. Index is NOT stable
 * across frames as characters die and slots are reused -- use geEnemyById to follow one enemy. */
int geEnemy(int index, GeEnemy *out);

/* By chrnum. This is the one to hold onto: a bot tracking "the guard that shot me" needs an
 * identity that survives another character dying and freeing a lower slot. */
int geEnemyById(int id, GeEnemy *out);

/* Enemies within `radius` of a point, NEAREST FIRST, at most `max`. Returns how many were
 * written. Dead characters are excluded -- a corpse is not a threat, and every caller would
 * otherwise have to filter them, which is the kind of thing exactly one caller forgets. */
int geEnemiesNear(float x, float y, float z, float radius, GeEnemy *out, int max);

/* How many living enemies believe their target is within `radius` of this point.
 *
 * Not the same question as "how many enemies are near here", and the difference is the point: a
 * spot can be crowded and safe if nobody is looking at it, or empty and lethal because four
 * guards are converging on it. This scores a DESTINATION, which is what a route follower needs
 * before it commits to the next waypoint. */
int geEnemyThreatAt(float x, float y, float z, float radius);

#ifdef __cplusplus
}
#endif

#endif /* GE_ENEMY_API_H */
