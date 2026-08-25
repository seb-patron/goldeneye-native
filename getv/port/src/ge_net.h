/* Deterministic lockstep session over the player-input seam.
 *
 * The transport is deliberately not part of this. A session needs to know when every slot's
 * input for a tick has arrived, when to stall, and when the machines have diverged -- none of
 * which is a socket concern. Sockets are supplied through GeNetTransport, so the hard part is
 * testable without one.
 */
#ifndef GE_NET_H
#define GE_NET_H

#include "ge_player_api.h"

#define GE_NET_MAX_PEERS   GE_MAX_SLOTS

/* Ticks of input delay. Every machine acts on input captured this many ticks ago, which is what
 * buys the network time to deliver it before the tick it belongs to is simulated. Higher hides
 * more latency at the cost of feel; 3 at 60Hz is 50ms, which covers most domestic WAN links. */
#define GE_NET_DEFAULT_DELAY 3

typedef enum GeNetSlotKind {
    GE_NET_SLOT_EMPTY = 0,
    GE_NET_SLOT_LOCAL,      /* this machine's player -- we capture and broadcast it */
    GE_NET_SLOT_REMOTE,     /* another machine's player -- we wait for it */
    GE_NET_SLOT_BOT         /* a local policy; simulated on every machine identically */
} GeNetSlotKind;

/* One slot's input for one tick, as it goes over the wire. Deliberately small and fixed: the
 * whole point of lockstep is that inputs travel, not state. */
typedef struct GeNetInputMsg {
    unsigned long tick;
    unsigned char slot;
    unsigned char _pad[3];
    unsigned int  buttons;
    signed char   stick_x;
    signed char   stick_y;
    unsigned char _pad2[2];
} GeNetInputMsg;

/* Periodic agreement check. If two machines simulated the same inputs and diverged, this is
 * where it shows up -- and it shows up as a number that differs, rather than as players
 * mysteriously standing in different places. */
typedef struct GeNetSyncMsg {
    unsigned long tick;
    unsigned int  fingerprint;
    unsigned char slot;
    unsigned char _pad[3];
} GeNetSyncMsg;

/* A transport moves bytes and says nothing about their meaning. Return the number of bytes
 * read, 0 for "nothing waiting", negative for a dead link. */
typedef struct GeNetTransport {
    void *ctx;
    int (*send)(void *ctx, const void *data, int len);
    int (*recv)(void *ctx, void *data, int max);
    void (*close)(void *ctx);
} GeNetTransport;

typedef struct GeNetStats {
    unsigned long ticks_simulated;
    unsigned long ticks_stalled;      /* how often we waited on a peer */
    unsigned long inputs_sent;
    unsigned long inputs_received;
    unsigned long inputs_late;        /* arrived for a tick already simulated: unusable */
    unsigned long desyncs;
} GeNetStats;

/* Start a session. `local_slot` is this machine's player. Returns 0 if the configuration is
 * unusable, which is checked rather than assumed. */
int  geNetOpen(GeNetTransport *transport, int local_slot, int delay_ticks);
void geNetClose(void);
int  geNetIsOpen(void);

void geNetSetSlotKind(int slot, GeNetSlotKind kind);
GeNetSlotKind geNetSlotKind(int slot);

/* Called once per tick, before the game simulates it.
 *
 * Publishes the local slot's input for tick+delay, drains anything that has arrived, and
 * reports whether every slot has input for the tick about to run. Returns 1 to proceed, 0 to
 * stall -- and a caller that ignores a 0 is a caller that desyncs. */
int  geNetTickBegin(const GePlayerInput *local_input);

/* Feed a received datagram in. Split out from the transport so a test can drive a session
 * without any I/O at all. */
void geNetDeliver(const void *data, int len);

void geNetStats(GeNetStats *out);
unsigned long geNetDelay(void);

#endif /* GE_NET_H */
