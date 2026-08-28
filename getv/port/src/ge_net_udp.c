/* UDP transport and session establishment for the lockstep layer.
 *
 * ge_net.c deliberately knows nothing about sockets: whether a tick is ready, when to stall and
 * when machines have diverged are not socket concerns. This file is the socket half, plus the
 * part that has to happen before any of it -- machines finding each other and agreeing on who
 * is which slot and when to begin.
 *
 * FULL MESH, NOT A STAR
 *
 * Every machine must hear every other machine's input directly. In lockstep no peer can relay
 * for another without adding a hop to the one thing that must arrive inside the input delay, and
 * a star would leave joiners unable to hear each other at all -- every tick stalling on a peer
 * they have no path to. So the host's job is only to introduce everyone: it hands out the peer
 * table, and from then on every machine talks to every other directly.
 *
 * tools/netsim.py has modelled it this way from the start, with each machine sending to all
 * others. The star was the transport deviating from its own specification.
 *
 * WHY THERE IS A HELLO
 *
 * Learning a peer's address is not the same as having a path to it. A UDP endpoint behind a home
 * router generally will not accept a datagram from an address it has never sent to, so each
 * machine sends a HELLO to every peer it learns about. That outbound packet is what opens the
 * return path. It carries no data and its only job is to have been sent.
 *
 * WHY THE SESSION STARTS ON A SIGNAL
 *
 * Everyone must prime the same window and begin together. The host waits until the expected
 * number of players is present and then tells everybody to start, rather than each machine
 * opening whenever it happens to be ready.
 *
 *   GETV_NET_HOST=<port>            host a session on this port
 *   GETV_NET_JOIN=<host>:<port>     join one
 *   GETV_NET_PLAYERS=<n>            players the host waits for (default 2, max 4)
 *   GETV_NET_DELAY=<ticks>          input delay override
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_net.h"

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int ge_socklen;
  typedef SOCKET ge_socket;
  #define GE_INVALID_SOCK INVALID_SOCKET
  #define ge_close_socket closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  typedef socklen_t ge_socklen;
  typedef int ge_socket;
  #define GE_INVALID_SOCK (-1)
  #define ge_close_socket close
#endif

/* Handshake opcodes, kept clear of the in-session ones in ge_net.c so a late join packet during
 * play cannot be mistaken for input. */
#define GE_NET_MSG_JOIN  0x10
#define GE_NET_MSG_PEERS 0x11    /* host -> peer: the whole table, your slot, and start */
#define GE_NET_MSG_HELLO 0x12    /* peer -> peer: opens the return path, carries nothing */

/* slot(1) + addr(4) + port(2) */
#define GE_PEER_ENTRY 7

/* joyGetButtons/joyGetStickX/joyGetStickY are the game's own read-only accessors into joy.c's
 * already-sampled input ring -- NOT osContGetReadData(), which would poll and rescan devices a
 * second time this frame if called from here too. Declared locally rather than via joy.h: the
 * port layer's include path (build_windows.ps1's $portFlags) reaches port/include, not
 * vendor/ge-decomp/src, and these three are the only symbols gePortNetTick needs from it. The
 * real definitions are joy.c, linked into the same binary regardless of which header declared
 * the prototype. ANY_BUTTON is os_extension.h's 0xFFFF, copied for the same reason. */
extern unsigned short joyGetButtons(signed char contpadnum, unsigned short mask);
extern signed char    joyGetStickX(signed char contpadnum);
extern signed char    joyGetStickY(signed char contpadnum);
#define GE_NET_ANY_BUTTON 0xFFFFu

/* Set once gePortNetInit() succeeds, independent of geNetIsOpen(): a host still waiting for
 * players, or a joiner still retrying its JOIN packet, has to keep polling the socket even
 * though the lockstep session has not opened yet -- that IS the handshake. */
static int ge_net_active;

typedef struct GeUdpPeer {
    struct sockaddr_in addr;
    int used;
    int slot;
    int greeted;
} GeUdpPeer;

typedef struct GeUdpCtx {
    ge_socket sock;
    GeUdpPeer peer[GE_NET_MAX_PEERS];   /* everyone except us */
    int  is_host;
    int  local_slot;
    int  want_players;
    int  started;
    int  delay;
    struct sockaddr_in host_addr;       /* joiners: where to aim JOIN before we know anyone */
} GeUdpCtx;

static GeUdpCtx ge_udp;

/* The transport callbacks, defined below and referenced by ge_udp_open_session above them. */
static int  ge_udp_send_all(void *ctx, const void *data, int len);
static int  ge_udp_recv_one(void *ctx, void *data, int max);
static void ge_udp_shutdown(void *ctx);

