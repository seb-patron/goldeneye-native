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
} GeBotSituation;

typedef struct GeBotAction {
    float       heading;      /* the ONLY heading; nothing else may write one */
    int         fire;
    int         advance;      /* 0 means hold position: never advance on an aim-owned heading */
    GeBotReason reason;
} GeBotAction;

GeBotAction geBotArbitrate(const GeBotSituation *s);

#endif
