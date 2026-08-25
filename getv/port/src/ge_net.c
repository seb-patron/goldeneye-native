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
#include <string.h>

#include "ge_net.h"

#define GE_NET_RING 64          /* per-slot ring of pending inputs; power of two */
#define GE_NET_RING_MASK (GE_NET_RING - 1)

#define GE_NET_MSG_INPUT 1
#define GE_NET_MSG_SYNC  2

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
} ge_net;

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
    printf("[getv][net] session open: local slot %d, input delay %d ticks\n",
           local_slot, delay_ticks);
    fflush(stdout);
    return 1;
}

void geNetClose(void)
{
    if (!ge_net.open) { return; }
    if (ge_net.tp.close) { ge_net.tp.close(ge_net.tp.ctx); }
    printf("[getv][net] session closed: %lu ticks, %lu stalls, %lu late inputs, %lu desyncs\n",
           ge_net.stats.ticks_simulated, ge_net.stats.ticks_stalled,
           ge_net.stats.inputs_late, ge_net.stats.desyncs);
    fflush(stdout);
    ge_net.open = 0;
}

int geNetIsOpen(void) { return ge_net.open; }
unsigned long geNetDelay(void) { return ge_net.delay; }

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

    if (p[0] == GE_NET_MSG_INPUT && len >= 1 + (int) sizeof(GeNetInputMsg)) {
        GeNetInputMsg m;
        GePlayerInput in;
        memcpy(&m, p + 1, sizeof m);
        memset(&in, 0, sizeof in);
        in.buttons = m.buttons;
        in.stick_x = m.stick_x;
        in.stick_y = m.stick_y;

        /* An input for a tick already simulated cannot be used -- the tick it belonged to is
         * gone. Counted rather than dropped quietly, because a rising late count is the signal
         * that the delay is too low for this link, which is a tunable rather than a bug. */
        if (m.tick <= gePlayerTick()) {
            ge_net.stats.inputs_late++;
            return;
        }
        ge_net_store((int) m.slot, m.tick, &in);
        ge_net.stats.inputs_received++;
        return;
    }

    if (p[0] == GE_NET_MSG_SYNC && len >= 1 + (int) sizeof(GeNetSyncMsg)) {
        GeNetSyncMsg m;
        memcpy(&m, p + 1, sizeof m);
        /* Only comparable if it is the tick we last fingerprinted ourselves. */
        if (m.tick == ge_net.last_sync_tick && m.fingerprint != ge_net.last_local_fp) {
            ge_net.stats.desyncs++;
            printf("[getv][net] DESYNC at tick %lu: peer slot %d reports %08x, we have %08x\n",
                   m.tick, (int) m.slot, m.fingerprint, ge_net.last_local_fp);
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

static void ge_net_send_input(unsigned long tick, const GePlayerInput *in)
{
    unsigned char buf[1 + sizeof(GeNetInputMsg)];
    GeNetInputMsg m;

    memset(&m, 0, sizeof m);
    m.tick    = tick;
    m.slot    = (unsigned char) ge_net.local_slot;
    m.buttons = in->buttons;
    m.stick_x = in->stick_x;
    m.stick_y = in->stick_y;

    buf[0] = GE_NET_MSG_INPUT;
    memcpy(buf + 1, &m, sizeof m);
    if (ge_net.tp.send) { ge_net.tp.send(ge_net.tp.ctx, buf, (int) sizeof buf); }
    ge_net.stats.inputs_sent++;
}

static void ge_net_send_sync(unsigned long tick)
{
    unsigned char buf[1 + sizeof(GeNetSyncMsg)];
    GeNetSyncMsg m;

    ge_net.last_sync_tick = tick;
    ge_net.last_local_fp  = gePlayerSeedFingerprint();

    memset(&m, 0, sizeof m);
    m.tick        = tick;
    m.fingerprint = ge_net.last_local_fp;
    m.slot        = (unsigned char) ge_net.local_slot;

    buf[0] = GE_NET_MSG_SYNC;
    memcpy(buf + 1, &m, sizeof m);
    if (ge_net.tp.send) { ge_net.tp.send(ge_net.tp.ctx, buf, (int) sizeof buf); }
}

int geNetTickBegin(const GePlayerInput *local_input)
{
    unsigned long now, future;
    int slot, ready = 1;

    if (!ge_net.open) { return 1; }          /* no session: nothing to coordinate */

    now    = gePlayerTick();
    future = now + ge_net.delay;

    /* Publish ours for the delayed tick, and keep a local copy: we are a peer to ourselves and
     * must not depend on the network echoing our own input back. */
    if (local_input) {
        ge_net_store(ge_net.local_slot, future, local_input);
        ge_net_send_input(future, local_input);
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
            gePlayerPost(slot, now, &in, 1);
        }
    }

    ge_net.stats.ticks_simulated++;

    /* Agreement check once a second. Cheap, and the only thing standing between a silent
     * divergence and knowing about it. */
    if ((now % 60) == 0) { ge_net_send_sync(now); }
    return 1;
}

void geNetStats(GeNetStats *out)
{
    if (out) { *out = ge_net.stats; }
}