static int ge_udp_startup(void)
{
#if defined(_WIN32)
    static int done = 0;
    WSADATA wsa;
    if (done) { return 1; }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[getv][udp] WSAStartup failed\n");
        return 0;
    }
    done = 1;
#endif
    return 1;
}

static void ge_udp_nonblocking(ge_socket s)
{
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int ge_udp_same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int ge_udp_peer_count(void)
{
    int i, n = 0;
    for (i = 0; i < GE_NET_MAX_PEERS; i++) { if (ge_udp.peer[i].used) { n++; } }
    return n;
}

/* Record a peer, or return the slot it already holds. Idempotent because UDP delivers a JOIN
 * twice as readily as once, and a duplicate must not consume a second slot. */
static int ge_udp_remember(const struct sockaddr_in *from, int slot)
{
    int i, free_i = -1;
    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        if (ge_udp.peer[i].used && ge_udp_same(&ge_udp.peer[i].addr, from)) {
            return ge_udp.peer[i].slot;
        }
        if (!ge_udp.peer[i].used && free_i < 0) { free_i = i; }
    }
    if (free_i < 0) { return -1; }

    if (slot < 0) {
        /* Host assigning: slot 0 is the host's, so joiners take 1 upward. */
        slot = 1;
        for (;;) {
            int taken = (slot == ge_udp.local_slot);
            for (i = 0; i < GE_NET_MAX_PEERS && !taken; i++) {
                if (ge_udp.peer[i].used && ge_udp.peer[i].slot == slot) { taken = 1; }
            }
            if (!taken) { break; }
            slot++;
            if (slot >= GE_NET_MAX_PEERS) { return -1; }
        }
    }
    ge_udp.peer[free_i].used    = 1;
    ge_udp.peer[free_i].addr    = *from;
    ge_udp.peer[free_i].slot    = slot;
    ge_udp.peer[free_i].greeted = 0;
    return slot;
}

/* Send a HELLO to any peer we have not yet spoken to. Until we have sent something, a peer's
 * router has no reason to let its replies back to us. */
static void ge_udp_greet(void)
{
    unsigned char msg = GE_NET_MSG_HELLO;
    int i;
    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        if (!ge_udp.peer[i].used || ge_udp.peer[i].greeted) { continue; }
        sendto(ge_udp.sock, (const char *) &msg, 1, 0,
               (struct sockaddr *) &ge_udp.peer[i].addr, sizeof(struct sockaddr_in));
        ge_udp.peer[i].greeted = 1;
    }
}

/* Host: tell one peer the whole table, its own slot, and whether to start.
 *
 * Built per recipient because each is told a different "your slot" and must be told about
 * everyone EXCEPT itself -- a machine that had itself in its own peer list would send every
 * input to itself and count its own echoes. */
static void ge_udp_send_table(const GeUdpPeer *to, int start)
{
    unsigned char buf[4 + GE_NET_MAX_PEERS * GE_PEER_ENTRY];
    int i, n = 0;
    int len;

    /* The host itself is a peer from everyone else's point of view. Its address is sent as all
     * zeroes rather than guessed: the host cannot know which of its own addresses a given peer
     * reached it on, and the peer already knows, because our datagram came from it. The whole
     * entry after the slot byte is zeroed -- address AND port -- because a partially filled
     * entry would ship uninitialised stack down the wire. */
    buf[4 + n * GE_PEER_ENTRY] = (unsigned char) ge_udp.local_slot;
    memset(buf + 5 + n * GE_PEER_ENTRY, 0, GE_PEER_ENTRY - 1);
    n++;

    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        if (!ge_udp.peer[i].used) { continue; }
        if (ge_udp_same(&ge_udp.peer[i].addr, &to->addr)) { continue; }   /* not itself */
        buf[4 + n * GE_PEER_ENTRY] = (unsigned char) ge_udp.peer[i].slot;
        memcpy(buf + 5 + n * GE_PEER_ENTRY, &ge_udp.peer[i].addr.sin_addr.s_addr, 4);
        memcpy(buf + 9 + n * GE_PEER_ENTRY, &ge_udp.peer[i].addr.sin_port, 2);
        n++;
    }

    buf[0] = GE_NET_MSG_PEERS;
    buf[1] = (unsigned char) n;
    buf[2] = (unsigned char) to->slot;
    buf[3] = (unsigned char) (start ? 1 : 0);
    len = 4 + n * GE_PEER_ENTRY;
    sendto(ge_udp.sock, (const char *) buf, (size_t) len, 0,
           (struct sockaddr *) &to->addr, sizeof(struct sockaddr_in));
}

