/* What is around a player, and who can see them.
 *
 * The knowledge APIs answer where things are. This answers what is in the way and who is looking
 * -- the questions a bot has to ask every tick and could not ask at all until now.
 *
 * The distinction matters more than it sounds. gePortProbeWalkable returns yes or no, so a policy
 * behaves identically at a wall, a crate and a closed door, when the correct response to each is
 * different: turn, shoot or skirt, press the action button. Every obstacle recovery in
 * ge_bot_route.c so far has been a guess for that reason.
 *
 * These are seeded from the tile under the asking point, so they are honest asked from where a
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

/* What actually stops a body and will not move on its own.
 *
 * GE_SENSE_BODY is not in here, and that is not a judgement call about tactics --
 * The line starts at the asking position, so the asker'S own collision sets it. Every reading
 * came back with BODY, every direction read blocked, and geSenseClearestHeading found nothing
 * open anywhere on the map. Steering decisions must use this mask; deciding whether to shoot
 * something can look at BODY on its own. */
#define GE_SENSE_SOLID   (GE_SENSE_WALL | GE_SENSE_OBJECT)

typedef struct GeSenseContact {
    unsigned int what;      /* GE_SENSE_* bitmask; GE_SENSE_CLEAR when nothing blocks */
    float        distance;  /* how far along the ray the first blocked sample sat */
    float        x, z;      /* the last point known clear, i.e. where a body would stop */
} GeSenseContact;

/* Is the straight line from a to b blocked, and by what. */
unsigned int geSenseLine(float from_x, float from_z, float to_x, float to_z);

/* Look ahead from a point along a heading in DEGREES, atan2(x, z) like everything else here.
 * Walks the ray in samples so the caller learns roughly how FAR the obstacle is, which a single
 * line test cannot say. Returns 1 and fills `out` always; check out->what for CLEAR. */
int geSenseAhead(float x, float z, float heading_deg, float reach, GeSenseContact *out);

/* The clearest heading within +/- span of `heading_deg`, or the input heading when everything is
 * blocked. Sweeps outward from straight ahead so a small correction is preferred to a large one,
 * and considers the full circle when span >= 180.
 *
 * Judges on GE_SENSE_SOLID, so a direction with a person in it still counts as open. */
float geSenseClearestHeading(float x, float z, float heading_deg, float span, float reach);

/* Can enemy `index` see player `slot`? Line of sight only -- not whether it is looking.
 * Alertness and lastseetarget60 in the enemy API answer that, and conflating them gives a bot
 * that hides from a guard facing away and walks past one staring at it. */
int geSenseVisibleTo(int enemy_index, int player_slot);

/* How many living enemies have line of sight to this player right now. */
int geSenseWatchers(int player_slot);

/* ---------------------------------------------------------------- 1a: attention, not sight
 *
 * geSenseVisibleTo is a LINE. A guard facing away has a clear line and is not looking, and a bot
 * that treats those as the same hides from someone who never noticed it.
 *
 * Both are kept. Train reporting 17-19 watchers of 40 guards is what an unobstructed line down a
 * row of carriages looks like -- the fix is the cone, not tightening the line test until the
 * number flatters.
 *
 * Returns a bitmask so a caller can tell WHICH condition failed: a guard with a line but facing
 * away is one you can walk behind, and one facing you through a wall is one you must not step in
 * front of. A bool throws away the half that decides what to do next. */
#define GE_NOTICE_NONE     0u
#define GE_NOTICE_LINE     (1u << 0)   /* unobstructed line of sight                      */
#define GE_NOTICE_FACING   (1u << 1)   /* the player is inside the enemy's view cone      */
#define GE_NOTICE_ALERT    (1u << 2)   /* the enemy is alert enough to be watching at all */
#define GE_NOTICE_SEEN     (GE_NOTICE_LINE | GE_NOTICE_FACING | GE_NOTICE_ALERT)

/* The facing could not be read at all -- distinct from facing-away, and the distinction is the one
 * that matters. Facing away means you can walk behind it; unknown means you cannot assume that. A
 * build without the facing accessor must not read as "nobody is looking". Same rule as
 * GePlayerState's absent fields: absent is not zero. */
