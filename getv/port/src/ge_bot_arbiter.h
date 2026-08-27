/* One decision, one writer: who owns the heading this frame.
 *
 * The route and the fight both want to steer. Two subsystems each writing a heading means the last
 * writer wins and the bot walks where neither intended, which is why latching each of them
 * separately never fixed it. This decides once.
 *
 * Pure by design -- everything it needs arrives in GeBotSituation and everything it concludes
 * leaves in GeBotAction. No engine calls, so the policy can be tested against the recorded
 * measurements without a level, a frame or a bot.
 */
#ifndef GE_BOT_ARBITER_H
#define GE_BOT_ARBITER_H

/* Why the arbiter chose what it chose. Worth reporting: a bot that stops is indistinguishable from
 * a bot that is stuck unless it can say which. */
typedef enum GeBotReason {
    GE_ARB_ROUTING = 0,       /* no target worth acting on; walking the route */
    GE_ARB_FIRING_ON_ROUTE,   /* target inside the auto-aim cone: shoot without turning */
    GE_ARB_CORNERED,          /* target too close to ignore; heading belongs to the fight */
    GE_ARB_SURVIVING          /* health low enough that progress is no longer the priority */
} GeBotReason;

typedef struct GeBotSituation {
    float route_heading;      /* degrees, atan2(x, z) like everything else here */
    int   has_target;
    float target_bearing;     /* degrees, same convention */
    float target_lateral;     /* signed left/right offset; used instead of the wrapped bearing
                               * error when a turn is needed, because it has no discontinuity
                               * behind the bot */
    float distance;           /* to the target */
    float health;             /* 0..1 */
    /* THE ENGINE'S OWN AUTO-AIM REQUIRES stanTestLineUnobstructed BEFORE IT LOCKS
     * (chrprop.c:2952), and a target inside the angular cone but behind cover is not something
     * auto-aim would ever help hit. Supplied by the caller rather than tested here, because this
     * file is pure and has no stan lookup to call; ge_sense_stable or geSenseLine is the natural
     * source.
     *
     * ⚠️ ZERO MEANS "DO NOT FIRE", NOT "UNKNOWN". A first draft of this comment said has_los
     * defaults to clear unless the caller sets it, which C cannot actually do: a zero-initialised
     * int and an explicit "no line of sight" are both the value 0, and nothing here can tell them
     * apart. Rather than add a third state, the zero value was given the SAFE meaning, matching how
     * ge_sense_api.c already treats an unresolved sample as "not safe to say clear" rather than
     * "assume clear". A caller that has not wired this up yet gets a bot that never fires, which
     * is a visible regression the moment it is exercised -- not a silent one. */
    int   has_los;
} GeBotSituation;

typedef struct GeBotAction {
    float       heading;      /* the ONLY heading; nothing else may write one */
    int         fire;
    int         advance;      /* 0 means hold position: never advance on an aim-owned heading */
    GeBotReason reason;
} GeBotAction;

GeBotAction geBotArbitrate(const GeBotSituation *s);

/* ⚠️ WHAT THIS DOES NOT MODEL, stated once here rather than discovered by watching a bot miss a
 * shot it should have kept.
 *
 * THE LOCK-ON SETTLE TIMER. bondviewUpdateXAutoAimTime keeps a target locked for 25-30 ticks
 * (BONDVIEW_AUTOAIM_TIME, bondview2.c) even after it leaves the acceptance box, so the real engine
 * is stickier than a per-frame cone test: a target that drifts out and back within the window never
 * loses lock. geBotArbitrate is stateless by design -- see the header comment on why that is what
 * makes it testable without a frame history -- and a settle timer needs state across frames by
 * definition. Modelling it means a THIN STATEFUL WRAPPER around this function, holding the last
 * target and a countdown, not a change to the pure core. Worth building if the per-frame cone
 * proves too twitchy in practice; not built here because there is no measurement yet showing it is
 * needed, and adding state on spec is exactly the kind of unverified change this project avoids. */

#endif