static void ge_udp_broadcast_table(int start)
{
    int i;
    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        if (ge_udp.peer[i].used) { ge_udp_send_table(&ge_udp.peer[i], start); }
    }
}

static void ge_udp_open_session(void)
{
    GeNetTransport tp;
    int i;

    if (geNetIsOpen()) { return; }
    /* ZERO IT FIRST -- see the same guard in ge_net_enet.c. Anything added to GeNetTransport
     * later is stack garbage here until someone assigns it, and ge_net.c calls function pointers
     * it finds non-NULL. */
    memset(&tp, 0, sizeof tp);
    tp.ctx   = &ge_udp;
    tp.send  = ge_udp_send_all;
    tp.recv  = ge_udp_recv_one;
    tp.close = ge_udp_shutdown;
    geNetOpen(&tp, ge_udp.local_slot, ge_udp.delay);
    geNetSetSlotKind(ge_udp.local_slot, GE_NET_SLOT_LOCAL);
    /* Local is GE_SLOT_INJECTED too, not left as GE_SLOT_HARDWARE -- found live, not assumed:
     * ge_pad_from_hardware (ge_player_api.c) reads joyGetButtons((s8) slot, ...), PORT-INDEXED
     * BY SLOT, not always port 0. A joiner assigned slot 1 would have its own simulation read
     * physical controller port 1 -- empty on an ordinary one-controller machine -- while
     * gePortNetTick captures and transmits port 0, the port a human actually plays on. Two
     * different inputs for the same player on two machines is a guaranteed desync the moment
     * anyone presses anything, and it is exactly the pattern a live two-process run surfaced:
     * clean whenever neither side's human touched a key during the test, real DESYNCs
     * otherwise. Claiming local as INJECTED too means geNetTickBegin's own gePlayerPost() loop
     * -- already looping over every non-empty slot uniformly, local included -- is what feeds
     * this machine's own simulation, from the exact same captured GePlayerInput that goes out
     * on the wire, rather than two independent reads that only sometimes happen to agree. */
    gePlayerClaim(ge_udp.local_slot, GE_SLOT_INJECTED);
    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        if (ge_udp.peer[i].used) {
            geNetSetSlotKind(ge_udp.peer[i].slot, GE_NET_SLOT_REMOTE);
            gePlayerClaim(ge_udp.peer[i].slot, GE_SLOT_INJECTED);
        }
    }
    printf("[getv][udp] session starting: slot %d of %d players\n",
           ge_udp.local_slot, ge_udp_peer_count() + 1);
    fflush(stdout);
}

static int ge_udp_send_all(void *ctx, const void *data, int len)
{
    GeUdpCtx *c = (GeUdpCtx *) ctx;
    int i, sent = 0;
    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        if (!c->peer[i].used) { continue; }
        sent += (int) sendto(c->sock, (const char *) data, (size_t) len, 0,
                             (struct sockaddr *) &c->peer[i].addr, sizeof(struct sockaddr_in));
    }
    return sent;
}

static int ge_udp_recv_one(void *ctx, void *data, int max)
{
    GeUdpCtx *c = (GeUdpCtx *) ctx;
    struct sockaddr_in from;
    ge_socklen fromlen = sizeof from;
    unsigned char *p = (unsigned char *) data;
    int got = (int) recvfrom(c->sock, (char *) data, (size_t) max, 0,
                             (struct sockaddr *) &from, &fromlen);
    if (got <= 0) { return 0; }

    /* Handshake traffic is consumed here rather than handed up: ge_net.c should never learn
     * that a session had to be negotiated. */
    if (p[0] == GE_NET_MSG_HELLO) {
        /* Someone opening a path to us. If we do not know them yet the table is still in
         * flight, and remembering them now costs nothing. */
        ge_udp_remember(&from, -1);
        return 0;
    }

    if (p[0] == GE_NET_MSG_JOIN && c->is_host) {
        int slot = ge_udp_remember(&from, -1);
        int players;
        if (slot < 0) {
            printf("[getv][udp] refusing join: session is full\n");
            return 0;
        }
        players = ge_udp_peer_count() + 1;
        printf("[getv][udp] peer joined as slot %d (%d of %d)\n", slot, players, c->want_players);
        fflush(stdout);
        /* Re-send to EVERYONE, not just the joiner: earlier joiners have to learn about later
         * ones or the mesh stays half-built and they stall on a peer they never heard of. */
        ge_udp_broadcast_table(players >= c->want_players);
        if (players >= c->want_players) { ge_udp_open_session(); }
        return 0;
    }

    if (p[0] == GE_NET_MSG_PEERS && !c->is_host && got >= 4) {
        int n = p[1];
        int i;
        if (got < 4 + n * GE_PEER_ENTRY) { return 0; }
        c->local_slot = p[2];

        for (i = 0; i < n; i++) {
            const unsigned char *e = p + 4 + i * GE_PEER_ENTRY;
            struct sockaddr_in a;
            unsigned int ip;
            unsigned short port;
            memcpy(&ip, e + 1, 4);
            memcpy(&port, e + 5, 2);

            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            if (ip == 0) {
                /* The host lists itself with a zero address because it cannot know which of its
                 * addresses we reached it on. The datagram we are reading came from it, so its
                 * real address is the one we already have. */
                a = from;
            } else {
                a.sin_addr.s_addr = ip;
                a.sin_port = port;
            }
            /* Never add ourselves. A machine carrying itself in its own peer table would send
             * every input to itself and then count the echoes as duplicates from a peer. */
            if (e[0] == (unsigned char) c->local_slot) { continue; }
            ge_udp_remember(&a, e[0]);
        }
        ge_udp_greet();

        if (p[3]) {
            c->started = 1;
            ge_udp_open_session();
        }
        return 0;
    }

    return got;
}

