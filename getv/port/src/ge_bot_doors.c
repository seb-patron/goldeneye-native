/* Navigate by DOORS and OBJECTIVES rather than by waypoints.
 *
 * Why this exists alongside ge_bot_route
 *
 * The route follower walks a graph of pads, and pads are not places to walk: measured with the
 * teleport probe, 139 of Train's 180 nodes cannot be stood on at all, and across the twenty solo
 * levels it runs 33 to 240 each. A follower given targets a body cannot occupy will always be
 * short by however far the pad sits off the floor.
 *
 * Doors and objective targets do not have that problem. They are real interactive geometry, the
 * level knowledge already carries all 459 doors with their positions and nearest nav nodes, and
 * a bot can verify it arrived at one by opening it.
 *
 * It also matches how the levels are actually built. Train's walkthrough describes a linear chain
 * of carriages joined by doors with seven brake units along it -- so "walk to the next door,
 * open it, walk to the next" reproduces the shape a person walks, without a graph at all.
 *
 * THE POLICY, greedy:
 *
 *  pick the door D minimising dist(here, D) + dist(D, objective)
 *  walk to it, open it, pass through, mark it used
 *  when no door improves on walking straight at the objective, walk at the objective
 *
 * Greedy will not solve a level that doubles back, and it is not trying to. It is the shortest
 * path from "can perceive" to "arrives somewhere", and everything it cannot do is
 * visible in the trace rather than hidden in a heuristic.
 *
 *  GETV_BOT_DOORS=<slot> drive this slot by doors
 *  GETV_BOT_DOORS_OBJ=<n> which objective to head for, default 0
 *  GETV_BOT_DOORS_TRACE=1 say what it is doing
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_player_api.h"
#include "ge_world_api.h"
#include "ge_sense_api.h"

#define GE_BD_WALK 60.0f
#define GE_BD_STICK_MAX 80.0f
#define GE_BD_TURN_GAIN 3.0f
#define GE_BD_ALIGN_DEG 60.0f

/* Close enough to a door to open it. NOT a guess -- doorTestForInteract (propobj.c:14411)
 * requires xdiff*xdiff + zdiff*zdiff < 40000, which is exactly 200 units, and |ydiff| < 200.
 * 220 was over the line and that is why USE did nothing at 278.
 *
 * 180 rather than 200: the check uses the door's runtime position and the pack carries its
 * authored one, and being inside the real threshold matters more than reaching the nominal edge
 * of it. */
#define GE_BD_DOOR_REACH 180.0f

/* and the door must BE ON screen. The same function requires PROPFLAG_ONSCREEN before it will
 * consider the press at all, so walking past a door with USE held does nothing however close it
 * is -- the bot has to be LOOKING at it. Within this many degrees counts as looking; wider than
 * the frustum would let a bot claim it is facing something the renderer has culled. */
#define GE_BD_DOOR_FACE 30.0f

/* Close enough to the objective to call it arrived. The objective position is a prop, not a
 * standing spot, so this carries the same caveat as the route follower's arrive radius. */
#define GE_BD_OBJ_REACH 250.0f

#define GE_BD_MAX_DOORS 64

static int   ge_bd_slot = -1;
static int   ge_bd_obj;
static int   ge_bd_trace;
static int   ge_bd_ready;
static int   ge_bd_used[GE_BD_MAX_DOORS];   /* doors already passed, by prop index */
static int   ge_bd_nused;
static int   ge_bd_arrived;
static float ge_bd_best;            /* closest we have been to the objective */
static int   ge_bd_since_gain;      /* frames since that improved */

static float ge_bd_norm180(float a)
{
    while (a > 180.0f)  { a -= 360.0f; }
    while (a < -180.0f) { a += 360.0f; }
    return a;
}

static int ge_bd_is_used(int idx)
{
    int i;
    for (i = 0; i < ge_bd_nused; i++) {
        if (ge_bd_used[i] == idx) { return 1; }
    }
    return 0;
}

static void ge_bd_init(void)
{
    const char *slot = getenv("GETV_BOT_DOORS");
    const char *obj  = getenv("GETV_BOT_DOORS_OBJ");
    const char *tr   = getenv("GETV_BOT_DOORS_TRACE");

    ge_bd_ready = 1;
    if (slot == NULL || *slot == '\0') { return; }

    ge_bd_slot  = atoi(slot);
    ge_bd_obj   = (obj != NULL && *obj != '\0') ? atoi(obj) : 0;
    ge_bd_trace = (tr != NULL && *tr != '\0' && *tr != '0');

    if (ge_bd_slot < 0 || ge_bd_slot >= 4) { ge_bd_slot = -1; return; }
    gePlayerClaim(ge_bd_slot, GE_SLOT_INJECTED);
}

