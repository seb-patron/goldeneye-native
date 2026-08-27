/* Event bus and the derived emitters. See ge_event.h for what is derived and what is not.
 *
 * The derivation is deliberately conservative. Every emitter here answers a question the port
 * can already answer honestly, and none of them guesses at something only the game knows. A
 * derived "player gone" is the slot no longer reporting a position -- that covers death, but it
 * also covers a level transition and a frame where the pointer is momentarily unset, so it is
 * named GONE rather than DIED. Naming it DIED would be a claim the data does not support, and a
 * learning agent scoring deaths off it would be scoring level loads too.
 *
 *   GETV_EVENT_TRACE=1   log every event as it fires
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_event.h"
#include "ge_player_api.h"
#include "ge_world_api.h"
#include "ge_sense_api.h"   /* geSenseContactUpdate: this loop is the detector's only source */
#include "ge_world_levels.h"

#define GE_EV_MAX_SUBS   16
#define GE_EV_GUARD_NEAR_RADIUS 700.0f
/* Hysteresis: a guard is "clear" only once further than this, so a player standing exactly on
 * the boundary does not produce a stream of near/clear pairs every frame. */
#define GE_EV_GUARD_CLEAR_RADIUS 850.0f
#define GE_EV_TRACKED_GUARDS 8

typedef struct { GeEventFn fn; void *user; } GeEvSub;

static GeEvSub ge_ev_subs[GE_EV_MAX_SUBS];
static int     ge_ev_nsubs;
static int     ge_ev_trace = -1;

/* Derivation state, per slot. */
static struct {
    int present;
    int room;
    int waypoint;
    int near_guards[GE_EV_TRACKED_GUARDS];
    int n_near;
} ge_ev_slot[GE_MAX_SLOTS];

static int ge_ev_stage = -1;

const char *geEventName(GeEventType t)
{
    switch (t) {
    case GE_EV_LEVEL_CHANGE: return "level_change";
    case GE_EV_PLAYER_SPAWN: return "player_spawn";
    case GE_EV_PLAYER_GONE:  return "player_gone";
    case GE_EV_ROOM_CHANGE:  return "room_change";
    case GE_EV_WAYPOINT:     return "waypoint";
    case GE_EV_GUARD_NEAR:   return "guard_near";
    case GE_EV_GUARD_CLEAR:  return "guard_clear";
    default:                 return "none";
    }
}

int geEventSubscribe(GeEventFn fn, void *user)
{
    int i;
    if (fn == NULL) { return 0; }
    for (i = 0; i < ge_ev_nsubs; i++) {
        if (ge_ev_subs[i].fn == fn && ge_ev_subs[i].user == user) { return 1; }  /* idempotent */
    }
    if (ge_ev_nsubs >= GE_EV_MAX_SUBS) { return 0; }
    ge_ev_subs[ge_ev_nsubs].fn = fn;
    ge_ev_subs[ge_ev_nsubs].user = user;
    ge_ev_nsubs++;
    return 1;
}

void geEventUnsubscribe(GeEventFn fn, void *user)
{
    int i;
    for (i = 0; i < ge_ev_nsubs; i++) {
        if (ge_ev_subs[i].fn == fn && ge_ev_subs[i].user == user) {
            ge_ev_subs[i] = ge_ev_subs[ge_ev_nsubs - 1];
            ge_ev_nsubs--;
            return;
        }
    }
}

void geEventEmit(GeEventType type, int a, int b, int c)
{
    int i, n;
    GeEvSub snapshot[GE_EV_MAX_SUBS];

    if (ge_ev_trace < 0) { ge_ev_trace = (getenv("GETV_EVENT_TRACE") != NULL); }
    if (ge_ev_trace) {
        printf("[getv][event] %-13s a=%d b=%d c=%d\n", geEventName(type), a, b, c);
        fflush(stdout);
    }

    /* Copy the list before dispatching. A subscriber that unsubscribes itself while being
     * called would otherwise shuffle the array underneath the loop and skip its neighbour --
     * and unsubscribing from a handler is exactly what a one-shot subscriber does. */
    n = ge_ev_nsubs;
    memcpy(snapshot, ge_ev_subs, sizeof(GeEvSub) * (size_t) n);
    for (i = 0; i < n; i++) {
        if (snapshot[i].fn != NULL) { snapshot[i].fn(type, a, b, c, snapshot[i].user); }
    }
}