static void ge_udp_shutdown(void *ctx)
{
    GeUdpCtx *c = (GeUdpCtx *) ctx;
    if (c->sock != GE_INVALID_SOCK) { ge_close_socket(c->sock); c->sock = GE_INVALID_SOCK; }
}

/* ENet is preferred when it was built in; this raw-socket transport is the fallback. Both
 * implement the same GeNetTransport and the same handshake, so which one is in use changes
 * nothing above the transport layer. */
extern int  geNetEnetInit(void);
extern void geNetEnetPoll(void);
static int ge_using_enet;

int gePortNetInit(void)
{
    const char *host_port = getenv("GETV_NET_HOST");
    const char *join      = getenv("GETV_NET_JOIN");
    const char *e;
    struct sockaddr_in me;

    if ((host_port == NULL || *host_port == '\0') && (join == NULL || *join == '\0')) {
        return 0;
    }

    if (geNetEnetInit()) {
        ge_using_enet = 1;
        ge_net_active = 1;
        return 1;
    }
    if (!ge_udp_startup()) { return 0; }

    memset(&ge_udp, 0, sizeof ge_udp);
    ge_udp.sock = GE_INVALID_SOCK;
    ge_udp.delay = GE_NET_DEFAULT_DELAY;
    ge_udp.want_players = 2;
    if ((e = getenv("GETV_NET_DELAY"))   != NULL) { ge_udp.delay = atoi(e); }
    if ((e = getenv("GETV_NET_PLAYERS")) != NULL) { ge_udp.want_players = atoi(e); }
    if (ge_udp.want_players < 2) { ge_udp.want_players = 2; }
    if (ge_udp.want_players > GE_NET_MAX_PEERS) { ge_udp.want_players = GE_NET_MAX_PEERS; }

    ge_udp.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ge_udp.sock == GE_INVALID_SOCK) {
        printf("[getv][udp] could not create socket\n");
        return 0;
    }
    ge_udp_nonblocking(ge_udp.sock);

    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);

    if (host_port != NULL && *host_port != '\0') {
        ge_udp.is_host    = 1;
        ge_udp.local_slot = 0;
        me.sin_port = htons((unsigned short) atoi(host_port));
        if (bind(ge_udp.sock, (struct sockaddr *) &me, sizeof me) != 0) {
            printf("[getv][udp] could not bind port %s\n", host_port);
            ge_close_socket(ge_udp.sock);
            ge_udp.sock = GE_INVALID_SOCK;
            return 0;
        }
        printf("[getv][udp] hosting on port %s, waiting for %d players\n",
               host_port, ge_udp.want_players);
    } else {
        char hostbuf[128];
        char *colon;
        struct sockaddr_in dst;

        me.sin_port = 0;
        if (bind(ge_udp.sock, (struct sockaddr *) &me, sizeof me) != 0) {
            printf("[getv][udp] could not bind a local port\n");
            ge_close_socket(ge_udp.sock);
            ge_udp.sock = GE_INVALID_SOCK;
            return 0;
        }

        strncpy(hostbuf, join, sizeof hostbuf - 1);
        hostbuf[sizeof hostbuf - 1] = '\0';
        colon = strchr(hostbuf, ':');
        if (colon == NULL) {
            printf("[getv][udp] GETV_NET_JOIN must be host:port, got '%s'\n", join);
            ge_close_socket(ge_udp.sock);
            ge_udp.sock = GE_INVALID_SOCK;
            return 0;
        }
        *colon = '\0';

        memset(&dst, 0, sizeof dst);
        dst.sin_family = AF_INET;
        dst.sin_port   = htons((unsigned short) atoi(colon + 1));
        dst.sin_addr.s_addr = inet_addr(hostbuf);
        if (dst.sin_addr.s_addr == INADDR_NONE) {
            printf("[getv][udp] '%s' is not a dotted-quad address\n", hostbuf);
            ge_close_socket(ge_udp.sock);
            ge_udp.sock = GE_INVALID_SOCK;
            return 0;
        }
        ge_udp.host_addr = dst;
        ge_udp_remember(&dst, 0);
        printf("[getv][udp] joining %s:%s\n", hostbuf, colon + 1);
    }
    fflush(stdout);
    ge_net_active = 1;
    return 1;
}

