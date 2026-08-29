/* Deterministic lockstep over the player-input seam.
 *
 * WHY THIS SHAPE
 *
 * ge_player_api already posts input per slot per tick and refuses a post for a tick that has
 * already run. That refusal is not a bot concern -- it is precisely the netplay failure it
 * looks like, which is why bots, remote players and RL agents can all ride one path. This file
 * is the part that makes several machines agree on which tick they are on.
 *
 * Lockstep rather than state replication: only inputs travel, and every machine simulates the
 * same thing from them. A GoldenEye tick's worth of input is twelve bytes, where its world
 * state is not something we could ship at 60Hz over a domestic link. The cost is that lockstep
 * is unforgiving -- every machine must simulate identically, and one that does not diverges
 * silently. Hence the fingerprint exchange below.
 *
 * Input delay rather than rollback: acting on input captured a few ticks ago gives the network
 * time to deliver it. Rollback hides more latency and would need full save/restore of game
 * state, which is a far larger change and not worth reaching for before measuring.
 *
 * THE TRANSPORT IS NOT HERE ON PURPOSE. Knowing when a tick is ready, when to stall and when
 * the machines have diverged is not a socket concern, and keeping it separate means the hard
 * part can be tested with no I/O at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_net.h"

#define GE_NET_RING 64          /* per-slot ring of pending inputs; power of two */
#define GE_NET_RING_MASK (GE_NET_RING - 1)

#define GE_NET_MSG_INPUT 1
#define GE_NET_MSG_SYNC  2
#define GE_NET_MSG_RELAY 3      /* a departed peer's inputs, pooled among survivors */
#define GE_NET_MSG_DROP  4      /* slot N stops existing at tick T, on every machine */

/* Ticks to wait after a departure before naming the drop tick, so relays from every survivor
 * have landed first. Naming it early means naming it from an incomplete pool. */
#define GE_NET_RELAY_SETTLE 8

typedef struct GeNetSlot {
    GeNetSlotKind kind;
    /* Ring of inputs by tick. `have` marks a slot-tick as known; without it a zeroed input is
     * indistinguishable from "nothing arrived", and standing still is a legitimate input. */
    GePlayerInput in[GE_NET_RING];
    unsigned long tick[GE_NET_RING];
    unsigned char have[GE_NET_RING];
} GeNetSlot;

static struct {
    int open;
    int local_slot;
    unsigned long delay;
    GeNetTransport tp;
    GeNetSlot slot[GE_NET_MAX_PEERS];
    GeNetStats stats;
    unsigned long last_sync_tick;
    unsigned int  last_local_fp;

    /* SESSION-RELATIVE TICK NUMBERING.
     *
     * Everything on the wire is numbered from zero at session open, not by gePlayerTick().
     * The game tick is per-machine: two players who joined a minute apart are thousands of
     * ticks apart, so "tick 900" would mean different moments on each machine and lockstep
     * would compare inputs that were never meant to line up. That failure looks exactly like a
     * desync while being purely a setup bug, which is the worst kind to debug.
     *
     * Each machine records where its own game clock stood when the session opened, and
     * converts at the boundary. Nothing else in the file needs to know. */
    unsigned long tick_base;

    /* Departure handling. */
    int  lost_slot;               /* -1 when nobody has gone */
    unsigned long lost_at;        /* session tick the departure was noticed */
    int  drop_slot;               /* -1 when no drop is scheduled */
    unsigned long drop_at;        /* the tick that slot stops existing, everywhere */
} ge_net;

/* Ticks since this session opened. */
static unsigned long ge_net_now(void)
{
    unsigned long t = gePlayerTick();
    return (t >= ge_net.tick_base) ? (t - ge_net.tick_base) : 0;
}

static void ge_net_store(int slot, unsigned long tick, const GePlayerInput *in)
{
    unsigned idx;
    if (slot < 0 || slot >= GE_NET_MAX_PEERS) { return; }
    idx = (unsigned)(tick & GE_NET_RING_MASK);
    ge_net.slot[slot].in[idx]   = *in;
    ge_net.slot[slot].tick[idx] = tick;
    ge_net.slot[slot].have[idx] = 1;
}

