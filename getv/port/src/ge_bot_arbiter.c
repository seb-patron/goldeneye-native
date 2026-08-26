/* Who owns the heading this frame: the route, or the fight.
 *
 * DERIVED FROM THE THREE MEASUREMENTS, not from a scheme. Routing alone reaches waypoint 10 alive.
 * Stop-and-shoot reaches 4 at the same end health. Firing on the move reaches 4 and dies.
 *
 * The second and third are the informative pair. If STOPPING were the cost, firing while moving
 * should have recovered most of the six lost waypoints. It recovered none -- the same waypoint 4.
 * So the cost is not the trigger and not the stop. It is the only thing both fight modes share:
 * THE TURN TO FACE THE TARGET.
 *
 * And that also explains the death. Once the aim owns the heading, "moving" means moving along the
 * aim heading, which is straight at the guard. Stop-and-shoot at least does not close the distance,
 * which is why it survives at the same health while making the same progress.
 *
 * WHY NOT STRAFE. The obvious fix is to face the target and translate along the route, and the
 * engine does not offer it. In the default single-controller styles bondview2.c:5637-5663 sets
 * tankTurnLeftSpeed AND digitalStepLeft from the same button, so a sidestep also rotates; and in
 * aim mode digitalStep is disabled and canNaturalTurn goes false. Facing and movement are coupled
 * and cannot be separated from the input side. This was the first design and the code refuted it.
 *
 * WHAT THE ENGINE DOES OFFER IS AUTO-AIM. bondview.h:551-555 keeps separate autoaim_target_x and
 * autoaim_target_y with a time constant, so a shot does not need the player pointed at the target.
 * The turn was never buying accuracy that the engine was not already providing.
 *
 * SO THE POLICY IS: DO NOT TURN. Hold the route heading, and pull the trigger when the target is
 * inside the cone auto-aim can close. The trigger is nearly free; the turn costs six waypoints and
 * eventually a life.
 *
 * AND THE HEADING HAS EXACTLY ONE WRITER. A route turn and an engagement turn that each write a
 * heading in the same frame means the last writer wins and the bot walks wherever the loser was
 * not pointing. Latching each of them separately does not fix that -- it makes two correct
 * decisions that still disagree. This function returns the heading, and it is the only thing that
 * decides it.
 *
 * Pure: no globals, no engine calls, no frame state beyond what is passed in. That is what lets
 * the policy be tested against the measurements offline rather than by watching a bot and guessing
 * which of four subsystems moved it.
 */
#include <math.h>

#include "ge_bot_arbiter.h"

/* How far off the nose a target can sit and still be worth shooting at without turning.
 *
 * Auto-aim closes the last of it, so this is not an accuracy threshold -- it is the angle beyond
 * which a shot is wasted noise that also reveals position. Deliberately generous: the cost of a
 * missed shot is a bullet, and the cost of a turn is measured at six waypoints. */
#define GE_ARB_FIRE_CONE 35.0f

/* Below this the target is close enough that ignoring it is worse than the turn. A guard at
 * touching distance kills faster than the route advances, so the arbiter stops pretending the
 * route is still the priority. */
#define GE_ARB_PANIC_RANGE 260.0f

/* Health below which survival outranks progress. Firing on the move died; the recorded fall was
 * 1.00, 0.88, 0.69, 0.39, 0.11, dead. By 0.39 the next contact is likely fatal, so that is where
 * the policy stops spending health on progress. */
#define GE_ARB_LOW_HEALTH 0.40f

static float ge_arb_wrap180(float deg)
{
    while (deg > 180.0f)  { deg -= 360.0f; }
    while (deg < -180.0f) { deg += 360.0f; }
    return deg;
}

GeBotAction geBotArbitrate(const GeBotSituation *s)
{
    GeBotAction a;
    float err;

    a.heading = s->route_heading;
    a.fire = 0;
    a.advance = 1;
    a.reason = GE_ARB_ROUTING;

    if (!s->has_target) {
        return a;
    }

    /* Bearing to the target relative to where the route wants to go. Wrapped to +-180 ONCE, here,
     * so nothing downstream has to. */
    err = ge_arb_wrap180(s->target_bearing - s->route_heading);

    /* A target dead behind sits at exactly -180, where the sign flips on the smallest change. That
     * is the frozen-heading failure that has now appeared seven times in the bot code. It is only
     * dangerous to something that TURNS toward the target, and the routing branch below does not
     * turn at all, so the ordinary path is immune by construction rather than by latching.
     *
     * The panic branch does turn, so it takes the sign from the target's LATERAL offset instead of
     * from the wrapped error. Lateral offset does not have a discontinuity at 180 degrees: a
     * target directly behind and a hair to the left is a hair to the left, frame after frame. */
    if (s->distance <= GE_ARB_PANIC_RANGE || s->health <= GE_ARB_LOW_HEALTH) {
        float turn = (s->target_lateral >= 0.0f) ? 1.0f : -1.0f;
        a.heading = s->route_heading + turn * fabsf(err);
        a.fire = 1;
        /* Do NOT advance while the heading belongs to the fight. This is the specific thing that
         * turned stop-and-shoot into fire-on-move-and-die: advancing along an aim-owned heading
         * walks into the guard. Progress is already lost in this branch; the life need not be. */
        a.advance = 0;
        a.reason = (s->health <= GE_ARB_LOW_HEALTH) ? GE_ARB_SURVIVING : GE_ARB_CORNERED;
        return a;
    }

    if (fabsf(err) <= GE_ARB_FIRE_CONE) {
        /* Inside the cone auto-aim can close. Fire WITHOUT touching the heading, and keep walking
         * the route. This is the whole point: the shot is free and the turn is not. */
        a.fire = 1;
        a.reason = GE_ARB_FIRING_ON_ROUTE;
        return a;
    }

    /* Outside the cone and not in danger: the target is not worth a turn. Keep routing and do not
     * announce a position by firing at something the engine cannot help hit. */
    return a;
}
