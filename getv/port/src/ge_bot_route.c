/* A bot that walks a real route, using the world API for knowledge and the player API to act.
 *
 * This is what the two seams were built for. ge_world_api says where the objective is and which
 * waypoints lead to it; ge_player_api posts the stick input that gets there. Nothing here knows
 * anything about GoldenEye's internals -- it reads knowledge, reads state, posts input -- which
 * is the same shape a network peer or a learning agent has.
 *
 * The steering law is the one validated in tools/routesim.py, constants included.
 *
 * Turn and walk at once, with forward speed scaled DOWN by heading error. That scaling is
 * essential rather than a refinement: turning radius is speed over turn rate, about 114 units
 * at full pelt, so a bot that walks flat out while turning cannot get inside a 120-unit arrival
 * radius and orbits its own waypoint instead. The model measures that as 29 of 61 routes failed
 * with the scaling removed. Do not "simplify" it away.
 *
 * Heading comes from the game.
 *
 * gePortPlayerAngle (objective_status.c, where `struct player` is visible) returns the collision
 * record's forward vector as degrees -- the same vector the walk code builds its move offset
 * from, so heading and travel cannot disagree. gePlayerStateGet reports it as GE_ST_ANGLE.
 *
 * The old dead-reckoning estimator survives as a fallback for a slot with no collision record
 * yet. On its own it deadlocked: turning more than GE_BR_ALIGN_DEG zeroes forward speed, so the
 * bot never moved, so the estimate never updated. Do not make it the primary source again.
 *
 *   GETV_BOT_ROUTE=<slot>       drive this slot along a route
 *   GETV_BOT_ROUTE_LEVEL=<name> which level's knowledge to load (no level accessor exists yet)
 *   GETV_BOT_ROUTE_OBJ=<n>      which objective to walk to, default the first with a route
 *   GETV_BOT_ROUTE_TRACE=1      log progress once a second
 */
#include <math.h>
#include "ge_bot_nav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_player_api.h"
#include "ge_world_api.h"

/* The engine's own navigation graph and player position, exposed from objective_status.c where
 * the setup and player structures are visible. */
extern int gePortNavNearestPad(float x, float z);
extern int gePortPlayerHealth(int idx, float *out_hp, float *out_armour, int *out_dead);
extern int gePortNavCount(void);
extern int gePortNavAt(int index, int *out_pad, float *out_pos, int *out_group);
extern int gePortNavNearest(float x, float z);
extern int gePortNavRoute(int from, int to, int *out, int max);
extern int gePortPlayerPos(int idx, float *out);
extern int gePortTileAt(float x, float z, int *out_id, int *out_room);
extern int gePortOpenDoorAhead(int idx, float to_x, float to_z);
extern int gePortObstacleEdge(float x0, float z0, float x1, float z1,
                              float *out_left, float *out_right);

#include "ge_enemy_api.h"
#include "ge_sense_api.h"
#include "ge_bot_arbiter.h"
#include "ge_world_levels.h"    /* generated: stage number -> extractor level name */

/* Straight from tools/routesim.py. Changing one of these without re-running the model is how a
 * validated law quietly becomes an unvalidated one. */
#define GE_BR_WALK          60.0f   /* N64 counts; the walk deadzone subtracts about 5 */
/* World units. Must stay outside the turning circle (about 114 at full pelt) or the bot orbits
 * its own target -- that is where 120 came from and it is still the floor.
 *
 * Raised to 200 because a waypoint is a PAD, and a pad is frequently not somewhere a body can
 * stand: measured with the teleport probe, 139 of Train's 180 nodes are unplaceable, and across
 * the twenty levels it runs 33 to 240 each. Requiring a body to reach within 120 of a pad centre
 * therefore demands something the level often does not permit -- Train's bot closed 626 to 173
 * and could get no nearer, not because it failed to navigate but because the last 53 units are
 * inside a bench.
 *
 * This is a statement about the WAYPOINT DATA, not a tolerance to tune when a bot misses. If
 * pads are ever replaced by points sampled on the navmesh, put it back to 120. */
#define GE_BR_ARRIVE       200.0f
#define GE_BR_TURN_GAIN      3.0f
#define GE_BR_ALIGN_DEG     60.0f
#define GE_BR_STICK_MAX     80.0f

/* Below this much movement in a tick the heading estimate is noise rather than direction. */
#define GE_BR_MOVE_EPSILON   1.5f

/* How long to tolerate "walking but not moving" before treating it as an obstacle, and how long
 * to steer around one. The first is generous because a legitimate stall happens for a tick or
 * two whenever the collision update clips a corner; the second is roughly the time it takes to
 * turn 90 degrees at full deflection, since the yaw is 3.5 degrees a tick. */
/* How close an enemy's BELIEF has to be to the next waypoint for it to count as contested.
 * wider than the arrival radius: the question is not "is a guard standing on it"
 * but "is anyone heading there", and a belief is a destination. */
#define GE_BR_THREAT_RADIUS 300.0f

/* Cap on how long the bot waits for a contested waypoint to clear, in frames. There must be a
 * cap: holding until a route clears sounds prudent and produces a bot that stands still forever,
 * because nothing about waiting makes a guard change its mind -- and a frozen bot is
 * indistinguishable from a crashed one. Three seconds, then commit. */
#define GE_BR_MAX_HOLD 180

/* What a walking body can climb and drop between two samples 55 units apart. Generous enough
 * for stairs and a kerb, tight enough that a floor reachable only by falling is refused. */
#define GE_BR_MAX_STEP      40.0f
#define GE_BR_MAX_DROP      90.0f

/* How long to lean on the action button before deciding an obstacle is not a door.
 *
 * Doors need opening, and a bot that only ever walks and turns will scrape along a closed one
 * forever. Bunker 1's spawn corridor is exactly that: a reachability map shows it sealed at both
 * ends with CDTYPE_DOORS in the mask and open at both ends without it, so the two exits are
 * doors. Long enough to cover the open animation, short enough that a real wall is not mistaken
 * for a stuck door for more than a moment. */
#define GE_BR_USE_TICKS     45

/* How far ahead the bot looks. Far enough to react before contact at walking speed, short enough
 * that it does not steer around something it was going to turn away from anyway. */
#define GE_BR_LOOKAHEAD    160.0f

/* How long an avoidance heading is held. Roughly the time to turn 60 degrees and clear the
 * obstacle at walking pace -- long enough to finish the manoeuvre, short enough to notice the
 * world changed. */
#define GE_BR_AVOID_TICKS   40

#define GE_BR_STUCK_TICKS   30
#define GE_BR_DETOUR_TICKS  26

/* how repeated stuck events AT one spot are escalated, not retried forever.
 *
 * Measured on Train: the stuck/door/detour cycle is fully deterministic (same position, same
 * heading -> same sweep result every time) and had no memory. A bot that keeps returning to one
 * chokepoint -- because the route's straight-line pursuit resumes toward the SAME unreached
 * target the instant the 26-tick detour ends -- repeats an identical failed sequence indefinitely.
 * Confirmed directly: a run spent 4,923 of 5,637 active frames confined to one 400-unit patch,
 * cycling stuck/door/detour roughly every 100 frames the whole time, at a spot with a real
 * cross-corridor wall in the extracted geometry and no LOCKED-door message ever printed -- so the
 * detour was finding SOME way clear, just not enough of one before pursuit turned the bot back.
 *
 * This does not touch the sweep's direction choice, which is already reasoned from real geometry
 * (gePortProbeWalkable, gePortProbeStandable) and not the thing at fault. It only lengthens the
 * COMMITMENT once a detour is chosen, so the bot gets further from the obstruction before pursuit
 * resumes -- the plain, minimal fix for "the escape manoeuvre was too short to work twice in a
 * row", tried before anything more invasive. */
#define GE_BR_STUCK_REPEAT_RADIUS  150.0f  /* same trap if the next stuck point is this close */
#define GE_BR_STUCK_ESCALATE_MAX      4    /* cap: detour never exceeds MAX * DETOUR_TICKS */
static float ge_br_stuck_last_x, ge_br_stuck_last_z;
static int   ge_br_stuck_last_valid;
static int   ge_br_stuck_repeat;    /* consecutive detours triggered near the same spot */

static int   ge_br_ready;
static int   ge_br_slot = -1;
static int   ge_br_obj  = -1;
static int   ge_br_trace;
static int   ge_br_step;            /* which step of the route we are on */
static int   ge_br_joined;          /* has the bot reached the route's first node yet */
static int   ge_br_stuck;           /* consecutive ticks commanded forward with no movement */
static int   ge_br_held;            /* frames spent waiting on a contested waypoint */
static float ge_br_turn_sign;       /* latched turn direction; 0 when not committed */
static float ge_br_turn_from;       /* |err| when the turn was committed to */
static int   ge_br_use;             /* ticks left pressing the action button at a door */
static int   ge_br_avoid;           /* ticks left committed to an avoidance heading */
static float ge_br_avoid_h;         /* the heading committed to, held for the whole manoeuvre */
static int   ge_br_detour;          /* ticks left steering around an obstacle */
static float ge_br_detour_sign;     /* which way round it -- kept until the detour ends */
static int   ge_br_steps;
static int   ge_br_have_heading;
static float ge_br_heading;         /* degrees, atan2(dx, dz), matching the extractor */
static float ge_br_px, ge_br_py, ge_br_pz;
static int   ge_br_have_prev;