static int ge_ev_was_near(int slot, int chrnum)
{
    int i;
    for (i = 0; i < ge_ev_slot[slot].n_near; i++) {
        if (ge_ev_slot[slot].near_guards[i] == chrnum) { return 1; }
    }
    return 0;
}

void gePortEventFrame(int frame)
{
    extern int bossGetStageNum(void);
    int stage, slot;

    (void) frame;

    /* Level change first: everything else is per-level state and must be reset before it is
     * compared, or the first frame of a new level reports a room change from the old level's
     * room to the new one. */
    /* GETV_FPTRACE=1 -- the per-frame simulation fingerprint, for S5 (netplay determinism).
     *
     * Lockstep has one correctness property: identical inputs must produce identical simulations.
     * ge_net.c CATCHES a divergence between two peers, but only after it has happened and only
     * during a live session -- it reports that two machines disagree, never how long they agreed
     * or where they parted.
     *
     * 🔑 THE LONG-RUN TEST NEEDS NO SECOND MACHINE. Two peers fed identical inputs are, for the
     * determinism question, the same thing as ONE binary run twice. Delivering identical inputs is
     * the network's job and netsim.py already models it; whether the simulation is reproducible
     * GIVEN them is a separate property, and nothing tested it.
     *
     * ⚠️ NECESSARY, NOT SUFFICIENT. A pass means the simulation reproduces itself from the same
     * inputs; it says nothing about whether the transport delivers them. A FAILURE is decisive
     * though: a machine that cannot reproduce itself will never agree with another.
     *
     * ⚠️ SAMPLED HERE, PER FRAME, AND NOT WHERE ge_seed_fp IS SET. The first version instrumented
     * ge_playback in ge_player_api.c, which only runs when a caller POSTS input -- with no bot
     * driving, a 3,000-frame run produced TWO samples. A determinism trace that goes quiet
     * whenever the thing is idle is worse than none: it reports agreement it never checked.
     * g_randomSeed is read directly for the same reason -- ge_seed_fp would be stale on any frame
     * without input. */
    {
        static int on = -1;
        if (on < 0) { const char *e = getenv("GETV_FPTRACE"); on = (e != NULL && *e == '1'); }
        if (on) {
            extern unsigned long long g_randomSeed;
            printf("[getv][fp] %d %08x\n", frame,
                   (unsigned int) (g_randomSeed & 0xffffffffu));
        }
    }

    stage = bossGetStageNum();
    if (stage != ge_ev_stage) {
        int old = ge_ev_stage;
        ge_ev_stage = stage;
        memset(ge_ev_slot, 0, sizeof ge_ev_slot);
        for (slot = 0; slot < GE_MAX_SLOTS; slot++) { ge_ev_slot[slot].room = -1;
                                                      ge_ev_slot[slot].waypoint = -1; }
        geEventEmit(GE_EV_LEVEL_CHANGE, stage, old, 0);
    }

    for (slot = 0; slot < GE_MAX_SLOTS; slot++) {
        GePlayerState st;
        int present = (gePlayerStateGet(slot, &st) && st.present &&
                       (st.fields & GE_ST_POSITION));

        if (present != ge_ev_slot[slot].present) {
            ge_ev_slot[slot].present = present;
            geEventEmit(present ? GE_EV_PLAYER_SPAWN : GE_EV_PLAYER_GONE, slot, 0, 0);
            if (!present) {
                ge_ev_slot[slot].room = -1;
                ge_ev_slot[slot].waypoint = -1;
                ge_ev_slot[slot].n_near = 0;
                continue;
            }
        }
        if (!present) { continue; }

        /* FEED THE CONTACT DETECTOR. It stores a short history of where each slot has been and
         * whether movement was asked of it, and geSenseIsStuck answers from that rather than from
         * geometry.
         *
         * Done here because this loop already walks every slot once a frame with a position in
         * hand, and because nothing else was doing it: the detector shipped with storage, a query
         * and no source, so is_stuck returned false forever and the atlas printed "moving freely"
         * for a player standing still. A query with no data that answers anyway is worse than one
         * that refuses -- it reads as a measurement. */
        geSenseContactUpdate(slot, st.x, st.z, gePlayerCommandedMove(slot));

        /* Room and waypoint need world knowledge; without it these simply do not fire, which is
         * correct -- four levels have none. */
        if (geWorldLoaded()) {
            GeWorldWaypoint w;
            if (geWorldNearestWaypoint(st.x, st.y, st.z, &w)) {
                if (w.id != ge_ev_slot[slot].waypoint) {
                    ge_ev_slot[slot].waypoint = w.id;
                    geEventEmit(GE_EV_WAYPOINT, slot, w.id, 0);
                }
                if (w.room != ge_ev_slot[slot].room) {
                    int old = ge_ev_slot[slot].room;
                    ge_ev_slot[slot].room = w.room;
                    geEventEmit(GE_EV_ROOM_CHANGE, slot, w.room, old);
                }
            }

            {
                GeWorldGuard g[GE_EV_TRACKED_GUARDS];
                int n = geWorldGuardsNear(st.x, st.y, st.z, GE_EV_GUARD_NEAR_RADIUS,
                                          g, GE_EV_TRACKED_GUARDS);
                int keep[GE_EV_TRACKED_GUARDS];
                int nkeep = 0, i, j;

                for (i = 0; i < n; i++) {
                    if (!ge_ev_was_near(slot, g[i].chrnum)) {
                        float dx = g[i].x - st.x, dy = g[i].y - st.y, dz = g[i].z - st.z;
                        int d = (int) sqrt((double) (dx * dx + dy * dy + dz * dz));
                        geEventEmit(GE_EV_GUARD_NEAR, slot, g[i].chrnum, d);
                    }
                    if (nkeep < GE_EV_TRACKED_GUARDS) { keep[nkeep++] = g[i].chrnum; }
                }

                /* Anyone previously near who is now beyond the CLEAR radius has left. Using a
                 * wider radius than NEAR is what stops a player on the boundary emitting a
                 * near/clear pair every frame. */
                for (i = 0; i < ge_ev_slot[slot].n_near; i++) {
                    int chrnum = ge_ev_slot[slot].near_guards[i];
                    int still = 0;
                    for (j = 0; j < nkeep; j++) { if (keep[j] == chrnum) { still = 1; break; } }
                    if (!still) {
                        GeWorldGuard far_g[GE_EV_TRACKED_GUARDS];
                        int m = geWorldGuardsNear(st.x, st.y, st.z, GE_EV_GUARD_CLEAR_RADIUS,
                                                  far_g, GE_EV_TRACKED_GUARDS);
                        int inband = 0;
                        for (j = 0; j < m; j++) {
                            if (far_g[j].chrnum == chrnum) { inband = 1; break; }
                        }
                        if (!inband) {
                            geEventEmit(GE_EV_GUARD_CLEAR, slot, chrnum, 0);
                        } else if (nkeep < GE_EV_TRACKED_GUARDS) {
                            keep[nkeep++] = chrnum;   /* still in the hysteresis band */
                        }
                    }
                }

                memcpy(ge_ev_slot[slot].near_guards, keep, sizeof(int) * (size_t) nkeep);
                ge_ev_slot[slot].n_near = nkeep;
            }
        }
    }
}