static int ge_net_get(int slot, unsigned long tick, GePlayerInput *out)
{
    unsigned idx = (unsigned)(tick & GE_NET_RING_MASK);
    if (!ge_net.slot[slot].have[idx])            { return 0; }
    if (ge_net.slot[slot].tick[idx] != tick)     { return 0; }   /* ring wrapped past it */
    if (out) { *out = ge_net.slot[slot].in[idx]; }
    return 1;
}

int geNetOpen(GeNetTransport *transport, int local_slot, int delay_ticks)
{
    if (local_slot < 0 || local_slot >= GE_NET_MAX_PEERS) {
        printf("[getv][net] refusing to open: slot %d is outside 0..%d\n",
               local_slot, GE_NET_MAX_PEERS - 1);
        return 0;
    }
    if (delay_ticks < 1 || delay_ticks >= GE_NET_RING) {
        printf("[getv][net] refusing to open: delay %d must be 1..%d\n",
               delay_ticks, GE_NET_RING - 1);
        return 0;
    }

    memset(&ge_net, 0, sizeof ge_net);
    ge_net.open       = 1;
    ge_net.local_slot = local_slot;
    ge_net.delay      = (unsigned long) delay_ticks;
    if (transport) { ge_net.tp = *transport; }
    ge_net.slot[local_slot].kind = GE_NET_SLOT_LOCAL;

    gePlayerApiInit();

    /* PRIME THE PIPELINE, or the session deadlocks before it starts.
     *
     * Input is only ever published for tick+delay, so the first `delay` ticks would never
     * receive input from anybody -- including from this machine itself -- and every slot would
     * stall forever waiting on a tick nobody was ever going to publish. Seeding those ticks
     * with neutral input is safe precisely because every machine seeds them identically:
     * agreement is preserved because nobody had a choice about them.
     *
     * Found by tools/netsim.py, which models this algorithm. Without priming, every scenario
     * it runs agrees on zero ticks and stalls indefinitely. */
    ge_net.lost_slot = -1;
    ge_net.drop_slot = -1;
    ge_net.tick_base = gePlayerTick();     /* session tick 0 is here, on this machine */
    {
        unsigned long t;
        GePlayerInput neutral;
        int s;
        memset(&neutral, 0, sizeof neutral);
        for (t = 0; t < ge_net.delay; t++) {
            for (s = 0; s < GE_NET_MAX_PEERS; s++) {
                ge_net_store(s, t, &neutral);
            }
        }
    }

    printf("[getv][net] session open: local slot %d, input delay %d ticks "
           "(first %d ticks primed neutral), tick_base %lu\n",
           local_slot, delay_ticks, delay_ticks, ge_net.tick_base);
    fflush(stdout);
    return 1;
}

void geNetClose(void)
{
    if (!ge_net.open) { return; }
    if (ge_net.tp.close) { ge_net.tp.close(ge_net.tp.ctx); }
    /* late and dup reported separately on purpose: late is a link problem worth acting on, dup
     * is the redundancy earning its keep and should be large. */
    printf("[getv][net] session closed: %lu ticks, %lu stalls, %lu late, %lu dup, %lu desyncs\n",
           ge_net.stats.ticks_simulated, ge_net.stats.ticks_stalled,
           ge_net.stats.inputs_late, ge_net.stats.inputs_dup, ge_net.stats.desyncs);
    fflush(stdout);
    ge_net.open = 0;
}

int geNetIsOpen(void) { return ge_net.open; }
unsigned long geNetDelay(void) { return ge_net.delay; }

/* -1 when no session is open, matching every other accessor here rather than returning a
 * stale slot number from a session that already closed. */
int geNetLocalSlot(void) { return ge_net.open ? ge_net.local_slot : -1; }

void geNetSetSlotKind(int slot, GeNetSlotKind kind)
{
    if (slot < 0 || slot >= GE_NET_MAX_PEERS) { return; }
    ge_net.slot[slot].kind = kind;
}

GeNetSlotKind geNetSlotKind(int slot)
{
    if (slot < 0 || slot >= GE_NET_MAX_PEERS) { return GE_NET_SLOT_EMPTY; }
    return ge_net.slot[slot].kind;
}