/* Drive the handshake until the session is up, then keep draining between ticks. */
void gePortNetPoll(void)
{
    unsigned char buf[1024];
    static int retry = 0;

    if (ge_using_enet) { geNetEnetPoll(); return; }
    if (ge_udp.sock == GE_INVALID_SOCK) { return; }

    /* Joiners repeat JOIN until the table arrives. A single join packet is exactly the thing UDP
     * loses, and losing it means waiting forever for a session nobody knows you want. */
    if (!ge_udp.is_host && !ge_udp.started) {
        if ((retry++ % 30) == 0) {
            unsigned char msg = GE_NET_MSG_JOIN;
            sendto(ge_udp.sock, (const char *) &msg, 1, 0,
                   (struct sockaddr *) &ge_udp.host_addr, sizeof(struct sockaddr_in));
        }
    }

    /* Bounded rather than a bare for(;;): a single test run produced a multi-million-line log
     * from this exact loop, both sides stuck, which a 20-second wall-clock kill was needed to
     * stop -- confirmed by direct measurement to NOT be gePortNetTick running unthrottled (the
     * call rate stayed a steady ~55-60/sec across every other run, including the ones that
     * stayed clean), so the runaway was specifically a receive/deliver cycle that kept finding
     * more to drain within one call. Never reproduced under controlled, instrumented conditions
     * across several further runs, so the exact trigger is still open -- this cap is a real
     * bound on the consequence (one gePortNetPoll call cannot itself run unbounded) rather than
     * a fix for a root cause that has not been pinned down yet. 256 is far above any legitimate
     * per-tick backlog: even a full second of redundancy-window traffic from every peer is a
     * small fraction of that. */
    {
        int drained = 0;
        for (;;) {
            int got = ge_udp_recv_one(&ge_udp, buf, (int) sizeof buf);
            if (got <= 0) { break; }
            geNetDeliver(buf, got);
            if (++drained >= 256) { break; }
        }
    }
}

/* Called once per simulated tick, from boss.c's retrace handler -- after
 * joyConsumeSamplesWrapper() has latched this tick's input, before anything simulates it.
 * Returns 1 to proceed, 0 to stall, the same contract as geNetTickBegin() itself, because that
 * is exactly what this wraps: a no-op (always 1) when no session was requested, a poll-and-run
 * pass-through while the session is still handshaking (geNetOpen has not been called yet, so
 * there is nothing to stall on -- ge_net.tick_base captures wherever gePlayerTick() happens to
 * be at that moment, which is what makes ticks spent waiting harmless rather than a discontinuity
 * to reconcile), and the real local-input-in/stall-or-post-out call once it has.
 *
 * The caller MUST treat 0 as "do nothing this pass" -- skip the simulation and the render both,
 * the same "nothing to do" shape boss.c already uses for its own frame-pacing skip -- or a
 * stalled machine runs ahead of input it does not have yet and desyncs. */
int gePortNetTick(void)
{
    GePlayerInput local;
    unsigned short pad;

    if (!ge_net_active) { return 1; }

    gePortNetPoll();

    if (!geNetIsOpen()) { return 1; }

    pad = joyGetButtons(0, (unsigned short) GE_NET_ANY_BUTTON);
    memset(&local, 0, sizeof local);
    local.buttons = gePlayerButtonsFromPad((unsigned int) pad);
    local.stick_x = joyGetStickX(0);
    local.stick_y = joyGetStickY(0);

    return geNetTickBegin(&local);
}
