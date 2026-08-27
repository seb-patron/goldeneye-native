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
 * MEASURED FROM chrpropScoreAutoAimTarget IN chrprop.c, not estimated -- the first version of this
 * file guessed 35 degrees with no source at all, and a second pass guessed 12 by hand-waving
 * "screen fraction is roughly angle fraction", which is not true: a screen position is TANGENT-
 * linear in angle, not angle-linear, and the error from skipping that step was about 25%.
 *
 * The decomp's own comment names the box: "central auto-aim acceptance region... 65% vertically in
 * favor of the top of the screen and 50% horizontally" -- left 25% to right 75% of frame width,
 * top 17.5% to bottom 82.5% of frame height. Converting a screen fraction f (of the HALF-screen,
 * centre to edge) to an angle takes tan(theta) = f * tan(half_fov), not theta = f * half_fov.
 *
 * At the engine's own vertical FOV of 46 degrees (player.c:430, c_perspfovy) and the reference 4:3
 * aspect the original screen math assumes:
 *
 *     half_vfov = 23.00 deg,  half_hfov = atan((4/3) * tan(23.00 deg)) = 29.51 deg
 *     horizontal box edge: atan(0.5  * tan(29.51 deg)) = 15.80 deg   (0.5  = (0.75-0.5)/0.5)
 *     vertical   box edge: atan(0.65 * tan(23.00 deg)) = 15.42 deg   (0.65 = (0.825-0.5)/0.5)
 *
 * Both edges land within half a degree of each other despite the box being asymmetric in SCREEN
 * fraction (50% wide, 65% tall) -- the box was evidently sized to compensate for the FOV's own
 * aspect distortion, so it is close to a genuine angular cone after all. 15 is used as the round
 * number; a wider window (the port's aspect is configurable and follows the OS window, ge_config.c
 * key_aspect) widens half_hfov further and would only make this MORE permissive, so 15 is the
 * conservative choice across window shapes, not merely the 4:3 one.
 *
 * Still an approximation of a shape this is not: the real test is a screen-space box against a
 * character's projected bounding box, gated on unobstructed line of sight and a lock-on settle
 * timer, none of which a single half-angle can represent. See GE_ARB_REQUIRE_LOS below for the
 * line-of-sight half of that gap; the settle timer is not modelled and is called out in the
 * accuracy limits at the bottom of this file. */
#define GE_ARB_FIRE_CONE 15.0f

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
    a.retreat = 0;
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
        /* Turning to face a threat is worth it even without a confirmed clear shot -- the turn
         * itself is defensive here, unlike the routing branch where a turn is pure cost. So LOS
         * gates the trigger but not the turn. */
        a.fire = s->has_los;
        /* Do NOT advance while the heading belongs to the fight. This is the specific thing that
         * turned stop-and-shoot into fire-on-move-and-die: advancing along an aim-owned heading
         * walks into the guard. Progress is already lost in this branch; the life need not be. */
        a.advance = 0;
        a.reason = (s->health <= GE_ARB_LOW_HEALTH) ? GE_ARB_SURVIVING : GE_ARB_CORNERED;
        /* NOT YET MEASURED THE WAY THE REST OF THIS FILE IS -- flagged rather than dressed up as
         * derived. Standing and firing back (the policy above, unchanged) was correct against the
         * three encounters this file's own header documents. Measured live against a fourth,
         * tougher one (a guard holding a contested Train waypoint): health fell 1.00 -> 0.14 over
         * roughly 1800 frames of standing and returning fire, and kept falling from there to dead.
         * Standing still was not the fix for THAT encounter; it only slowed the loss.
         *
         * Retreat only below GE_ARB_LOW_HEALTH, not merely CORNERED -- a fully healthy bot that is
         * simply close to a target is not yet losing anything by turning to fight it, and backing
         * away from every close encounter would cost the six waypoints stop-and-shoot already
         * proved not worth paying except when health is actually the constraint. */
        a.retreat  = (s->health <= GE_ARB_LOW_HEALTH) ? 1 : 0;
        return a;
    }

    if (fabsf(err) <= GE_ARB_FIRE_CONE && s->has_los) {
        /* Inside the cone auto-aim can close, AND the shot is confirmed clear. Fire WITHOUT
         * touching the heading, and keep walking the route. This is the whole point: the shot is
         * free and the turn is not. */
        a.fire = 1;
        a.reason = GE_ARB_FIRING_ON_ROUTE;
        return a;
    }

    /* Outside the cone and not in danger: the target is not worth a turn. Keep routing and do not
     * announce a position by firing at something the engine cannot help hit. */
    return a;
}