void geNetDeliver(const void *data, int len)
{
    const unsigned char *p = (const unsigned char *) data;
    if (!ge_net.open || p == NULL || len < 1) { return; }

    /* An input datagram is a COUNT followed by that many entries: the current input plus the
     * last few, so a packet lost in transit is covered by the next one to arrive. */
    if (p[0] == GE_NET_MSG_INPUT && len >= 2) {
        int count = p[1];
        int i;
        if (len < 2 + count * (int) sizeof(GeNetInputMsg)) { return; }

        for (i = 0; i < count; i++) {
            GeNetInputMsg m;
            GePlayerInput in;
            memcpy(&m, p + 2 + i * (int) sizeof(GeNetInputMsg), sizeof m);
            if (m.slot >= GE_NET_MAX_PEERS) { continue; }

            /* Already held: a redundant copy, which is the scheme working rather than anything
             * going wrong. Counted separately so it cannot swamp the late signal -- with a
             * window of 8 these outnumber real traffic several times over. */
            if (ge_net_get((int) m.slot, m.tick, NULL)) {
                ge_net.stats.inputs_dup++;
                continue;
            }

        /* Strictly less than. Input for the tick ABOUT TO RUN is still usable -- that tick has
         * not been simulated yet. Testing <= throws away every input that arrives exactly on
         * time, which caps the session at its primed window and then stalls forever on any link
         * where latency reaches the delay. Found by tools/netsim.py, which models this
         * algorithm: with <=, a 4-player session at latency == delay agreed on 3 ticks and then
         * stalled permanently; with <, it runs all 400 with no stalls at all. */
            if (m.tick < ge_net_now()) {
                ge_net.stats.inputs_late++;
                continue;
            }

            memset(&in, 0, sizeof in);
            in.buttons = m.buttons;
            in.stick_x = m.stick_x;
            in.stick_y = m.stick_y;
            ge_net_store((int) m.slot, m.tick, &in);
            ge_net.stats.inputs_received++;
        }
        return;
    }

    /* A relay carries the same entries as an input datagram; it just arrives once, on a
     * departure, and carries the vanished peer's inputs rather than the sender's own. Same
     * duplicate and late rules apply -- a relayed input for a tick we already simulated is as
     * useless as any other. */
    if (p[0] == GE_NET_MSG_RELAY && len >= 2) {
        int count = p[1];
        int i;
        if (len < 2 + count * (int) sizeof(GeNetInputMsg)) { return; }
        for (i = 0; i < count; i++) {
            GeNetInputMsg m;
            GePlayerInput in;
            memcpy(&m, p + 2 + i * (int) sizeof(GeNetInputMsg), sizeof m);
            if (m.slot >= GE_NET_MAX_PEERS) { continue; }
            if (ge_net_get((int) m.slot, m.tick, NULL)) { ge_net.stats.inputs_dup++; continue; }
            if (m.tick < ge_net_now()) { ge_net.stats.inputs_late++; continue; }
            memset(&in, 0, sizeof in);
            in.buttons = m.buttons;
            in.stick_x = m.stick_x;
            in.stick_y = m.stick_y;
            ge_net_store((int) m.slot, m.tick, &in);
        }
        return;
    }

    if (p[0] == GE_NET_MSG_DROP && len >= 1 + (int) sizeof(GeNetSyncMsg)) {
        GeNetSyncMsg m;
        memcpy(&m, p + 1, sizeof m);
        if (m.slot < GE_NET_MAX_PEERS) {
            ge_net.drop_slot = (int) m.slot;
            ge_net.drop_at   = m.tick;
            printf("[getv][net] slot %d drops at tick %lu (agreed)\n",
                   (int) m.slot, m.tick);
            fflush(stdout);
        }
        return;
    }

    if (p[0] == GE_NET_MSG_SYNC && len >= 1 + (int) sizeof(GeNetSyncMsg)) {
        GeNetSyncMsg m;
        memcpy(&m, p + 1, sizeof m);
        /* Only comparable if it is the tick we last fingerprinted ourselves. */
        if (m.tick == ge_net.last_sync_tick && m.fingerprint != ge_net.last_local_fp) {
            ge_net.stats.desyncs++;
            {
                extern unsigned long gePortRandomCallCount(void);
                printf("[getv][net] DESYNC at tick %lu: peer slot %d reports %08x, "
                       "we have %08x after %lu draws\n",
                       m.tick, (int) m.slot, m.fingerprint, ge_net.last_local_fp,
                       gePortRandomCallCount());
            }
            fflush(stdout);
        }
        return;
    }
}