void gePortBotDoorsFrame(int frame)
{
    GePlayerState st;
    GeWorldObjective ob;
    GePlayerInput in;
    GeWorldProp door;
    float tx, tz, dist_obj;
    int have_door = 0, door_index = -1;
    float best = 0.0f;
    int i, n;

    if (!ge_bd_ready) { ge_bd_init(); }
    if (ge_bd_slot < 0 || ge_bd_arrived) { return; }
    if (!geWorldLoaded()) { return; }
    if (!gePlayerStateGet(ge_bd_slot, &st) || !st.present) { return; }
    if (!(st.fields & GE_ST_ANGLE)) { return; }
    if (!geWorldObjective(ge_bd_obj, &ob)) { return; }

    tx = ob.tx;
    tz = ob.tz;
    dist_obj = (float) sqrt((double) (((tx - st.x) * (tx - st.x)) + ((tz - st.z) * (tz - st.z))));

    /* MONOTONIC PROGRESS. Train measures 39.5:1 along its axis -- the only level in the game
     * over 8:1 -- and on a shape like that "further from the objective" is not a route, it is a
     * wrong turn. The bot has been taking them: it reaches roughly 12,800 and then wanders back
     * out and stalls.
     *
     * When the distance has not improved for a while, stop trusting the local obstacle logic and
     * re-aim. The obstacle rules are what walk it sideways into a compartment; the objective
     * bearing is what brings it back onto the axis. a NUDGE rather than an override:
     * the obstacle logic is right most of the time, and a rule that fires constantly would just
     * be a slower version of ignoring it. */
    if (ge_bd_best <= 0.0f || dist_obj < ge_bd_best - 30.0f) {
        ge_bd_best = dist_obj;
        ge_bd_since_gain = 0;
    } else {
        ge_bd_since_gain++;
    }

    if (dist_obj <= GE_BD_OBJ_REACH) {
        ge_bd_arrived = 1;
        printf("[getv][botdoors] ARRIVED at objective %d, %.0f units, after %d door(s)\n",
               ob.index, (double) dist_obj, ge_bd_nused);
        fflush(stdout);
        return;
    }

    /* Pick the door that most shortens the journey. dist(here, D) + dist(D, objective) rather
     * than nearest-door: the nearest one is regularly BEHIND the bot, and walking to it makes the
     * trip longer while looking like progress. */
    n = geWorldPropCount();
    for (i = 0; i < n; i++) {
        GeWorldProp pr;
        float d1, d2, total;

        if (!geWorldProp(i, &pr) || pr.kind != GE_PROP_DOOR) { continue; }
        if (ge_bd_is_used(i)) { continue; }

        d1 = (float) sqrt((double) (((pr.x - st.x) * (pr.x - st.x))
                                  + ((pr.z - st.z) * (pr.z - st.z))));
        d2 = (float) sqrt((double) (((tx - pr.x) * (tx - pr.x))
                                  + ((tz - pr.z) * (tz - pr.z))));
        total = d1 + d2;

        /* the next door ON the way, not the best door overall.
         *
         * Scoring on total path length picks whichever door happens to sit nearest the
         * objective, which on Train was one 9,351 units away -- the bot set off across the level
         * toward a door six carriages ahead instead of opening the one in front of it. Levels are
         * built as chains: the useful door is the nearest one that gets you CLOSER, and the rest
         * are reached by repeating that.
         *
         * The margin stops a door level with the bot, or slightly past the objective, from
         * counting as progress and causing a shuffle between two of them. */
        (void) total;
        if (d2 >= dist_obj - GE_BD_DOOR_REACH) { continue; }   /* not actually toward it */
        if (!have_door || d1 < best) { best = d1; door = pr; door_index = i; have_door = 1; }
    }

    if (have_door) {
        float dd = (float) sqrt((double) (((door.x - st.x) * (door.x - st.x))
                                        + ((door.z - st.z) * (door.z - st.z))));
        if (dd <= GE_BD_DOOR_REACH) {
            /* FACE IT FIRST. doorTestForInteract wants PROPFLAG_ONSCREEN, so a door beside the
             * bot is not openable no matter how near. Turn onto it and press; only count it used
             * once we have actually squared up, or the bot marks doors used that it never opened
             * and walks away from every one of them. */
            float db = ge_bd_norm180(
                (float) (atan2((double) (door.x - st.x), (double) (door.z - st.z))
                         * 180.0 / 3.14159265358979) - st.angle);

            if ((float) fabs((double) db) > GE_BD_DOOR_FACE) {
                float sxd = -db * GE_BD_TURN_GAIN;
                if (sxd >  GE_BD_STICK_MAX) { sxd =  GE_BD_STICK_MAX; }
                if (sxd < -GE_BD_STICK_MAX) { sxd = -GE_BD_STICK_MAX; }
                memset(&in, 0, sizeof in);
                in.stick_x = (signed char) sxd;
                if (ge_bd_trace && (frame % 60) == 0) {
                    printf("[getv][botdoors] squaring up to door %d, %+.0f off\n",
                           door_index, (double) db);
                    fflush(stdout);
                }
                gePlayerPost(ge_bd_slot, gePlayerTick() + 1, &in, 1);
                return;
            }

            /* Facing it and inside 200: press and walk through. */
            memset(&in, 0, sizeof in);
            in.buttons = GE_IN_USE;
            in.stick_y = (signed char) GE_BD_WALK;
            gePlayerPost(ge_bd_slot, gePlayerTick() + 1, &in, 1);
        }
        if (0) {
            /* Reached it. Mark it used BEFORE walking through, or the bot re-targets the door it
             * is standing in and never advances -- the same latch problem as the nearest-waypoint
             * test, which spent a whole run chasing a goal that moved with it. */
            if (ge_bd_nused < GE_BD_MAX_DOORS) { ge_bd_used[ge_bd_nused++] = door_index; }
            if (ge_bd_trace) {
                printf("[getv][botdoors] REACHED door %d at %.0f units (%d used), heading on\n",
                       door_index, (double) dd, ge_bd_nused);
                fflush(stdout);
            }
        } else {
            tx = door.x;
            tz = door.z;
        }
    }

    /* Steer. Same law as the route follower, including the negated sign: the stick and the
     * heading run in opposite senses, so err * gain drives away from the target. */
    {
        float dx = tx - st.x, dz = tz - st.z;
        float bearing = (float) (atan2((double) dx, (double) dz) * 180.0 / 3.14159265358979);
        float err = ge_bd_norm180(bearing - st.angle);
        float sx = -err * GE_BD_TURN_GAIN;
        float align;
        GeSenseContact c;

        if (sx >  GE_BD_STICK_MAX) { sx =  GE_BD_STICK_MAX; }
        if (sx < -GE_BD_STICK_MAX) { sx = -GE_BD_STICK_MAX; }

        memset(&in, 0, sizeof in);
        in.stick_x = (signed char) sx;

        align = 1.0f - ((float) fabs((double) err) / GE_BD_ALIGN_DEG);
        if (align < 0.0f) { align = 0.0f; }
        in.stick_y = (signed char) (GE_BD_WALK * align);

        /* Re-aim when nothing has improved for four seconds, and walk while doing it. This
         * runs BEFORE the obstacle check so the obstacle logic can still veto it -- a bot that
         * charges the axis through a crate has swapped one stall for another. */
        if (ge_bd_since_gain > 240) {
            ge_bd_since_gain = 0;
            in.stick_y = (signed char) GE_BD_WALK;
            if (ge_bd_trace) {
                printf("[getv][botdoors] no gain for 240f at %.0fu -- re-aiming on the axis\n",
                       (double) dist_obj);
                fflush(stdout);
            }
        }

        /* Perceive before stepping. A door gets the action button and forward pressure; anything
         * solid gets the nearest heading that is actually open. GE_SENSE_SOLID, not "not clear":
         * the ray starts at the bot's own feet, so its own body sets GE_SENSE_BODY every time. */
        if (geSenseAhead(st.x, st.z, st.angle, 160.0f, &c)) {
            if (c.what & GE_SENSE_DOOR) {
                in.buttons |= GE_IN_USE;
                in.stick_y = (signed char) GE_BD_WALK;
                in.stick_x = 0;
            } else if (c.what & GE_SENSE_SOLID) {
                float open_h = geSenseClearestHeading(st.x, st.z, st.angle, 180.0f, 160.0f);
                float turn = ge_bd_norm180(open_h - st.angle);
                float s2 = -turn * GE_BD_TURN_GAIN;

                if (s2 >  GE_BD_STICK_MAX) { s2 =  GE_BD_STICK_MAX; }
                if (s2 < -GE_BD_STICK_MAX) { s2 = -GE_BD_STICK_MAX; }
                in.stick_x = (signed char) s2;
                in.stick_y = (signed char) (GE_BD_WALK * 0.5f);
            }
        }

        if (ge_bd_trace && (frame % 120) == 0) {
            printf("[getv][botdoors] obj %.0fu, target %s %.0fu, err %+.0f, %d door(s) used\n",
                   (double) dist_obj, have_door ? "door" : "objective",
                   (double) sqrt((double) ((dx * dx) + (dz * dz))), (double) err, ge_bd_nused);
            fflush(stdout);
        }

        gePlayerPost(ge_bd_slot, gePlayerTick() + 1, &in, 1);
    }
}