#define GE_NOTICE_FACE_UNKNOWN (1u << 3)

/* Half-angle of a character's view cone. Generous rather than tight: a guard that half-notices you
 * is a real event, and a flatteringly narrow cone makes a bot confident exactly where it should
 * not be. */
#define GE_NOTICE_CONE_DEG 60.0f

unsigned int geSenseNoticedBy(int enemy_index, int player_slot);

/* How many enemies have all three, against geSenseWatchers which counts lines only. The PAIR is
 * the point: a wide gap means many enemies could turn and see you. */
int geSenseNoticing(int player_slot);

/* ---------------------------------------------------------------- 1b: contact, not prediction
 *
 * "There is a wall 40 units ahead" is a prediction. "I have been pushing forward for half a second
 * and gone nowhere" is a fact, and it is the one that means stuck. They disagree often: a body
 * wedged on a corner the ray misses, or a doorway the ray fits through and the body does not.
 *
 * Fed the slot's position once a frame; answers from history rather than geometry. No ray. */
void geSenseContactUpdate(int player_slot, float x, float z, int commanded_move);

/* Non-zero when the slot was commanded to move and has not moved for `ticks` frames. A legitimate
 * stall of a frame or two happens whenever the collision update clips a corner, hence the window
 * rather than an instant. */
int geSenseIsStuck(int player_slot, int ticks);

/* Distance actually travelled over the remembered window. Separates "wedged" from "moving slowly",
 * which look identical at a single instant. */
float geSenseRecentTravel(int player_slot);

/* ---------------------------------------------------------------- 1c: a body is not a line
 *
 * A ray passes through gaps narrower than the player, so geSenseAhead can report clear down a
 * corridor a body cannot enter -- which reads as the follower refusing a path the data says is
 * fine.
 *
 * Sweeps three parallel lines a body-radius apart and takes the NEAREST obstruction, because a
 * body stops at whichever shoulder meets something first. Judges on GE_SENSE_SOLID for the same
 * reason geSenseClearestHeading does. */
#define GE_BODY_RADIUS 30.0f

int geSenseAheadForBody(float x, float z, float heading_deg, float reach, GeSenseContact *out);

/* The clearest heading judged with a BODY, not a line.
 *
 * geSenseClearestHeading is a line test and a line has no width, so a crate/wall gap narrower
 * than the player passes it -- and the sweep then reports that gap as the best way out. A router
 * that commits to it wedges itself in the one direction it cannot fit through, and every trace
 * says it chose correctly. The sensor is lying, the policy is fine.
 *
 * Anything steering a body must use this one. The line version remains for questions genuinely
 * about a line, such as whether a shot or a sightline reaches.
 *
 * `out_room` receives how far the chosen heading is clear for, so a caller that must move
 * through a tight place knows how much it bought. */
float geSenseClearestHeadingForBody(float x, float z, float heading_deg, float span, float reach,
                                    float *out_room);

/* ---------------------------------------------------------------- 1d: what can I act on
 *
 * The prop data knows where doors, switches and pickups are. Nothing said whether a bot standing
 * here can act on one, which is the only form of the question a bot can use. */
#define GE_USABLE_NONE   0u
#define GE_USABLE_DOOR   (1u << 0)
#define GE_USABLE_PICKUP (1u << 1)   /* collectable: a key, a weapon, ammo */
#define GE_USABLE_SWITCH (1u << 2)

/* Reach of the action button. From chraction.c:9138, where a character opens a door by walking
 * within 200 units of it -- the game's own number rather than a guess. */
#define GE_USABLE_REACH 200.0f

typedef struct GeUsable {
    unsigned int kind;      /* GE_USABLE_* */
    int          prop;      /* index into the prop table, so a caller can ask for more */
    float        x, y, z;
    float        distance;
} GeUsable;

/* What is within reach of this point, nearest first. Returns how many were written. */
int geSenseUsable(float x, float y, float z, GeUsable *out, int max);

#endif /* GE_SENSE_API_H */