/* Advance toward a contested waypoint, or hold? Returns 1 to advance.
 *
 * Pure: the interesting part is the policy, and a policy tangled up with posting input
 * and reading world state can only be tested by running the game. See tests/test_bot_policy.c. */
static int ge_br_should_advance(int threat, int held_frames)
{
    if (threat <= 0) { return 1; }             /* nobody is heading there */
    return (held_frames >= GE_BR_MAX_HOLD);    /* waited long enough; commit rather than freeze */
}

static float ge_br_norm180(float a)
{
    while (a > 180.0f)  { a -= 360.0f; }
    while (a < -180.0f) { a += 360.0f; }
    return a;
}

/* Is there really a door in front of us, or did the ray just clip a door frame?
 *
 * GeSenseLine reports wall|door|object together whenever the ray grazes a doorway edge, and
 * taking the DOOR bit from that made the bot drive into a wall with the action button held --
 * for an entire run, while its actual target sat at a bearing of -60. The bitmask says what the
 * line touched, not what is in front of you.
 *
 * The prop table knows where doors are, so ask it. Within the engine's own 200-unit interact
 * range (doorTestForInteract, propobj.c:14411) and roughly ahead, because that function also
 * wants the door ON SCREEN.
 */
static int ge_br_door_ahead(const GePlayerState *st, float to_target_bearing)
{
    GeWorldProp d;
    float dx, dz, bearing, off_heading, off_route;

    if (!geWorldNearestProp(GE_PROP_DOOR, st->x, st->y, st->z, &d)) { return 0; }
    dx = d.x - st->x;
    dz = d.z - st->z;
    if (((dx * dx) + (dz * dz)) > (200.0f * 200.0f)) { return 0; }

    bearing = (float) (atan2((double) dx, (double) dz) * 180.0 / 3.14159265358979);
    off_heading = ge_br_norm180(bearing - st->angle);
    if ((float) fabs((double) off_heading) > 45.0f) { return 0; }

    /* And IT has TO BE ON the way.
     *
     * A door within 200 units and in front of the bot is not automatically the route: Train's
     * carriages have doors down both sides, so the bot stood pressing USE on a side door while
     * its waypoint sat at a bearing of -60 and never turned. "There is a door here" and "the
     * door is where I am going" are different questions and only the second should divert the
     * follower.
     *
     * Compared against the bearing to the TARGET rather than the heading, because the heading is
     * whatever the last manoeuvre left it as -- including pointing at the door it is about to
     * decide about, which makes the test agree with itself. */
    off_route = ge_br_norm180(bearing - to_target_bearing);
    return ((float) fabs((double) off_route) <= 50.0f);
}

#define GE_BR_CLEARSTEP 260.0f   /* how far ahead a candidate heading is tested, runtime units */

/* Routing on the engine's own waypoint graph (padhalllv.c).
 *
 * The level setup ships a two-level graph that the guards route over; our tile graph is a
 * reconstruction of it. Train's is 104 waypoints, 206 links, 6 groups, and waypointFindRoute
 * returns 63 hops from the spawn to the far end.
 *
 * Its advantage is authorship: every edge was placed by someone who could playtest it, so an edge
 * existing means a body can walk it, doors included. Ours infers that, and is wrong exactly where
 * the level is hard.
 *
 * Held as indices into the engine's list, which is also the id space worth reporting -- "waypoint
 * 41, group 3" means something in the level's own data. */
#define GE_BR_NAV_MAX 128

static int   ge_br_nav[GE_BR_NAV_MAX];
static int   ge_br_nav_len = 0;
static int   ge_br_use_nav = 0;

static void ge_br_nav_build(void)
{
    float spawn[3];
    GeWorldObjective ob;
    int from, to = -1;

    ge_br_nav_len = 0;
    if (!ge_br_use_nav) { return; }
    if (gePortNavCount() <= 0) {
        printf("[getv][nav] this level ships no waypoint graph -- staying on the tile graph\n");
        ge_br_use_nav = 0;
        return;
    }

    /* From where the bot actually is, to the objective's target. Both resolved to nav waypoints,
     * because a route between two arbitrary points is not something this graph answers. */
    if (!gePortPlayerPos(ge_br_slot, spawn)) { ge_br_use_nav = 0; return; }
    from = gePortNavNearest(spawn[0], spawn[2]);

    if (geWorldObjective(ge_br_obj, &ob) && ob.steps > 0) {
        to = gePortNavNearest(ob.tx, ob.tz);
    }
    if (from < 0 || to < 0 || from == to) {
        printf("[getv][nav] no nav endpoints for objective %d -- staying on the tile graph\n",
               ge_br_obj);
        ge_br_use_nav = 0;
        return;
    }

    ge_br_nav_len = gePortNavRoute(from, to, ge_br_nav, GE_BR_NAV_MAX);
    if (ge_br_nav_len <= 0) {
        printf("[getv][nav] engine found no route %d -> %d -- staying on the tile graph\n",
               from, to);
        ge_br_use_nav = 0;
        return;
    }
    printf("[getv][nav] routing on the ENGINE graph: %d hop(s), waypoint %d -> %d\n",
           ge_br_nav_len, from, to);
    fflush(stdout);
}

/* Aim at an edge of the obstacle, not at "somewhere open".
 *
 * Ported from Perfect Dark's chrNavTryObstacle. PD does not sweep for a clear heading: it takes
 * the two ends of the thing that blocked it and goes round one of them, pushed outward by a
 * clearance of the body radius times 1.26 so the shoulder clears too. Choosing between the two
 * ends is then a real comparison -- which side is actually nearer the way I need to go -- instead
 * of the sign of an error, which is what kept sending our bot right when left was correct.
 *
 * GE reports the blocking edge through gePortObstacleEdge, built on the same stanSavedColl_tile /
 * stanSavedColl_pointI pair the engine keeps for its own collision.
 *
 * Returns 1 and writes a heading when it found a side worth trying.
 */
#define GE_BR_RADIUS     35.0f
#define GE_BR_CLEARANCE  1.26f   /* PD's own figure, chraction.c: chr->radius * 1.26f */

extern int gePortSkirt(float x0, float z0, float tx, float tz, float *out_x, float *out_z);

static int ge_br_edge_heading(float x, float z, float tx, float tz, float *out_deg)
{
    /* Rare's own construction, not the approximation this function used to be.
     *
     * gePortSkirt is a direct port of Perfect Dark's chrNavTryObstacle (chraction.c:12120): pick
     * the obstacle corner that deviates least from the desired bearing, aim at the tangent point
     * a clearance beyond it (rotate by acos(clearance/distance), not just push the corner point
     * outward on its own radial), and -- the half this function never had -- CONFIRM the line
     * there is actually walkable before committing, falling through to the other side if it is
     * not. Aiming at an unvalidated corner is how a bot commits to the one direction it cannot
     * fit through and the trace says it chose correctly every time.
     *
     * Kept as a heading rather than exposing the point directly: this function's caller steers
     * on a heading, and changing that contract would touch every call site for no reason. */
    float sx, sz;
    int side = gePortSkirt(x, z, tx, tz, &sx, &sz);

    if (side <= 0) { return 0; }
    if (out_deg != NULL) {
        *out_deg = (float) (atan2((double) (sx - x), (double) (sz - z)) * 180.0 / 3.14159265358979);
    }
    return 1;
}


/* Fighting back.
 *
 * The follower had no combat at all, and on Train that is fatal rather than merely incomplete:
 * the flight recorder shows health falling 1.00, 0.88, 0.69, 0.39, 0.11 and then DEAD at around
 * waypoint 11, in a carriage the walkthroughs describe as holding most of the level's guards.
 * Every stall past that point was a corpse posting stick input, which looks exactly like a
 * navigation failure from the outside and is why the recorder now logs health.
 *
 * The policy is small. Face the nearest guard that can see us, stop walking, and
 * fire in bursts. GoldenEye aims for the player once the target is roughly in front, so a
 * follower does not need a firing solution -- it needs to stop walking away and to point.
 *
 * Bursts rather than a held trigger, which is Perfect Dark's shape (botactGetShootInterval60
 * paces every weapon rather than holding it down). Holding fire empties a clip into a wall
 * during the turn and leaves nothing for the guard behind it.
 */
/* Engagement range. 1400 was too generous on Train: nearly every guard in the carriage qualified,
 * the bot fought continuously and advanced two waypoints instead of eleven. It survived, which is
 * the point, but a bot that never arrives is not finishing a mission either. 800 is roughly the
 * length of a carriage section, so it engages what is in the room and walks past what is not. */
#define GE_BR_ENGAGE_RANGE   800.0f
#define GE_BR_ENGAGE_SEEN      150    /* frames: a guard that had eyes on us this recently is shooting */
#define GE_BR_ENGAGE_KEEP   1100.0f   /* hysteresis: a target already chosen is kept a little longer */
#define GE_BR_ENGAGE_MAX       420    /* frames to spend on one target before walking on */
/* GE_BR_ENGAGE_FACE is gone. It was 35.0, an unsourced guess -- and independently the SAME wrong
 * number a first pass at ge_bot_arbiter.c also guessed, before that file traced the real test to
 * chrpropScoreAutoAimTarget (chrprop.c) and corrected it to 15, derived from the engine's own
 * screen-space acceptance box and its 46-degree fovy. Two people reaching for 35 as a round-number
 * default is worth noting on its own; neither of us had a source for it. The arbiter's
 * GE_ARB_FIRE_CONE is that number now, sourced, and this file no longer keeps its own copy. */