static void ge_net_drain(void)
{
    unsigned char buf[256];
    int n;
    if (!ge_net.tp.recv) { return; }
    /* Bounded so a flooded socket cannot stall the frame indefinitely. */
    for (n = 0; n < 64; n++) {
        int got = ge_net.tp.recv(ge_net.tp.ctx, buf, (int) sizeof buf);
        if (got <= 0) { break; }
        geNetDeliver(buf, got);
    }
}

/* Send the input for `tick` plus the last GE_NET_REDUNDANCY-1 before it, in one datagram.
 *
 * Everything sent here is read back out of our own slot's ring, which is also where the local
 * input was just stored -- so there is no second copy of "what we published" to drift out of
 * step with the first. */
static void ge_net_send_inputs(unsigned long tick)
{
    unsigned char buf[2 + GE_NET_REDUNDANCY * sizeof(GeNetInputMsg)];
    int count = 0;
    int i;

    for (i = GE_NET_REDUNDANCY - 1; i >= 0; i--) {
        GePlayerInput in;
        GeNetInputMsg m;
        unsigned long t;

        if ((unsigned long) i > tick) { continue; }      /* no such tick yet, early in a session */
        t = tick - (unsigned long) i;
        if (!ge_net_get(ge_net.local_slot, t, &in)) { continue; }

        memset(&m, 0, sizeof m);
        m.tick    = t;
        m.slot    = (unsigned char) ge_net.local_slot;
        m.buttons = in.buttons;
        m.stick_x = in.stick_x;
        m.stick_y = in.stick_y;
        memcpy(buf + 2 + count * (int) sizeof m, &m, sizeof m);
        count++;
    }
    if (count == 0) { return; }

    buf[0] = GE_NET_MSG_INPUT;
    buf[1] = (unsigned char) count;
    if (ge_net.tp.send) {
        ge_net.tp.send(ge_net.tp.ctx, buf, 2 + count * (int) sizeof(GeNetInputMsg));
    }
    ge_net.stats.inputs_sent++;
}

static void ge_net_send_sync(unsigned long tick)
{
    unsigned char buf[1 + sizeof(GeNetSyncMsg)];
    GeNetSyncMsg m;

    ge_net.last_sync_tick = tick;
    ge_net.last_local_fp  = gePlayerSeedFingerprint();

    /* GETV_NET_RNGTRACE=1: tick, seed fingerprint and how many times this machine has drawn from
     * the shared sequence. The fingerprint alone says two machines diverged; it does not say
     * whether they drew a different NUMBER of times or the same number in a different order, and
     * those are different bugs. Diff two of these logs and the first tick whose call count
     * differs is the frame to look at.
     *
     * Printed every sync rather than only on a mismatch, because the count can separate before
     * the seed does: draws that cancel out in the low 32 bits still move the count. */
    {
        extern unsigned long gePortRandomCallCount(void);
        static int tr = -1;
        if (tr < 0) {
            const char *e = getenv("GETV_NET_RNGTRACE");
            tr = (e != NULL && *e != '\0' && *e != '0');
        }
        if (tr) {
            printf("[getv][net] rng tick=%lu slot=%d fp=%08x draws=%lu\n",
                   tick, (int) ge_net.local_slot, ge_net.last_local_fp,
                   gePortRandomCallCount());
            fflush(stdout);
        }
    }

    memset(&m, 0, sizeof m);
    m.tick        = tick;
    m.fingerprint = ge_net.last_local_fp;
    m.slot        = (unsigned char) ge_net.local_slot;

    buf[0] = GE_NET_MSG_SYNC;
    memcpy(buf + 1, &m, sizeof m);
    if (ge_net.tp.send) { ge_net.tp.send(ge_net.tp.ctx, buf, (int) sizeof buf); }
}

/* The lowest surviving slot names the drop tick. Arbitrary, but it must be a rule every machine
 * computes the same way, or two of them announce different ticks for the same peer. */
