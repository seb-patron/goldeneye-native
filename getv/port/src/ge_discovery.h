/* ge_discovery -- how machines find each other, and nothing more than that.
 *
 * docs/NETPLAY.md draws the line this file sits on, and it is worth restating because it is easy
 * to erode one convenience at a time:
 *
 *     server  -- who is playing, which slot, and each other's addresses
 *     peers   -- every tick of input, directly, over the transport
 *
 * Routing sixty-hertz input through a server adds a hop to the one thing that must arrive inside
 * the input delay, and turns every player's latency into the sum of two links instead of one. So
 * this layer produces exactly one thing -- a parsed description of who is in the session -- and is
 * then irrelevant. If the lobby server dies mid-match the match carries on, because nothing in a
 * running session depends on it. Any future call from the tick path into this file is a bug.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO
 *
 * It does not build the mesh. Both transports already do that themselves and have all along --
 * `ge_net_udp.c` says "FULL MESH, NOT A STAR" at the top of the file, and `ge_net_enet.c` relays
 * per-recipient peer tables including an "only the lower slot dials" rule so a pair does not end
 * up with two connections. An earlier version of this work added a second, session-level mesh
 * because NETPLAY.md claimed there was none. There was. It has been removed.
 *
 * So this file has no dependency on ge_net at all. It turns a session description into a list a
 * caller can act on -- typically by handing the host's endpoint to whichever transport is being
 * set up, which then does what it already knew how to do.
 *
 * WHY A SOURCE SEAM RATHER THAN A NAKAMA CLIENT
 *
 * Nakama is the adopted answer for lobbies, matchmaking and presence (Apache 2.0, self-hosted, and
 * a large amount of unglamorous work nobody here should be writing). Binding this file to it would
 * drag an HTTP client into the port layer and make the whole thing untestable without a server
 * running.
 *
 * A source is therefore a poll function returning a spec. The static source below covers LAN and
 * any WAN game where the addresses are known ahead of time, works today, and is fully testable. A
 * Nakama source becomes a small adapter that returns 0 while its request is in flight, with no
 * change to anything here.
 *
 * NOTHING BLOCKS. geDiscoveryPoll is called from the frame loop and must return immediately, every
 * time. A discovery layer that blocks on a socket freezes the game on a slow lobby server, which
 * is precisely the coupling the peer-to-peer split exists to avoid.
 */
#ifndef GE_DISCOVERY_H
#define GE_DISCOVERY_H

#ifdef __cplusplus
extern "C" {
#endif

#define GE_DISCOVERY_SPEC_MAX     512
#define GE_DISCOVERY_ENDPOINT_MAX 96
#define GE_DISCOVERY_MAX_PEERS    4    /* the game has four player slots and no more */

typedef enum GeDiscoveryState {
    GE_DISCOVERY_IDLE = 0,   /* nothing started */
    GE_DISCOVERY_PENDING,    /* asked, waiting -- poll again next frame */
    GE_DISCOVERY_READY,      /* a session description was parsed and is available */
    GE_DISCOVERY_FAILED      /* gave up; geDiscoveryError says why */
} GeDiscoveryState;

typedef struct GeDiscoveryPeer {
    int  slot;
    int  is_bot;                                 /* simulated everywhere; never connected to */
    char endpoint[GE_DISCOVERY_ENDPOINT_MAX];    /* empty for a bot */
} GeDiscoveryPeer;

/* Where a session description comes from. */
typedef struct GeDiscoverySource {
    void *ctx;

    /* Non-blocking. Returns 1 with a NUL-terminated spec in `out`, 0 for "not yet, ask again",
     * or -1 for failure. Must not block: see the header comment. */
    int (*poll)(void *ctx, char *out, int max);

    void (*close)(void *ctx);   /* optional */
} GeDiscoverySource;

/* ---------------------------------------------------------------- the spec format
 *
 * Comma-separated entries, each `slot@endpoint`:
 *
 *     0@10.0.0.1:5000,1@10.0.0.2:5000,2@bot
 *
 * `@` separates rather than `:` because an endpoint contains colons -- an IPv6 literal is mostly
 * colons -- and splitting on one would mangle every address that is not bare IPv4.
 *
 * `bot` marks a slot simulated identically on every machine, which needs no address. Endpoints are
 * kept as text: this file never learns what an address looks like, which is the same reason
 * GeNetTransport exists.
 */

/* Start discovery from a source. Takes effect on the next poll; does no work here. */
int geDiscoveryBegin(const GeDiscoverySource *src);

/* A source that answers immediately with a fixed spec. Covers LAN play, a dedicated setup, and any
 * WAN game where the addresses are known -- and it is what makes this layer testable without a
 * server. `spec` is copied. */
int geDiscoveryBeginStatic(const char *spec);

/* Drive discovery. Call once per frame; returns immediately, always. */
GeDiscoveryState geDiscoveryPoll(void);
GeDiscoveryState geDiscoveryState(void);

/* The parsed session, valid once READY. */
int geDiscoveryPeerCount(void);
int geDiscoveryPeer(int i, GeDiscoveryPeer *out);

/* The endpoint for one slot, or NULL. What a transport being configured actually wants. */
const char *geDiscoveryEndpointFor(int slot);

/* Why it failed, or "" if it has not. Human-readable and meant to be shown: a session that will
 * not start needs to say which endpoint it could not parse, not merely that something went
 * wrong. */
const char *geDiscoveryError(void);

void geDiscoveryReset(void);

/* Parse a spec without touching any state. Exposed for tests and for a caller that wants to
 * inspect a description before acting on it. Returns entries parsed, or -1. */
int geDiscoveryParse(const char *spec, GeDiscoveryPeer *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* GE_DISCOVERY_H */