#define GE_BR_BURST_ON           6    /* frames holding fire */
#define GE_BR_BURST_OFF         10    /* frames between bursts */

/* Off by default, and the measurement is why.
 *
 * Combat exists because the bot was dying: health fell 1.00, 0.88, 0.69, 0.39, 0.11, DEAD around
 * waypoint 11, and every stall past that point was a corpse posting stick input. But the deaths
 * turned out to be downstream of the avoidance hold, which was only held while the sensor kept
 * agreeing there was something there. With that fixed the bot survives the same carriage without
 * firing a shot. Measured on Train, same build, 20,000 frames:
 *
 *     routing only        waypoint 10, ends at 0.39 health
 *     routing and combat  waypoint  4, ends at 0.34 health
 *
 * So as written it costs six waypoints and buys nothing. It stops to shoot, and stopping is what
 * it cannot afford on a level with a timer and a moving train.
 *
 * Firing without stopping was tried too, on the theory that the cost was the standing still.
 * It is worse: waypoint 4 and dead, against waypoint 4 and alive for stopping to aim, and
 * waypoint 10 alive for not fighting at all. Shooting announces the bot to a carriage it cannot
 * win, and it does not need to win -- it needs to walk past. A bot that shoots badly is worse
 * than one that does not shoot.
 *
 * The next version IS ge_bot_arbiter.c, wired in below. The measurements above are exactly what
 * it was derived from: the shared cost between the two failing modes was the turn, not the
 * trigger or the stop, so the fix is to fire inside the cone WITHOUT turning and to hold position
 * rather than advance when a target is close enough that ignoring it is worse than the turn.
 *
 * GETV_BOT_FIGHT=1 to enable it. GETV_BOT_AIM=1 now means something narrower than it used to: it
 * forces the CORNERED response (stop, face, fire) on every engagement rather than only when the
 * arbiter judges the target close enough to demand it, which reproduces the OLD stop-and-face
 * behaviour above for a direct A/B against the new default. */
static int   ge_br_fight = 0;
static float ge_br_nav_aim_x, ge_br_nav_aim_z;   /* mirrors gePortNavTick's last aim, for the door check */
static float ge_br_nav_target_x, ge_br_nav_target_z;   /* wp.x/wp.z, or the portal point when crossing rooms */
static int   ge_br_target_id = -1;    /* chrnum of the guard being fought, -1 for none */
/* The mission's own objective, separately from guard combat.
 *
 * Train's objectives are things to destroy -- brake units, tagged 8 through 13 -- mounted low on
 * the wall beside each carriage door, worth 1000 damage and requiring aim mode pitched down: the
 * C-button look never puts the crosshair on a knee-height target, measured with the object's own
 * damage counter reading zero across a full run while GE_IN_AIM with stick_y down took the same
 * counter from 0 to 750 and past its threshold. This is not optional the way guard combat is --
 * a route with no live brake units left is not a route this level can finish -- so it runs
 * unconditionally rather than behind GETV_BOT_FIGHT.
 */
static int   ge_br_obj_tag = -1;      /* setup tag of the target being worked, -1 for none */
static int   ge_br_obj_aimed;         /* pitched down for this target already */
static int   ge_br_obj_shots;
static int   ge_br_engaged;           /* frames spent on this target */
static int   ge_br_aim;               /* GETV_BOT_AIM=1: force the cornered (stop-and-face) response */

/* The stick-versus-heading sense, resolved the same way the steering block resolves it. */
static int ge_br_sign_of(void)
{
    static int sign = 0;
    if (sign == 0) {
        const char *e = getenv("GETV_BOT_ROUTE_SIGN");
        sign = (e && *e && *e != '0') ? 1 : -1;
    }
    return sign;
}
static int   ge_br_burst;             /* counts down through one on/off cycle */

/* The guard worth shooting at: nearest, alive, within range, and able to see us. Visibility
 * matters both ways -- a guard that cannot see the bot is not shooting it, and shooting through
 * a wall wastes the clip and announces the bot to the room. */
static int ge_br_pick_target(float x, float y, float z, GeEnemy *out)
{
    int i, best = -1;
    float bestd = 0.0f;

    /* Stay on the guard already being fought while it is alive, visible and has not run off.
     * Re-choosing every frame is the same mistake the steering made before the turn latch: with
     * several guards at similar range the bot swaps between them and shoots none of them, which
     * the log shows as two turning frames for every firing frame. */
    if (ge_br_target_id >= 0 && ge_br_engaged < GE_BR_ENGAGE_MAX) {
        for (i = 0; i < geEnemyCount(); i++) {
            GeEnemy e;
            float dx, dz;
            if (!geEnemy(i, &e) || e.id != ge_br_target_id) { continue; }
            dx = e.x - x;
            dz = e.z - z;
            if (e.alive && sqrtf(dx * dx + dz * dz) <= GE_BR_ENGAGE_KEEP
                && geSenseVisibleTo(i, ge_br_slot)) {
                ge_br_engaged++;
                *out = e;
                return i;
            }
            break;
        }
    }

    ge_br_target_id = -1;
    ge_br_engaged = 0;

    for (i = 0; i < geEnemyCount(); i++) {
        GeEnemy e;
        float dx, dz, d;

        if (!geEnemy(i, &e) || !e.alive) { continue; }
        dx = e.x - x;
        dz = e.z - z;
        d = sqrtf(dx * dx + dz * dz);
        if (!geSenseVisibleTo(i, ge_br_slot)) { continue; }

        /* Close enough to matter, or currently engaging us. Distance alone is the wrong test:
         * tightening it from 1400 to 800 stopped the bot fighting the whole carriage at once and
         * immediately got it killed again by the guards firing from further down the car. A guard
         * that had eyes on us in the last two and a half seconds is shooting, whatever the range. */
        if (d > GE_BR_ENGAGE_RANGE) {
            if (!(e.fields & GE_EN_BELIEF) || e.saw_target_ago > GE_BR_ENGAGE_SEEN) { continue; }
        }
        if (best < 0 || d < bestd) { bestd = d; best = i; *out = e; }
    }
    if (best >= 0) {
        ge_br_target_id = out->id;
        ge_br_engaged = 0;
    }
    (void) y;
    return best;
}


/* The nearest live objective target, in setup-tag terms, doors/keys/collectables excluded (those
 * have their own handling). "Live" is asked of the OBJECT, not the pack: the pack lists a
 * destroyed brake unit forever, so a picker that trusted it would keep aiming at wreckage. */
static int ge_br_pick_objective_target(float x, float z, float *out_x, float *out_z, int *out_tag,
                                        int *out_room, float *out_radius)
{
    extern int gePortTargetState(int tag, int *destroyed, float *dmg, float *maxdmg);
    int i, n, best = -1;
    float bestd = 0.0f, bx = 0.0f, bz = 0.0f, brad = 0.0f;
    int btag = -1, broom = GE_WORLD_NO_ROOM;

    n = geWorldPropCount();
    for (i = 0; i < n; i++) {
        GeWorldProp p;
        float dx, dz, d;
        int dead = 0;

        if (!geWorldProp(i, &p)) { continue; }
        if (p.tag < 0) { continue; }
        if (p.kind == GE_PROP_DOOR || p.kind == GE_PROP_KEY || p.kind == GE_PROP_COLLECTABLE) { continue; }
        if (gePortTargetState(p.tag, &dead, NULL, NULL) && dead) { continue; }

        dx = p.x - x; dz = p.z - z;
        d = dx * dx + dz * dz;
        if (best < 0 || d < bestd) {
            best = i; bestd = d; bx = p.x; bz = p.z; btag = p.tag; broom = p.room; brad = p.radius;
        }
    }
    if (best < 0) { return 0; }
    if (out_x != NULL)      { *out_x = bx; }
    if (out_z != NULL)      { *out_z = bz; }
    if (out_tag != NULL)    { *out_tag = btag; }
    if (out_room != NULL)   { *out_room = broom; }
    if (out_radius != NULL) { *out_radius = brad; }
    return 1;
}

/* The flight recorder.
 *
 * The trace prints every sixtieth frame, which is fine for watching and useless for answering
 * "it turned right there and it should have turned left". That question needs every decision, in
 * order, with the ids and the positions attached -- so it is written to a file instead, one row
 * per event, tab separated so it opens in anything.
 *
 * Columns: frame, event, node, pad, x, z, heading, bearing, err, stick_x, stick_y, note.
 * A row is written when the bot ARRIVES somewhere, when it CHOOSES a turn, and when it hits
 * something -- the three things that together explain a wrong turn.
 *
 * GETV_BOT_LOG names the file; unset writes build/botlog-<level>.tsv.
 */

static FILE *ge_br_log = NULL;
static unsigned ge_br_log_rows = 0;
static int ge_br_locked_seen = 0;
static int ge_br_use_edges = 0;
static int   ge_br_last_target = -1;
/* Centring the avoidance sweep on the target bearing rather than the current heading is a real
 * fix for a real bug -- caught at Train frame 12769, steering asking -66 while the posted stick
 * was +80 -- but it did not move the wall: both centres stall at the same x, so it is off by
 * default until something measures it better. GETV_BOT_NEWSWEEP=1 to enable. */
static int   ge_br_recentre = 0;