static int ge_net_proposer(void)
{
    int s;
    for (s = 0; s < GE_NET_MAX_PEERS; s++) {
        if (s == ge_net.lost_slot) { continue; }
        if (ge_net.slot[s].kind == GE_NET_SLOT_LOCAL ||
            ge_net.slot[s].kind == GE_NET_SLOT_REMOTE) {
            return s;
        }
    }
    return -1;
}

/* Highest tick we hold input for on `slot`. Read from the ring only -- never seeded with the
 * current tick. Seeding it puts the drop one tick beyond reach: a machine stalled at T would be
 * told to drop at T+1, which it can only get to by simulating T, which needs the very input
 * nobody has. That off-by-one presents as a session that agrees perfectly and stops dead. */
static long ge_net_highest_held(int slot)
{
    long top = -1;
    unsigned i;
    for (i = 0; i < GE_NET_RING; i++) {
        if (ge_net.slot[slot].have[i]) {
            long t = (long) ge_net.slot[slot].tick[i];
            if (t > top) { top = t; }
        }
    }
    return top;
}

void geNetPeerLost(int slot)
{
    unsigned char buf[2 + GE_NET_RING * sizeof(GeNetInputMsg)];
    int count = 0;
    unsigned i;

    if (!ge_net.open || slot < 0 || slot >= GE_NET_MAX_PEERS) { return; }
    if (ge_net.lost_slot == slot) { return; }              /* already handling it */

    ge_net.lost_slot = slot;
    ge_net.lost_at   = ge_net_now();

    /* RELAY what we hold for the departed slot. Nobody can have SIMULATED past the highest tick
     * anybody HOLDS, so once the survivors pool what they have, every one of them can reach the
     * same tick -- which is what makes a common drop tick reachable rather than a deadlock.
     * Bounded: it happens once, over the ring. */
    for (i = 0; i < GE_NET_RING && count < 64; i++) {
        GeNetInputMsg m;
        if (!ge_net.slot[slot].have[i]) { continue; }
        memset(&m, 0, sizeof m);
        m.tick    = ge_net.slot[slot].tick[i];
        m.slot    = (unsigned char) slot;
        m.buttons = ge_net.slot[slot].in[i].buttons;
        m.stick_x = ge_net.slot[slot].in[i].stick_x;
        m.stick_y = ge_net.slot[slot].in[i].stick_y;
        memcpy(buf + 2 + count * (int) sizeof m, &m, sizeof m);
        count++;
    }

    buf[0] = GE_NET_MSG_RELAY;
    buf[1] = (unsigned char) count;
    if (count > 0 && ge_net.tp.send) {
        ge_net.tp.send(ge_net.tp.ctx, buf, 2 + count * (int) sizeof(GeNetInputMsg));
    }
    printf("[getv][net] slot %d gone at tick %lu; relayed %d held input(s)\n",
           slot, ge_net.lost_at, count);
    fflush(stdout);
}

