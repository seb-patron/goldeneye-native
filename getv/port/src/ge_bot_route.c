/* A bot that walks a real route, using the world API for knowledge and the player API to act.
 *
 * This is what the two seams were built for. ge_world_api says where the objective is and which
 * waypoints lead to it; ge_player_api posts the stick input that gets there. Nothing here knows
 * anything about GoldenEye's internals -- it reads knowledge, reads state, posts input -- which
 * is the same shape a network peer or a learning agent has.
 *
 * THE STEERING LAW IS THE ONE VALIDATED IN tools/routesim.py, constants included.
 *
 * Turn and walk at once, with forward speed scaled DOWN by heading error. That scaling is
 * load-bearing rather than a refinement: turning radius is speed over turn rate, about 114 units
 * at full pelt, so a bot that walks flat out while turning cannot get inside a 120-unit arrival
 * radius and orbits its own waypoint instead. The model measures that as 29 of 61 routes failed
 * with the scaling removed. Do not "simplify" it away.
 *
 * HEADING COMES FROM THE GAME.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_player_api.h"
#include "ge_world_api.h"
#include "ge_enemy_api.h"
#include "ge_world_levels.h"    /* generated: stage number -> extractor level name */

/* Straight from tools/routesim.py. Changing one of these without re-running the model is how a
 * validated law quietly becomes an unvalidated one. */
#define GE_BR_WALK          60.0f   /* N64 counts; the walk deadzone subtracts about 5 */
#define GE_BR_ARRIVE       120.0f   /* world units; must stay outside the turning circle */
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
 * Deliberately wider than the arrival radius: the question is not "is a guard standing on it"
 * but "is anyone heading there", and a belief is a destination. (From the Surface.) */
#define GE_BR_THREAT_RADIUS 300.0f

/* Cap on how long the bot waits for a contested waypoint to clear, in frames. There must be a
 * cap: holding until a route clears sounds prudent and produces a bot that stands still forever,
 * because nothing about waiting makes a guard change its mind -- and a frozen bot is
 * indistinguishable from a crashed one. Three seconds, then commit. (From the Surface.) */
#define GE_BR_MAX_HOLD 180

#define GE_BR_STUCK_TICKS   30
#define GE_BR_DETOUR_TICKS  26

static int   ge_br_ready;
static int   ge_br_slot = -1;
static int   ge_br_obj  = -1;
static int   ge_br_trace;
static int   ge_br_step;            /* which step of the route we are on */
static int   ge_br_joined;          /* has the bot reached the route's first node yet */
static int   ge_br_stuck;           /* consecutive ticks commanded forward with no movement */
static int   ge_br_held;            /* frames spent waiting on a contested waypoint */
static int   ge_br_detour;          /* ticks left steering around an obstacle */
static float ge_br_detour_sign;     /* which way round it -- kept until the detour ends */
static int   ge_br_steps;
static int   ge_br_have_heading;
static float ge_br_heading;         /* degrees, atan2(dx, dz), matching the extractor */
static float ge_br_px, ge_br_py, ge_br_pz;
static int   ge_br_have_prev;

