/* Events: things happening, delivered to whoever asked, instead of everyone polling.
 *
 * A mod that wants to know when the player enters a room currently has to sample the position
 * every frame and diff it itself, and so does the next mod, and each does it slightly
 * differently. Worse, a learning agent needs EPISODE BOUNDARIES -- when a run started, when the
 * player died, when the level changed -- and those are exactly the transitions polling is worst
 * at: sample on the wrong frame and the boundary is silently missed.
 *
 * Derived events and authoritative ones are not the same thing, and this header says which is
 * which rather than blurring them.
 *
 * Everything here is DERIVED: the port polls what it can already read -- stage number, player
 * presence, position -- and emits an event when it changes. That makes them honest but coarse.
 * A derived death is "the slot stopped reporting a position", which is also what a level
 * transition looks like for a frame. A derived room change is a change in NEAREST WAYPOINT's
 * room, so it fires when the player crosses the midpoint between two waypoints rather than when
 * they cross the doorway.
 *
 * The authoritative versions -- actual damage, actual death, a shot fired, an objective flag
 * being set -- need publish sites inside the game, which is a different lane. When those land
 * they should emit through this same bus with the same type ids, and the derived emitters for
 * those types should be deleted rather than left racing them.
 */
#ifndef GE_EVENT_H
#define GE_EVENT_H

typedef enum GeEventType {
    GE_EV_NONE = 0,
    GE_EV_LEVEL_CHANGE,      /* a = new stage, b = old stage */
    GE_EV_PLAYER_SPAWN,      /* a = slot */
    GE_EV_PLAYER_GONE,       /* a = slot -- derived, so death and level exit look alike */
    GE_EV_ROOM_CHANGE,       /* a = slot, b = new room, c = old room */
    GE_EV_WAYPOINT,          /* a = slot, b = waypoint id */
    GE_EV_GUARD_NEAR,        /* a = slot, b = guard chrnum, c = distance rounded */
    GE_EV_GUARD_CLEAR,       /* a = slot, b = guard chrnum */
    GE_EV_COUNT
} GeEventType;

/* A C subscriber. Return value ignored; subscribers must not block or post input from here --
 * an event fires inside the frame hook, and a subscriber that posts would be posting for a tick
 * that is still being assembled. */
typedef void (*GeEventFn)(GeEventType type, int a, int b, int c, void *user);

int  geEventSubscribe(GeEventFn fn, void *user);
void geEventUnsubscribe(GeEventFn fn, void *user);

/* Publish. Safe to call from anywhere in the port; when game-side publish sites exist they call
 * this too. */
void geEventEmit(GeEventType type, int a, int b, int c);

const char *geEventName(GeEventType type);

/* Poll the derivable state and emit what changed. Called once per rendered frame. */
void gePortEventFrame(int frame);

#endif /* GE_EVENT_H */