static void ge_br_log_open(const char *level)
{
    const char *path = getenv("GETV_BOT_LOG");
    char buf[512];

    if (path == NULL || *path == '\0') {
        snprintf(buf, sizeof buf, "build/botlog-%s.tsv", level);
        path = buf;
    }
    ge_br_log = fopen(path, "w");
    if (ge_br_log == NULL) { return; }
    fprintf(ge_br_log, "frame\tevent\tnode\tpad\tx\tz\theading\tbearing\terr\tstick_x\tstick_y\tnote\n");
    fflush(ge_br_log);
    printf("[getv][botroute] logging every decision to %s\n", path);
    fflush(stdout);
}

static void ge_br_logf(int frame, const char *event, int node, int pad,
                       float x, float z, float heading, float bearing, float err,
                       int sx, int sy, const char *note)
{
    if (ge_br_log == NULL) { return; }
    fprintf(ge_br_log, "%d\t%s\t%d\t%d\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%d\t%d\t%s\n",
            frame, event, node, pad, (double) x, (double) z,
            (double) heading, (double) bearing, (double) err, sx, sy, note ? note : "");
    /* Not flushed per row. Measured on Windows, a flushed line at ~24 ms there -- 516
     * osSyncPrintf sites were costing it seven times its frame rate -- and this recorder writes
     * a row on EVERY tick. Flushing each one does not just run slow, it changes the thing being
     * measured: two runs of the same build logged 1,539 and 9,521 frames in the same wall clock,
     * which made an A/B of two steering policies compare their log volume rather than their
     * steering. Flushed every 512 rows instead, so a crash still leaves nearly everything. */
    if ((++ge_br_log_rows & 511) == 0) { fflush(ge_br_log); }
}

/* The heading the game itself permits.
 *
 * gePortPathClear is the engine's own answer -- stanTestLineUnobstructed for the run, then
 * stanTestVolume at the far end for the body's width -- and it is a better authority than either
 * of the two things this file was using. The sense sweep reports what a ray can see from here;
 * the derived wall set is a reconstruction, and both are approximations of exactly this call.
 *
 * Preference order is the bearing to the target FIRST, then the smallest deviation from it that
 * the engine says is clear. A detour is a cost, so the cheapest legal one wins.
 *
 * Returns -1 for "no answer" -- gePortPathClear cannot seed a tile -- which is not the same as
 * blocked and must not be treated as such.
 */
static int ge_br_use_clear;

static int ge_br_heading_clear(float x, float z, float deg)
{
    extern int gePortPathClear(float x0, float z0, float x1, float z1);
    float r = deg * (float) (3.14159265358979323846 / 180.0);
    /* heading is atan2(dx, dz), so forward is (+sin, +cos). */
    float nx = x + sinf(r) * GE_BR_CLEARSTEP;
    float nz = z + cosf(r) * GE_BR_CLEARSTEP;
    return gePortPathClear(x, z, nx, nz);
}

static float ge_br_clear_heading(float x, float z, float want, int *found)
{
    float off;

    if (found != NULL) { *found = 0; }
    if (ge_br_heading_clear(x, z, want) == 1) {
        if (found != NULL) { *found = 1; }
        return want;
    }
    for (off = 12.0f; off <= 168.0f; off += 12.0f) {
        if (ge_br_heading_clear(x, z, want + off) == 1) {
            if (found != NULL) { *found = 1; }
            return ge_br_norm180(want + off);
        }
        if (ge_br_heading_clear(x, z, want - off) == 1) {
            if (found != NULL) { *found = 1; }
            return ge_br_norm180(want - off);
        }
    }
    return want;
}

/* Level facts from build/levels/<level>.brief.json: what the walkthroughs record about the
 * place, such as Train's traversal axis and its lack of lateral escape.
 *
 * Reported, not steered on. Feeding lateral_escape into the detour sweep was tried and measured
 * worse -- Train reaches waypoint 10 of 46 with a 180-degree sweep, 3 at 110 and 2 at 55 -- because
 * the sweep both dodges obstacles and re-acquires the target, so narrowing it fixes the first by
 * breaking the second. The fact is true; that lever is the wrong one for it.
 *
 * Walls derived from the floor mesh (tools/gen_level_walls.py) were tried the same way and fell
 * the same way: as a hard veto on heading they take Train from waypoint 10 to 1, because a veto
 * mostly forbids ground the bot can walk. The tool and its output remain -- that data belongs in
 * routing, which happens before the first step -- but no runtime consumer does.
 */

static void ge_br_load_brief(const char *level)
{
    char path[512];
    const char *dir;
    FILE *f;
    long size;
    char *buf;

    dir = getenv("GETV_BRIEF_DIR");
    if (dir == NULL || *dir == '\0') { dir = "build/levels"; }
    snprintf(path, sizeof path, "%s/%s.brief.json", dir, level);

    f = fopen(path, "r");
    if (f == NULL) { return; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (1 << 20)) { fclose(f); return; }
    buf = (char *) malloc((size_t) size + 1);
    if (buf == NULL) { fclose(f); return; }
    size = (long) fread(buf, 1, (size_t) size, f);
    fclose(f);
    buf[size] = '\0';

    if (strstr(buf, "\"lateral_escape\": false") != NULL ||
        strstr(buf, "\"lateral_escape\":false") != NULL) {
        printf("[getv][botroute] %s: the level records no lateral escape\n", level);
    }
    free(buf);
}

void gePortBotRouteInit(void)
{
    const char *e;

    if (ge_br_ready) { return; }
    ge_br_ready = 1;

    e = getenv("GETV_BOT_ROUTE");
    if (e == NULL || *e == '\0') { return; }
    ge_br_slot = atoi(e);
    if (ge_br_slot < 0 || ge_br_slot >= GE_MAX_SLOTS) { ge_br_slot = -1; return; }
    ge_br_trace = (getenv("GETV_BOT_ROUTE_TRACE") != NULL);

    e = getenv("GETV_BOT_ROUTE_LEVEL");
    if (e == NULL || *e == '\0') {
        /* No level accessor exists in the port yet, so this cannot be inferred. Saying so beats
         * loading the wrong level's routes and walking confidently into a wall. */
        printf("[getv][botroute] GETV_BOT_ROUTE_LEVEL is required: the port cannot yet report "
               "which level is loaded\n");
        ge_br_slot = -1;
        return;
    }
    ge_br_recentre = (getenv("GETV_BOT_NEWSWEEP") != NULL);
    ge_br_fight    = (getenv("GETV_BOT_FIGHT") != NULL);
    ge_br_aim      = (getenv("GETV_BOT_AIM") != NULL);
    ge_br_use_nav  = (getenv("GETV_BOT_NAV") != NULL);
    /* Off until it wins. The edge model is the right idea and it now genuinely fires -- 173
     * blocking props reported against 4 misses, where the first version using the stan mesh
     * reported 114 misses out of 114 because stan is the FLOOR and crates are not in it. But
     * measured on Train it reaches step 7 where the sweep it replaces reaches 11.
     *
     * The likely reason is the lesson this file has already learned twice: PD holds its chosen
     * side in waydata->mode until it ARRIVES there, and this re-derives the side whenever the
     * avoidance latch lapses, so the bot can swap ends of the same crate. Committing properly is
     * the next piece of work, not a reason to ship the half of it that loses ground.
     *
     * GETV_BOT_EDGES=1 to try it. */
    ge_br_use_edges = (getenv("GETV_BOT_EDGES") != NULL);
    ge_br_log_open(e);
    ge_br_load_brief(e);

    /* What the engine itself thinks the nav graph is.
     *
     * Reported once at init because it settles a question our own routing has been guessing at:
     * GoldenEye ships a waypoint graph in the level setup and the guards route on it. If it is
     * populated here, our tile graph is a reconstruction of something we already had. */
    {
        extern int gePortNavCount(void);
        extern int gePortNavAt(int index, int *out_pad, float *out_pos, int *out_group);
        extern int gePortNavNeighbours(int index, int *out, int max);
        int n = gePortNavCount();
        float st0x = 779.0f, st0z = -60.0f;   /* Bond's measured Train spawn */

        printf("[getv][nav] engine waypoint graph: %d waypoint(s)\n", n);
        if (n > 0) {
            int i, links = 0, groups = -1;
            for (i = 0; i < n; i++) {
                int nb[32], g = -1, pad = -1;
                float pos[3];
                links += gePortNavNeighbours(i, nb, 32);
                if (gePortNavAt(i, &pad, pos, &g) && g > groups) { groups = g; }
                if (i < 3) {
                    printf("[getv][nav]   waypoint %d: pad %d at (%.0f %.0f %.0f) group %d, %d neighbour(s)\n",
                           i, pad, (double) pos[0], (double) pos[1], (double) pos[2], g,
                           gePortNavNeighbours(i, nb, 32));
                }
            }
            printf("[getv][nav] %d link(s), %d group(s)\n", links, groups + 1);

            /* Does the engine's own pathfinder route across it? Spawn to the farthest waypoint,
             * which is the hardest question the graph can be asked. */
            {
                extern int gePortNavNearest(float x, float z);
                extern int gePortNavRoute(int from, int to, int *out, int max);
                int route[64];
                int from = gePortNavNearest(st0x, st0z);
                int far = -1, k;
                float fx[3], bestd = -1.0f, p0[3];

                if (from >= 0 && gePortNavAt(from, NULL, p0, NULL)) {
                    for (k = 0; k < n; k++) {
                        if (gePortNavAt(k, NULL, fx, NULL)) {
                            float dd = (fx[0]-p0[0])*(fx[0]-p0[0]) + (fx[2]-p0[2])*(fx[2]-p0[2]);
                            if (dd > bestd) { bestd = dd; far = k; }
                        }
                    }
                }
                if (from >= 0 && far >= 0) {
                    int got = gePortNavRoute(from, far, route, 64);
                    printf("[getv][nav] route %d -> %d: %d hop(s)\n", from, far, got);
                    if (got > 0) {
                        int j;
                        printf("[getv][nav]   ");
                        for (j = 0; j < got && j < 20; j++) { printf("%d ", route[j]); }
                        printf("%s\n", got > 20 ? "..." : "");
                    }
                }
            }
        }
        fflush(stdout);
    }
    ge_br_use_clear = (getenv("GETV_BOT_CLEAR") != NULL);

    if (!geWorldLoad(e)) {
        printf("[getv][botroute] no world data for '%s' -- run tools/pack_world.py\n", e);
        ge_br_slot = -1;
        return;
    }

    /* Pick the objective: the requested one, else the first that actually has a route. Several
     * objectives have no tagged target and so no route at all; walking to objective 0 blindly
     * would stand still and look broken. */
    ge_br_obj = -1;
    e = getenv("GETV_BOT_ROUTE_OBJ");
    if (e != NULL && *e != '\0') {
        GeWorldObjective ob;
        int want = atoi(e);
        if (geWorldObjective(want, &ob) && ob.steps > 0) { ge_br_obj = want; }
        else {
            printf("[getv][botroute] objective %d has no route; falling back to the first that "
                   "does\n", want);
        }
    }
    if (ge_br_obj < 0) {
        int i;
        for (i = 0; i < geWorldObjectiveCount(); i++) {
            GeWorldObjective ob;
            if (geWorldObjective(i, &ob) && ob.steps > 0) { ge_br_obj = i; break; }
        }
    }
    if (ge_br_obj < 0) {
        printf("[getv][botroute] %s has no routable objective\n", geWorldLevel());
        ge_br_slot = -1;
        return;
    }

    {
        GeWorldObjective ob;
        geWorldObjective(ge_br_obj, &ob);
        ge_br_steps = ob.steps;
        printf("[getv][botroute] slot %d walking objective %d of %s: %d steps to (%.0f %.0f %.0f)\n",
               ge_br_slot, ge_br_obj, geWorldLevel(), ob.steps,
               (double) ob.tx, (double) ob.ty, (double) ob.tz);
    }
    gePlayerApiInit();
    gePlayerClaim(ge_br_slot, GE_SLOT_INJECTED);
    fflush(stdout);
}