/* Advance toward a contested waypoint, or hold? Returns 1 to advance.
 *
 * Pure on purpose: the interesting part is the policy, and a policy tangled up with posting input
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
    if (!geWorldRouteStep(ge_br_obj, ge_br_step, &step)) { return; }

    /* JOIN THE ROUTE BEFORE WALKING IT.
     *
     * A route is a chain of waypoints joined by edges the graph says are walkable. The bot does
     * not spawn on one. Even with the measured spawn it lands some hundreds of units off the
     * nearest node -- Silo 40, Bunker 1 583, Caverns 3,390 -- and heading straight for step 0's
     * DESTINATION means crossing whatever lies between, on no edge at all. That is a straight
     * line through walls, and it is what the bot was doing: on Bunker 1 it closed 1332 to 1176
     * and then pushed into geometry for the rest of the run.
     *
     * So the first target is step 0's ORIGIN, not its destination. Reaching it puts the bot on
     * the graph, and from there every subsequent target is one edge away along a path the
     * extractor already decided was walkable.
     *
     * Only for the first step. Once joined, aiming at each step's origin again would be a
     * wasted round trip, because the previous step's destination IS this step's origin. */
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
        for (i = 0; i < geWorldWaypointCount(); i++) {
            if (geWorldWaypoint(i, &wp) && wp.id == want) { found = 1; break; }
        }
        if (!found) { return; }
    }

    /* Is anyone else heading for this waypoint? With no enemy source installed geEnemyThreatAt
     * returns 0 and the bot behaves exactly as before, so the policy is inert rather than wrong
     * on a build whose game-side shim has not landed. (From the Surface.) */
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
        if (ge_br_trace) {
            printf("[getv][botroute] reached waypoint %d (step %d/%d)\n",
                   step.to, ge_br_step, ge_br_steps);
            fflush(stdout);
        }
        return;
    }

    memset(&in, 0, sizeof in);
    if (!ge_br_have_heading) {
        /* No heading yet: walk forward to make one. Steering on an unknown heading turns the
         * bot in a random direction and then estimates from that, which converges eventually
         * but looks like a drunk and wastes the first seconds of a run. */
        in.stick_y = (signed char) GE_BR_WALK;
        gePlayerPost(ge_br_slot, gePlayerTick() + 1, &in, 1);
        return;
    }

    bearing = (float) (atan2((double) dx, (double) dz) * 180.0 / 3.14159265358979);
    err = ge_br_norm180(bearing - ge_br_heading);

    {
        /* NEGATED, and that is the fix rather than a convention wobble.
         *
         * The stick and the heading run in opposite senses: bondview2.c:7312 integrates
         * vv_theta += speedtheta * delta * 3.5, and speedtheta is built from analogTurn with the
         * sign the pad gives it, so a positive stick DECREASES the heading that atan2(x, z)
         * reports. Steering with err * gain therefore drives the bot away from its waypoint
         * instead of towards it.
         *
         * The failure is worse than "turns the wrong way", which is why it read as a freeze.
         * The bot rotates away until the error reaches 180 degrees, and at the antipode the
         * normalised error flips sign on the smallest heading change -- so the stick alternates
         * hard left and hard right every frame and the net rotation is nothing. Measured on
         * Bunker 1: heading went 360 to 325 away from a bearing of 146, then sat between
         * err=-178 and err=+177 while distance grew from 2384 to 2503.
         *
         * GETV_BOT_ROUTE_SIGN=1 restores the old sense for an A/B. */
        static int sign = 0;
        float sx;
        if (sign == 0) {
            const char *e = getenv("GETV_BOT_ROUTE_SIGN");
            sign = (e && *e && *e != '0') ? 1 : -1;
        }
        sx = (float) sign * err * GE_BR_TURN_GAIN;
        if (sx >  GE_BR_STICK_MAX) { sx =  GE_BR_STICK_MAX; }
        if (sx < -GE_BR_STICK_MAX) { sx = -GE_BR_STICK_MAX; }
        in.stick_x = (signed char) sx;
    }
    align = 1.0f - (float) fabs((double) err) / GE_BR_ALIGN_DEG;
    if (align < 0.0f) { align = 0.0f; }
    in.stick_y = (signed char) (GE_BR_WALK * align);

    /* WALLS.
     *
     * A route is a chain of waypoints; it is not a promise that the straight line to the next
     * one is clear, and between the spawn and the graph there is no edge at all. The bot walked
     * into geometry and stayed there: on Bunker 1 it closed 583 to 430 and then held position
     * for the rest of the run with speedforwards at 0.818 and the collision update refusing its
     * offset every single frame.
     *
     * Nothing in the state readout says "blocked", so it is inferred: commanded forward, aligned
     * with the target, and not actually moving. That combination cannot happen in open floor.
     *
     * The response is the cheap half of a bug-following walk -- turn a fixed amount and keep
     * walking, so the bot slides along the obstacle rather than pressing into it. The direction
     * is CHOSEN ONCE and held for the whole detour: re-deciding each tick makes it oscillate
     * against the wall, which is the same antipode instability that made the inverted steering
     * sign look like a freeze.
     *
     * This is deliberately not a pathfinder. It gets a bot off a wall it is scraping; it will
     * not solve a concave dead end, and it should not pretend to.
     */
    {
        float moved = (st.x - ge_br_px) * (st.x - ge_br_px)
                    + (st.z - ge_br_pz) * (st.z - ge_br_pz);

        if (ge_br_detour > 0) {
            ge_br_detour--;
            in.stick_x = (signed char) (ge_br_detour_sign * GE_BR_STICK_MAX);
            in.stick_y = (signed char) GE_BR_WALK;
        } else if (in.stick_y > 0 && moved < (GE_BR_MOVE_EPSILON * GE_BR_MOVE_EPSILON)) {
            ge_br_stuck++;
            if (ge_br_stuck >= GE_BR_STUCK_TICKS) {
                /* Turn towards the side the target is already on, so the detour makes progress
                 * around the obstacle instead of away from the route. */
                ge_br_detour_sign = (err < 0.0f) ? -1.0f : 1.0f;
                ge_br_detour = GE_BR_DETOUR_TICKS;
                ge_br_stuck = 0;
                if (ge_br_trace) {
                    printf("[getv][botroute] blocked at (%.0f %.0f) -- detouring %s\n",
                           (double) st.x, (double) st.z,
                           (ge_br_detour_sign < 0.0f) ? "left" : "right");
                    fflush(stdout);
                }
            }
        } else {
            ge_br_stuck = 0;
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
