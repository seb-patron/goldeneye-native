/* What is around a player, and who can see them.
 *
 * The knowledge APIs answer where things ARE. This answers what is IN THE WAY and who is looking
 * -- the questions a bot has to ask every tick and could not ask at all until now.
 *
 * The distinction matters more than it sounds. gePortProbeWalkable returns yes or no, so a policy
 * behaves identically at a wall, a crate and a closed door, when the correct response to each is
 * different: turn, shoot or skirt, press the action button. Every obstacle recovery in
 * ge_bot_route.c so far has been a guess for that reason.
 *
 * ⚠️ These are seeded from the tile under the asking point, so they are honest asked from where a
 * body stands and guesswork asked about a spot nobody is at. That is the same rule the edge
 * validator had to learn: the seed decides the answer.
 */
#ifndef GE_SENSE_API_H
#define GE_SENSE_API_H

/* What a line ran into. A bitmask, not a verdict -- a door is an obstacle to a route planner and
 * an opportunity to a bot with a hand, and only the caller knows which it is. */
#define GE_SENSE_CLEAR   0u
#define GE_SENSE_WALL    (1u << 0)   /* level geometry: turn, or go around */
#define GE_SENSE_DOOR    (1u << 1)   /* openable: press the action button */
#define GE_SENSE_OBJECT  (1u << 2)   /* crate, scenery, path blocker: shoot it or skirt it */
#define GE_SENSE_BODY    (1u << 3)   /* a character or another player: wait, or shoot */

typedef struct GeSenseContact {
    unsigned int what;      /* GE_SENSE_* bitmask; GE_SENSE_CLEAR when nothing blocks */
    float        distance;  /* how far along the ray the first blocked sample sat */
    float        x, z;      /* the last point known clear, i.e. where a body would stop */
} GeSenseContact;

/* Is the straight line from a to b blocked, and by what. */
unsigned int geSenseLine(float from_x, float from_z, float to_x, float to_z);

/* Look ahead from a point along a heading in DEGREES, atan2(x, z) like everything else here.
 * Walks the ray in samples so the caller learns roughly HOW FAR the obstacle is, which a single
 * line test cannot say. Returns 1 and fills `out` always; check out->what for CLEAR. */
int geSenseAhead(float x, float z, float heading_deg, float reach, GeSenseContact *out);

/* The clearest heading within +/- span of `heading_deg`, or the input heading when everything is
 * blocked. Sweeps outward from straight ahead so a small correction is preferred to a large one,
 * and considers the full circle when span >= 180. */
float geSenseClearestHeading(float x, float z, float heading_deg, float span, float reach);

/* Can enemy `index` see player `slot`? Line of sight only -- NOT whether it is looking.
 * Alertness and lastseetarget60 in the enemy API answer that, and conflating them gives a bot
 * that hides from a guard facing away and walks past one staring at it. */
int geSenseVisibleTo(int enemy_index, int player_slot);

/* How many living enemies have line of sight to this player right now. */
int geSenseWatchers(int player_slot);

#endif /* GE_SENSE_API_H */