/* Once per rendered frame, beside the other bot policies. */
void gePortBotRouteFrame(int frame)
{
    GePlayerState st;
    GeWorldStep step;
    GeWorldWaypoint wp;
    GePlayerInput in;
    float dx, dz, dist, bearing, err, align;

    if (!ge_br_ready) { gePortBotRouteInit(); }
    if (ge_br_slot < 0) { return; }
    if (!gePlayerStateGet(ge_br_slot, &st) || !(st.fields & GE_ST_POSITION)) { return; }

    /* Heading, real when the game reports one and dead-reckoned when it does not.
     *
     * The estimator below was the only source until gePortPlayerAngle landed, and on its own it
     * deadlocks: a bot needing more than GE_BR_ALIGN_DEG of turn walks at zero forward speed, so
     * it does not move, so the estimate never updates and the turn never finishes. Bunker 1 held
     * err at 142 degrees for 1100 frames that way. The estimator is kept as the fallback for a
     * slot the accessor refuses -- a player with no collision record yet, mid-spawn. */
    if (st.fields & GE_ST_ANGLE) {
        ge_br_heading = st.angle;
        ge_br_have_heading = 1;
    } else if (ge_br_have_prev) {
        float mx = st.x - ge_br_px, mz = st.z - ge_br_pz;
        if (mx * mx + mz * mz > GE_BR_MOVE_EPSILON * GE_BR_MOVE_EPSILON) {
            ge_br_heading = (float) (atan2((double) mx, (double) mz) * 180.0 / 3.14159265358979);
            ge_br_have_heading = 1;
        }
    }
    ge_br_px = st.x; ge_br_py = st.y; ge_br_pz = st.z;
    ge_br_have_prev = 1;

    if (ge_br_step >= ge_br_steps) {
        if (ge_br_trace && (frame % 60) == 0) {
            printf("[getv][botroute] route complete at (%.0f %.0f %.0f)\n",
                   (double) st.x, (double) st.y, (double) st.z);
            fflush(stdout);
        }
        return;
    }
    /* GETV_BOT_ROUTE_NEAREST=1 -- walk to the nearest waypoint instead of following a route.
     *
     * A diagnostic, and a separable one. "The bot never arrives" has two possible
     * causes and they need different fixes: the movement stack cannot get anywhere, or the routes
     * point somewhere it cannot go. Following a route tests both at once and tells you nothing
     * about which. This tests only the first. */
    {
        static int nearest = -1;
        if (nearest < 0) { const char *e = getenv("GETV_BOT_ROUTE_NEAREST"); nearest = (e && *e && *e != '0'); }
        if (nearest) {
            /* LATCHED. Re-picking the nearest waypoint every tick means the target changes as the
             * bot moves, so it chases a moving goal and converges on nothing -- and the
             * skip-if-already-close test then discards the very waypoint it just reached. Choose
             * once, walk to that one, and say so when it arrives. */
            static int have_target = 0;
            static GeWorldWaypoint target;

            if (!have_target) {
                int i, found = 0;
                float bd = 0.0f;
                for (i = 0; i < geWorldWaypointCount(); i++) {
                    GeWorldWaypoint w;
                    float dx2, dz2, d2;
                    if (!geWorldWaypoint(i, &w)) { continue; }
                    dx2 = w.x - st.x;
                    dz2 = w.z - st.z;
                    d2 = (dx2 * dx2) + (dz2 * dz2);
                    /* Not the one being stood on: arriving before starting proves nothing. */
                    if (d2 < (GE_BR_ARRIVE * GE_BR_ARRIVE)) { continue; }

                    /* on THIS FLOOR. The nearest node in plan view is regularly on another
                     * storey -- Train's nearest was a doorway 308 units BELOW the carriage the
                     * bot stands in -- and walking at it just presses the bot against the edge of
                     * its own deck. Compared floor to floor via the stan query rather than pad y
                     * to body y, which differ by about 157 and would make every candidate look
                     * like a cliff. */
                    {
                        extern int gePortProbeStandable(float x, float y, float z, float radius,
                                                        float *out_y, int *out_room);
                        float my_floor = st.y, its_floor = w.y;
                        if (!gePortProbeStandable(st.x, st.y, st.z, 60.0f, &my_floor, NULL)) { continue; }
                        if (!gePortProbeStandable(w.x, w.y, w.z, 60.0f, &its_floor, NULL)) { continue; }
                        if ((its_floor - my_floor) > GE_BR_MAX_STEP
                            || (my_floor - its_floor) > GE_BR_MAX_DROP) { continue; }
                    }
                    if (!found || d2 < bd) { bd = d2; target = w; found = 1; }
                }
                if (found) {
                    have_target = 1;
                    if (ge_br_trace) {
                        printf("[getv][botroute] nearest-target test: waypoint %d at %.0f units\n",
                               target.id, (double) sqrt((double) bd));
                        fflush(stdout);
                    }
                }
            }
            if (have_target) {
                wp = target;
                dx = wp.x - st.x;
                dz = wp.z - st.z;
                dist = (float) sqrt((double) ((dx * dx) + (dz * dz)));
                if (dist <= GE_BR_ARRIVE) {
                    printf("[getv][botroute] REACHED waypoint %d at %.0f units\n", wp.id, (double) dist);
                    fflush(stdout);
                    return;
                }
                goto steer;
            }
        }
    }

    if (ge_br_use_nav && ge_br_nav_len == 0) {
        /* Built on the first tick rather than at init: the route starts from where the bot is,
         * and at init it has no position yet -- player.c zeroes it until the first spawn. */
        ge_br_nav_build();
    }

    if (ge_br_use_nav) {
        /* The engine route is a plain chain of waypoints, so a "step" is just this hop and the
         * next. No join phase: the route already starts at the waypoint nearest the bot. */
        float here[3], there[3];
        if (ge_br_step + 1 >= ge_br_nav_len) { return; }
        if (!gePortNavAt(ge_br_nav[ge_br_step], NULL, here, NULL)) { return; }
        if (!gePortNavAt(ge_br_nav[ge_br_step + 1], NULL, there, NULL)) { return; }
        ge_br_joined = 1;
        step.from = ge_br_nav[ge_br_step];
        step.to   = ge_br_nav[ge_br_step + 1];
        step.threats = 0;
        wp.id = step.to;
        wp.x = there[0]; wp.y = there[1]; wp.z = there[2];
        /* FOUND LIVE, THE HARD WAY: this branch never set wp.room, and wp is a plain
         * uninitialised local (line 822) reused call after call. The room-crossing check
         * added above reads wp.room to decide whether to steer at a portal -- with garbage
         * in it, that decision flapped on effectively every tick, and the 40-unit hysteresis
         * gate in gePortNavTick treats every flap as a brand new target, resetting the whole
         * state machine before it can move at all. Measured: the bot never left the spawn
         * room, 10634 lines, alternating between two targets 2400 units apart. gePortTileAt
         * is the same stan-lookup ge_cli.c already uses for this exact purpose. */
        if (!gePortTileAt(wp.x, wp.z, NULL, &wp.room)) { wp.room = GE_WORLD_NO_ROOM; }
        goto have_target;
    }

    if (!geWorldRouteStep(ge_br_obj, ge_br_step, &step)) { return; }

    /* Join the route before walking it.
     *
     * The bot does not spawn on the graph -- it lands hundreds of units off the nearest node
     * (Silo 40, Bunker 1 583, Caverns 3,390). Heading straight for step 0's destination crosses
     * whatever lies between on no edge at all, which is a straight line through walls.
     *
     * So the first target is step 0's ORIGIN. Reaching it puts the bot on the graph, and every
     * target after that is one walkable edge away. First step only: afterwards a step's origin
     * is the previous step's destination, so aiming at it again is a wasted round trip. */
    if (ge_br_step == 0 && !ge_br_joined) {
        int i, found = 0;
        for (i = 0; i < geWorldWaypointCount(); i++) {
            if (geWorldWaypoint(i, &wp) && wp.id == step.from) { found = 1; break; }
        }
        if (found) {
            float jx = wp.x - st.x, jz = wp.z - st.z;
            if ((jx * jx + jz * jz) <= (GE_BR_ARRIVE * GE_BR_ARRIVE)) {
                ge_br_joined = 1;
                if (ge_br_trace) {
                    printf("[getv][botroute] joined the route at node %d\n", step.from);
                    fflush(stdout);
                }
            }
        } else {
            /* No origin node in the table means the route and the waypoint set disagree, and
             * walking the chain anyway would be walking a chain that is not there. */
            ge_br_joined = 1;
        }
    }

    /* The step names waypoints; the position comes from the waypoint table. */
    {
        int i, found = 0;
        int want = (ge_br_step == 0 && !ge_br_joined) ? step.from : step.to;
        ge_br_last_target = want;   /* so the log rows below can name where it was heading */
        for (i = 0; i < geWorldWaypointCount(); i++) {
            if (geWorldWaypoint(i, &wp) && wp.id == want) { found = 1; break; }
        }
        if (!found) { return; }
    }

have_target:

    /* Is anyone else heading for this waypoint? With no enemy source installed geEnemyThreatAt
     * returns 0 and the bot behaves exactly as before, so the policy is inert rather than wrong
     * on a build whose game-side shim has not landed. */
    {
        int threat = geEnemyThreatAt(wp.x, wp.y, wp.z, GE_BR_THREAT_RADIUS);
        if (!ge_br_should_advance(threat, ge_br_held)) {
            ge_br_held++;
            if (ge_br_trace && (ge_br_held % 30) == 1) {
                printf("[getv][botroute] holding: waypoint %d contested by %d (%d/%d frames)\n",
                       step.to, threat, ge_br_held, GE_BR_MAX_HOLD);
                fflush(stdout);
            }
            /* Post neutral rather than posting nothing. A slot that goes quiet falls back to
             * whatever was held, and the bot would keep walking while believing it had stopped. */
            memset(&in, 0, sizeof in);
            gePlayerPost(ge_br_slot, gePlayerTick() + 1, &in, 1);
            return;
        }
        if (ge_br_held > 0 && ge_br_trace) {
            printf("[getv][botroute] advancing on waypoint %d after %d frames held\n",
                   step.to, ge_br_held);
            fflush(stdout);
        }
        ge_br_held = 0;
    }

    /* The pad is where the route says to go; it is not always ground a body can walk. See
     * ge_route_lane.c -- on Train the pads run 70-odd units off the walkable centre. Inert
     * unless GETV_ROUTE_LANE is set. */
    {
        extern float gePortRouteLane(float px, float pz, float *tx, float *tz);
        float ax = wp.x, az = wp.z;
        if (gePortRouteLane(st.x, st.z, &ax, &az) != 0.0f) {
            wp.x = ax;
            wp.z = az;
        }
    }
    dx = wp.x - st.x;
    dz = wp.z - st.z;
    dist = (float) sqrt((double) (dx * dx + dz * dz));

    if (dist <= GE_BR_ARRIVE) {
        /* Arriving at the JOIN target means the bot is on the graph, not that it has finished
         * step 0. Advancing here would silently skip the first edge of every route. */
        if (ge_br_step == 0 && !ge_br_joined) {
            ge_br_joined = 1;
            if (ge_br_trace) {
                printf("[getv][botroute] joined the route at node %d\n", step.from);
                fflush(stdout);
            }
            return;
        }
        ge_br_step++;
        ge_br_turn_sign = 0.0f;   /* new target, new decision */
        ge_br_logf((int) frame, "arrive", step.to, gePortNavNearestPad(st.x, st.z),
                   st.x, st.z, ge_br_heading, 0.0f, 0.0f, 0, 0, "");
        if (ge_br_trace) {
            printf("[getv][botroute] reached waypoint %d (step %d/%d)\n",
                   step.to, ge_br_step, ge_br_steps);
            fflush(stdout);
        }
        return;
    }

    /* ROUTE ON ROOMS FOR STEERING ONLY -- NEVER FOR ARRIVAL.
     *
     * dist and the ARRIVE check above already ran against the real route target, so step
     * advancement is untouched by any of this. What follows only changes what gePortNavTick
     * below is asked to walk toward, for exactly the reason the Python CLI driver stopped
     * fighting Train's first carriage: the route waypoint can sit in a room the bot is not
     * currently in, and a straight line to it then points through whatever is between -- a
     * wall, a crate row the pad graph has never heard of, or the carriage divider itself.
     * geWorldNextPortal finds the doorway to aim at instead, using the same portal graph
     * tools/ge_rooms.py already proved on this exact case: Train room 5 to room 7 goes via
     * room 6, portal at (-1431 -196), verified against this function bit for bit before it
     * was wired in here.
     *
     * Deliberately NOT substituted into wp itself, and deliberately not given its own arrival
     * radius -- both were the shape of the bug that cost an earlier attempt at this. Once the
     * bot's own room changes, geWorldNextPortal naturally stops returning the same portal (the
     * BFS is asked fresh every tick, it is cheap enough), so there is nothing here to fall out of
     * sync when the room updates.
     *
     * FOUND LIVE: this used to assign dx/dz here and stop. gePortNavTick a few lines down then
     * ran unconditionally against wp.x/wp.z, overwriting dx/dz regardless of room -- so the
     * portal point was computed, printed, and never actually steered toward. The nav state
     * machine was handed a target on the other side of a wall with no way to know the wall was
     * there, and its skirt/tangent search read the wall as an obstacle it could approach but
     * never round: exactly the frozen-after-a-1-2-unit-creep signature measured at Train
     * (-1454 -236), room 6 -> 1, 11347 of 11419 samples in. nav_tx/nav_tz is what actually
     * reaches gePortNavTick now. */
    {
        float nav_tx = wp.x, nav_tz = wp.z;
        if (wp.room != GE_WORLD_NO_ROOM && st.room >= 0 && st.room != wp.room) {
            extern int geWorldNextPortal(int fromRoom, int goalRoom, float *out_x, float *out_z);
            float px, pz;
            if (geWorldNextPortal(st.room, wp.room, &px, &pz)) {
                nav_tx = px;
                nav_tz = pz;
                if (ge_br_trace && (frame % 60) == 0) {
                    printf("[getv][botroute] room %d -> %d: steering at the portal (%.0f %.0f)"
                           " instead of the route target\n", st.room, wp.room, (double) px, (double) pz);
                    fflush(stdout);
                }
            }
        }
        ge_br_nav_target_x = nav_tx;
        ge_br_nav_target_z = nav_tz;
    }
    /* Extending this to the objective target's room (mirroring the Python CLI driver's actual
     * breakthrough) was tried and reverted: it regressed to 6.1m against this version's 13.4m,
     * and even then the routing line never fired. Something about calling
     * ge_br_pick_objective_target from two places in the same frame broke more than it was meant
     * to fix, and it was not diagnosed before time ran out -- flagged rather than shipped. */

    memset(&in, 0, sizeof in);
    if (!ge_br_have_heading) {
        /* No heading yet: walk forward to make one. Steering on an unknown heading turns the
         * bot in a random direction and then estimates from that, which converges eventually
         * but looks like a drunk and wastes the first seconds of a run. */
        in.stick_y = (signed char) GE_BR_WALK;
        gePlayerPost(ge_br_slot, gePlayerTick() + 1, &in, 1);
        return;
    }

    /* ONE NAVIGATION DECISION, not five that can disagree.
     *
     * This used to be a hard-coded no-go zone (one bad point, found and blocked by hand), a
     * sensor-triggered avoidance hold, and a stuck/detour timer with its own escalation -- three
     * independently triggered systems, each added to fix one measured symptom, each able to
     * overwrite the others' steering in the same frame. Watched live: the no-go zone's repulsor
     * and the objective standoff's attractor were caught pulling opposite ways in the same spot,
     * neither winning, while a guard shot the bot standing still between them.
     *
     * gePortNavTick (ge_bot_nav.c) replaces all three. It is a faithful, simplified port of
     * Perfect Dark's chrNavTickMain -- the state machine guards themselves run, which does not
     * have this failure mode by construction: one mode variable, one branch runs per tick, and
     * its own retry/give-up counters (five ticks per attempt, matching the source) are what the
     * stuck timer was reaching for by hand. */
    {
        extern void gePortNavTick(GeNavState *nav, float px, float pz, float target_x,
                                  float target_z, float *out_aim_x, float *out_aim_z);
        static GeNavState nav;
        float aim_x, aim_z;

        gePortNavTick(&nav, st.x, st.z, ge_br_nav_target_x, ge_br_nav_target_z, &aim_x, &aim_z);
        dx = aim_x - st.x;
        dz = aim_z - st.z;
        ge_br_nav_aim_x = aim_x;
        ge_br_nav_aim_z = aim_z;
    }

steer:
    bearing = (float) (atan2((double) dx, (double) dz) * 180.0 / 3.14159265358979);
    err = ge_br_norm180(bearing - ge_br_heading);

    {
        /* Negated: the stick and the heading run in opposite senses.
         *
         * bondview2.c:7312 integrates vv_theta += speedtheta * delta * 3.5 with the sign the pad
         * gives it, so a positive stick DECREASES the heading atan2(x, z) reports. Steering with
         * err * gain drives the bot away from its target.
         *
         * It presents as a freeze rather than a wrong turn: the bot rotates to the antipode,
         * where normalised error flips sign on the smallest change, and the stick then alternates
         * hard left and right with no net rotation.
         *
         * GETV_BOT_ROUTE_SIGN=1 restores the old sense for an A/B. */
        static int sign = 0;
        float sx;
        float mag = (float) fabs((double) err);

        if (sign == 0) {
            const char *e = getenv("GETV_BOT_ROUTE_SIGN");
            sign = (e && *e && *e != '0') ? 1 : -1;
        }

        /* Latch the turn direction until the error is materially reduced.
         *
         * Near +/-180 the normalised error flips sign on the smallest heading change, so a policy
         * that re-derives its turn each tick argues with itself and the stick alternates hard
         * left and right while the distance grows.
         *
         * The same instability was patched five times in five separate branches before landing
         * here, once, where the turn is decided.
         *
         * Released on progress rather than on a timer -- when the error drops by a third or falls
         * inside the alignment cone. A timer releases mid-swing and hands the oscillation back. */
        if (ge_br_turn_sign != 0.0f) {
            if (mag < GE_BR_ALIGN_DEG || mag < ge_br_turn_from * 0.66f) {
                ge_br_turn_sign = 0.0f;
            }
        }
        if (ge_br_turn_sign == 0.0f && mag > GE_BR_ALIGN_DEG) {
            ge_br_turn_sign = (err < 0.0f) ? -1.0f : 1.0f;
            ge_br_turn_from = mag;
        }

        /* Committed: keep turning the way we chose, at the magnitude the error asks for. Only the
         * SIGN is held -- holding the magnitude too would make the bot overshoot a target it is
         * nearly lined up with. */
        sx = (float) sign * err * GE_BR_TURN_GAIN;
        if (ge_br_turn_sign != 0.0f) {
            float held = (float) sign * ge_br_turn_sign * mag * GE_BR_TURN_GAIN;
            sx = held;
        }
        if (sx >  GE_BR_STICK_MAX) { sx =  GE_BR_STICK_MAX; }
        if (sx < -GE_BR_STICK_MAX) { sx = -GE_BR_STICK_MAX; }
        in.stick_x = (signed char) sx;
        ge_br_logf((int) frame, "steer", ge_br_last_target, gePortNavNearestPad(st.x, st.z),
                   st.x, st.z, ge_br_heading, bearing, err, (int) sx, 0,
                   ge_br_turn_sign != 0.0f ? "latched" : "free");
    }
    align = 1.0f - (float) fabs((double) err) / GE_BR_ALIGN_DEG;
    if (align < 0.0f) { align = 0.0f; }
    in.stick_y = (signed char) (GE_BR_WALK * align);

    /* Look before walking.
     *
     * Everything below this used to be reactive: walk at the waypoint, notice after thirty ticks
     * that nothing moved, then guess. That is why the bot walked into a crate, turned, and walked
     * into a wall -- it could not see either until it was already against them, and "blocked" told
     * it nothing about which.
     *
     * The sensing API answers both questions before the step is taken, and the response differs
     * by what is there. A door is not an obstacle to something with a hand; a crate is not a wall
     * you turn away from if there is a gap beside it; a body will move on its own.
     */
    /* Ask the door, the way the guards do -- on A timer, not on A sensor verdict.
     *
     * First attempt put this inside the branch that fires when the sensor says DOOR, and it never
     * ran once in a whole Train run: at the place the bot actually stops, the sensor says OBJECT.
     * The guards do not wait to be told there is a door either. chraction.c:9202 sweeps for one
     * every tenth tick no matter what it thinks is ahead, and lets the sweep answer.
     *
     * Aimed at gePortNavTick's own current aim point, not a synthetic projection along the
     * heading -- PD's own version (chrOpenDoor, called from chrNavTickMain) sweeps toward
     * waydata->aimposobj, the state machine's actual target, for the same reason: a bot wedged
     * against a crate is facing the crate, and the door is on the way to wherever the nav state
     * machine has decided to go around it, which is not necessarily where the body is pointed.
     *
     * Reports LOCKED, and does not open it. A locked door is not an obstacle to be steered
     * around -- it is an errand: a guard is carrying the key and has to be dealt with first.
     */
    if ((frame % 10) == 0) {
        int r = gePortOpenDoorAhead(ge_br_slot, ge_br_nav_aim_x, ge_br_nav_aim_z);

        if (r == -2 && ge_br_locked_seen == 0) {
            ge_br_locked_seen = 1;
            printf("[getv][botroute] LOCKED DOOR at (%.0f %.0f) -- the bot is not carrying its "
                   "key. A guard is holding it.\n", (double) st.x, (double) st.z);
            fflush(stdout);
        }
        if (r == 1 || r == -2) {
            ge_br_logf((int) frame, "door", ge_br_last_target, gePortNavNearestPad(st.x, st.z),
                       st.x, st.z, ge_br_heading, bearing, 0.0f, 0, 0,
                       r == 1 ? "opened" : "LOCKED");
        }
    }

    /* Ground truth, logged where the input actually leaves.
     *
     * The steer row above records what the steering block WANTED, and several branches below it
     * overwrite stick_x afterwards -- so reading the steer rows alone tells you the bot asked for
     * a left turn at a moment it in fact sent a right one. This row is the input as posted. When
     * the two disagree, the disagreement is the bug. */
    {
        /* Health in the row, because a stalled bot and a dead one look identical from the input
         * side: both post a stick that changes nothing. */
        float hp = -1.0f, armour = 0.0f;
        int dead = 0;
        char note[40];
        gePortPlayerHealth(ge_br_slot, &hp, &armour, &dead);
        snprintf(note, sizeof note, "%shp=%.2f%s",
                 (in.buttons & GE_IN_USE) ? "use," : "", (double) hp, dead ? ",DEAD" : "");
        ge_br_logf((int) frame, "post", ge_br_last_target, gePortNavNearestPad(st.x, st.z),
                   st.x, st.z, ge_br_heading, 0.0f, 0.0f,
                   (int) in.stick_x, (int) in.stick_y, note);
    }

    /* DESTROY THE OBJECTIVE. Unconditional, and ordered before the guard-combat arbiter: a
     * brake unit does not shoot back, so there is no survival case where ignoring it is correct.
     * Only overrides steering within engagement range -- outside that, routing (with its portal
     * and skirt navigation) is what gets the bot there in the first place. */
    {
        float tx, tz, trad = 0.0f;
        int tag = -1;

        if (ge_br_pick_objective_target(st.x, st.z, &tx, &tz, &tag, NULL, &trad)) {
            float dx = tx - st.x, dz = tz - st.z;
            float td = sqrtf(dx * dx + dz * dz);
            /* The prop's OWN radius plus a body's worth of clearance -- not a re-derived
             * geometric guess. This is what a screenshot showed directly: crouched against the
             * brake's own housing, the door it needed next on a different pad entirely, "skirt
             * obstacle found but neither side is clear" because the tangent-point construction
             * assumes approaching an obstacle from outside it, not standing inside its own
             * collision disc. Getting closer than this is not progress, it is wedging in. */
            float standoff = trad + GE_BR_RADIUS * 1.5f;   /* GE_BR_RADIUS is this file's own body-radius constant */

            if (tag != ge_br_obj_tag) {
                ge_br_obj_tag = tag;
                ge_br_obj_aimed = 0;
                ge_br_obj_shots = 0;
            }

            if (td < standoff) {
                /* BACK STRAIGHT OUT. Known centre, known radius -- walk directly away from it
                 * until clear, no sensor, no skirt, no re-derivation. This is the one case that
                 * construction cannot answer (you cannot round the corner of a thing you are
                 * standing inside), so do not ask it to. */
                float bb = (float) (atan2((double) -dx, (double) -dz) * 180.0 / 3.14159265358979);
                float be = ge_br_norm180(bb - ge_br_heading);
                float sxb = (float) ge_br_sign_of() * be * GE_BR_TURN_GAIN;
                if (sxb >  GE_BR_STICK_MAX) { sxb =  GE_BR_STICK_MAX; }
                if (sxb < -GE_BR_STICK_MAX) { sxb = -GE_BR_STICK_MAX; }
                memset(&in, 0, sizeof in);
                in.stick_x = (signed char) sxb;
                in.stick_y = (signed char) (GE_BR_WALK * (1.0f - (float) fabs((double) be) / 180.0f));
                if (ge_br_trace && (frame % 30) == 0) {
                    printf("[getv][botroute] %.0fu from tag %d (radius %.0f, standoff %.0f) --"
                           " backing out, not routing around\n",
                           (double) td, tag, (double) trad, (double) standoff);
                    fflush(stdout);
                }
            } else if (td < 220.0f) {
                float tb = (float) (atan2((double) dx, (double) dz) * 180.0 / 3.14159265358979);
                float err2 = ge_br_norm180(tb - ge_br_heading);

                if (fabs((double) err2) > 12.0) {
                    float sx4 = (float) ge_br_sign_of() * err2 * GE_BR_TURN_GAIN;
                    if (sx4 >  GE_BR_STICK_MAX) { sx4 =  GE_BR_STICK_MAX; }
                    if (sx4 < -GE_BR_STICK_MAX) { sx4 = -GE_BR_STICK_MAX; }
                    memset(&in, 0, sizeof in);
                    in.stick_x = (signed char) sx4;
                } else if (ge_br_obj_shots < 60) {
                    /* AIM MODE, STICK DOWN, NOT THE LOOK BUTTON. Measured: with GE_IN_LOOK_DOWN
                     * inside aim mode the object's own damage counter stays at 0 across a whole
                     * run; with stick_y held down under GE_IN_AIM it climbs to 750 and past its
                     * 1000 threshold. The look axis and the aim axis are not the same control. */
                    memset(&in, 0, sizeof in);
                    in.buttons = GE_IN_AIM | GE_IN_FIRE;
                    in.stick_y = -80;
                    ge_br_obj_aimed = 1;
                    ge_br_obj_shots++;
                } else if (ge_br_obj_aimed) {
                    /* Bounded, and stand back up once done. A destroyed target drops out of the
                     * picker on its own next frame; this cap exists so a report that never flips
                     * DESTROYED for some other reason cannot pin the bot crouched at one spot
                     * forever. */
                    memset(&in, 0, sizeof in);
                    in.buttons = GE_IN_CROUCH_UP;
                    ge_br_obj_aimed = 0;
                }
                if (ge_br_trace && (frame % 30) == 0) {
                    printf("[getv][botroute] objective tag %d, %.0fu away, %d shot(s)\n",
                           tag, (double) td, ge_br_obj_shots);
                    fflush(stdout);
                }
            }
        } else {
            ge_br_obj_tag = -1;
        }
    }

    /* the arbiter owns this decision. geBotArbitrate was derived from exactly the three
     * measurements in the comment above, and its job is to decide which of the route's heading
     * and the fight's heading gets to stand this frame -- see ge_bot_arbiter.c for the full
     * derivation and ge_bot_arbiter.h for what it deliberately does not model (the lock-on settle
     * timer, chiefly).
     *
     * A deliberate asymmetry: the arbiter is consulted, but it is never allowed to be the only
     * thing that decided a heading. In the common case (GE_ARB_ROUTING or GE_ARB_FIRING_ON_ROUTE)
     * this block does not touch in.stick_x or in.stick_y at all -- whatever the steering and
     * avoidance blocks above already decided stands untouched, latch and all. That is the whole
     * point: routing is bit-for-bit what it would have been with combat off, in a corridor, which
     * is most of the level, because the route and the guards point the same way. */
    if (ge_br_fight) {
        GeEnemy tgt;
        int idx = ge_br_pick_target(st.x, st.y, st.z, &tgt);

        if (idx >= 0) {
            float dx = tgt.x - st.x;
            float dz = tgt.z - st.z;
            float tb = (float) (atan2((double) dx, (double) dz) * 180.0 / 3.14159265358979);
            float tdist = sqrtf(dx * dx + dz * dz);
            float hp = -1.0f, armour = 0.0f;
            int dead = 0;
            GeBotSituation sit;
            GeBotAction act;
            const char *why;

            gePortPlayerHealth(ge_br_slot, &hp, &armour, &dead);

            sit.route_heading = ge_br_heading;   /* the arbiter's err is measured off CURRENT
                                                   * facing, not the route's target bearing --
                                                   * what matters for the cone is where the gun
                                                   * is actually pointed right now. */
            sit.has_target    = 1;
            sit.target_bearing = tb;
            /* Lateral offset via a straight rotation of (dx,dz) into the bot's own frame, NOT via
             * sin() of the wrapped bearing error. Both would look equivalent at first glance, but
             * sin(wrap180(x)) still carries the wrap's own sign flip right at the seam, which is
             * exactly the instability target_lateral exists to avoid -- see the field's doc
             * comment in ge_bot_arbiter.h. This is pure geometry: no angle difference, no modulo,
             * so there is nothing left in the computation that CAN flip discontinuously. */
            {
                double h = (double) ge_br_heading * 3.14159265358979 / 180.0;
                sit.target_lateral = (float) ((double) dx * cos(h) - (double) dz * sin(h));
            }
            sit.distance = tdist;
            sit.health   = (hp >= 0.0f) ? hp : 1.0f;
            /* Always 1, not a stand-in: ge_br_pick_target already required geSenseVisibleTo on
             * every path that can return idx>=0 (both the "stay on target" branch and the
             * fresh-search branch), so a target reaching here has ALREADY passed the same line
             * test the real auto-aim runs before it locks. Re-testing would just repeat a check
             * already made, and "always 1" documents that rather than hiding it behind a second
             * call that would silently always succeed. */
            sit.has_los = 1;

            /* GETV_BOT_AIM forces the cornered response regardless of range, reproducing the OLD
             * stop-and-face behaviour for a direct A/B against the arbiter's default. Done by
             * lying about distance rather than duplicating the cornered branch's logic -- the
             * arbiter is the one place that logic should exist. */
            if (ge_br_aim) { sit.distance = 0.0f; }

            act = geBotArbitrate(&sit);

            if (act.reason == GE_ARB_CORNERED || act.reason == GE_ARB_SURVIVING) {
                /* The fight owns the heading this frame. Turn toward act.heading with the same
                 * proportional law and sign convention the route steering uses, so a trace
                 * comparing the two blocks is comparing like with like. No separate latch is
                 * needed here: act.heading was built inside the arbiter from target_lateral's
                 * SIGN, which does not flip at the wrap seam, so recomputing it fresh every frame
                 * converges instead of oscillating -- that is what the 12-step boundary-crossing
                 * check in test_bot_arbiter.c verifies offline. */
                float turn = ge_br_norm180(act.heading - ge_br_heading);
                float sx = (float) ge_br_sign_of() * turn * GE_BR_TURN_GAIN;
                if (sx >  GE_BR_STICK_MAX) { sx =  GE_BR_STICK_MAX; }
                if (sx < -GE_BR_STICK_MAX) { sx = -GE_BR_STICK_MAX; }
                in.stick_x = (signed char) sx;
                in.buttons &= ~GE_IN_USE;
            }
            if (!act.advance) {
                /* Never advance on a heading the fight owns -- this is the specific rule that
                 * turned "fire on the move" from waypoint-4-and-dead into a survivable branch:
                 * advancing along an aim-owned heading walks straight at the guard.
                 *
                 * act.retreat: walk BACKWARD along that same aim-owned heading instead of
                 * holding. The heading still points at the target (act.heading, applied above),
                 * so a negative stick_y is a straight retreat, not a turn -- no separate lateral
                 * input needed, and no interaction with the single-controller strafe/turn
                 * coupling ge_bot_arbiter.h's header comment documents for the sideways case.
                 * No clearance check on the space behind the bot: advancing toward the route
                 * target does not get one either, and gePortNavTick's own obstacle handling
                 * takes back over the instant this branch is not active. */
                in.stick_y = act.retreat ? (signed char) -GE_BR_WALK : 0;
            }

            if (act.fire) {
                if (ge_br_burst <= 0) { ge_br_burst = GE_BR_BURST_ON + GE_BR_BURST_OFF; }
                if (ge_br_burst > GE_BR_BURST_OFF) { in.buttons |= GE_IN_FIRE; }
                ge_br_burst--;
            } else {
                ge_br_burst = 0;
            }

            switch (act.reason) {
                case GE_ARB_FIRING_ON_ROUTE: why = "fire-on-route"; break;
                case GE_ARB_CORNERED:        why = "cornered";      break;
                case GE_ARB_SURVIVING:       why = "surviving";     break;
                default:                     why = "routing";       break;
            }
            ge_br_logf((int) frame, "engage", tgt.id, gePortNavNearestPad(st.x, st.z),
                       st.x, st.z, ge_br_heading, tb, ge_br_norm180(tb - ge_br_heading),
                       (int) in.stick_x, (int) in.stick_y, why);
        } else {
            /* No target: nothing to lose lock on and nothing to fire at. Reset the burst here
             * too, not only on the in-cone/out-of-cone branch above -- a target that DISAPPEARS
             * mid-burst (killed, or line broken) used to leave ge_br_burst counting down toward a
             * stale ON phase that would resume the instant a new target appeared, borrowing timing
             * from an engagement that already ended. */
            ge_br_burst = 0;
        }
    }

    /* Post for the NEXT tick: the playback handler has already run for this frame, so posting
     * "now" is posting into the past and gePlayerPost correctly refuses it. */
    gePlayerPost(ge_br_slot, gePlayerTick() + 1, &in, 1);

    if (ge_br_trace && (frame % 60) == 0) {
        printf("[getv][botroute] step %d/%d -> wp %d  dist=%.0f heading=%.0f bearing=%.0f "
               "err=%.0f stick=(%d,%d) threats=%d\n",
               ge_br_step, ge_br_steps, step.to, (double) dist, (double) ge_br_heading,
               (double) bearing, (double) err, in.stick_x, in.stick_y, step.threats);
        fflush(stdout);
    }
}