int geNetTickBegin(const GePlayerInput *local_input)
{
    unsigned long now, future;
    int slot, ready = 1;

    if (!ge_net.open) { return 1; }          /* no session: nothing to coordinate */

    now    = ge_net_now();
    future = now + ge_net.delay;

    /* A scheduled drop takes effect at ITS tick, identically on every machine. This is checked
     * before readiness, because the whole point is to stop waiting on a slot that is gone. */
    if (ge_net.drop_slot >= 0 && now >= ge_net.drop_at) {
        geNetSetSlotKind(ge_net.drop_slot, GE_NET_SLOT_EMPTY);
        printf("[getv][net] slot %d dropped at tick %lu\n", ge_net.drop_slot, now);
        fflush(stdout);
        ge_net.drop_slot = -1;
        ge_net.lost_slot = -1;
    }

    /* Once the relays have settled, the lowest surviving slot names the tick. Everyone holds
     * the same pooled inputs by now, so everyone can reach it. */
    if (ge_net.lost_slot >= 0 && ge_net.drop_slot < 0 &&
        now >= ge_net.lost_at + GE_NET_RELAY_SETTLE &&
        ge_net_proposer() == ge_net.local_slot) {
        long top = ge_net_highest_held(ge_net.lost_slot);
        unsigned char buf[1 + sizeof(GeNetSyncMsg)];
        GeNetSyncMsg m;

        ge_net.drop_slot = ge_net.lost_slot;
        ge_net.drop_at   = (unsigned long) (top + 1);

        memset(&m, 0, sizeof m);
        m.tick = ge_net.drop_at;
        m.slot = (unsigned char) ge_net.drop_slot;
        buf[0] = GE_NET_MSG_DROP;
        memcpy(buf + 1, &m, sizeof m);
        if (ge_net.tp.send) {
            ge_net.tp.send(ge_net.tp.ctx, buf, (int) sizeof buf);
        }
        printf("[getv][net] naming drop of slot %d at tick %lu (highest held %ld)\n",
               ge_net.drop_slot, ge_net.drop_at, top);
        fflush(stdout);
    }

    /* Publish ours for the delayed tick, and keep a local copy: we are a peer to ourselves and
     * must not depend on the network echoing our own input back.
     *
     * THIS HAPPENS BEFORE THE READINESS CHECK AND MUST STAY THERE. A stalled machine still has
     * to publish; if it went quiet while waiting, then the moment anything arrived late every
     * machine would be waiting on a peer that had stopped talking, and the session would
     * deadlock permanently rather than recover. tools/netsim.py reproduces exactly that when
     * the send is moved after the check. */
    if (local_input) {
        ge_net_store(ge_net.local_slot, future, local_input);
        ge_net_send_inputs(future);
    }

    ge_net_drain();

    /* Every slot that is supposed to act must have input for the tick about to run. A bot slot
     * counts as ready because it is simulated identically on every machine from the same seed;
     * that is only true while bot policy stays deterministic, which is a real constraint on
     * anything added to ge_bot.c. */
    for (slot = 0; slot < GE_NET_MAX_PEERS; slot++) {
        GeNetSlotKind k = ge_net.slot[slot].kind;
        if (k == GE_NET_SLOT_EMPTY || k == GE_NET_SLOT_BOT) { continue; }
        if (!ge_net_get(slot, now, NULL)) { ready = 0; }
    }

    if (!ready) {
        ge_net.stats.ticks_stalled++;
        return 0;
    }

    /* Hand every known input to the game for this tick. Posting rather than writing directly
     * keeps one path into the simulation for local play, bots and the network alike. */
    for (slot = 0; slot < GE_NET_MAX_PEERS; slot++) {
        GePlayerInput in;
        if (ge_net.slot[slot].kind == GE_NET_SLOT_EMPTY) { continue; }
        if (ge_net_get(slot, now, &in)) {
            /* GETV_NET_INTRACE=1 -- every input this machine applies, per tick per slot.
             *
             * Lockstep's one correctness property is that every machine applies the same inputs
             * for the same tick, and until this existed there was no way to check it: a desync
             * report says the states disagreed, not whether the inputs did. Diffing two
             * machines' traces answers that in one command, and it is what established that the
             * divergence here is NOT an input problem -- two runs that desynced applied
             * byte-identical inputs for every tick they shared. */
            if (getenv("GETV_NET_INTRACE") != NULL) {
                printf("[getv][in] t=%lu s=%d btn=%08x sx=%d sy=%d\n",
                       now, slot, (unsigned) in.buttons, (int) in.stick_x, (int) in.stick_y);
            }
            /* Back to game ticks at the boundary: the wire counts from session open, the
             * player API counts from game start. */
            gePlayerPost(slot, ge_net.tick_base + now, &in, 1);
        }
    }

    ge_net.stats.ticks_simulated++;

    /* Agreement check once a second. Cheap, and the only thing standing between a silent
     * divergence and knowing about it.
     *
     * GETV_NET_SYNCEVERY=<n> tightens it. At the default 60 a report names the second the
     * divergence was noticed, not the tick it happened on, which is the difference between
     * knowing a run desynced and being able to look at what ran. Set it to 1 to bisect; it
     * costs a small packet per tick and is not meant for ordinary play. */
    {
        static unsigned long every = 0;
        if (every == 0) {
            const char *e = getenv("GETV_NET_SYNCEVERY");
            long v = (e != NULL && *e != '\0') ? atol(e) : 60;
            every = (v > 0) ? (unsigned long) v : 60;
        }
        if ((now % every) == 0) { ge_net_send_sync(now); }
    }
    return 1;
}

void geNetStats(GeNetStats *out)
{
    if (out) { *out = ge_net.stats; }
}
