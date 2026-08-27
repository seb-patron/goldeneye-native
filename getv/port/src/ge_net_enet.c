/* ENet-backed transport for the lockstep session.
 *
 * ge_net_udp.c does this with raw sockets and works, but it reimplements what ENet has done
 * since 2002: connection setup and teardown, timeouts, disconnection detection, sequencing and
 * fragmentation. This is the same session, over a library that has been wrong in all the
 * interesting ways already and been fixed.
 *
 * What does NOT move here is ge_net.c. When a tick is ready, when to stall, and when the
 * machines have diverged is lockstep logic rather than transport, which is why the transport
 * was behind two function pointers from the start.
 *
 * INPUT IS SENT UNRELIABLE AND UNSEQUENCED, DELIBERATELY.
 *
 * Reliable delivery is the wrong tool here and actively harmful: it would hold a packet back
 * until an earlier lost one was resent, which is head-of-line blocking on the one thing that
 * has to arrive inside the input delay. We already solve loss better for this workload -- every
 * datagram repeats the last GE_NET_REDUNDANCY inputs, so a loss is covered by the next packet
 * with no round trip. Asking ENet for reliability on top would add latency to fix a problem
 * that is already fixed.
 *
 * ONLY THE LOWER SLOT DIALS.
 *
 * Every machine learns about every other, so without a rule both ends of each pair would call
 * connect and the mesh would carry two connections per pair -- every input arriving twice, and
 * two ways for a peer's state to disagree with itself. The rule is arbitrary but must be total
 * and agreed: the lower slot number connects, the higher listens.
 *
 * Built only when the library is present. tools/fetch_enet.sh fetches it; without it this file
 * compiles to a single function reporting that it is unavailable, and ge_net_udp.c is used
 * instead. ENet is an upgrade, never a prerequisite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_net.h"

#if !defined(GE_WITH_ENET)

int geNetEnetInit(void) { return 0; }
void geNetEnetPoll(void) { }

#else

#include <enet/enet.h>

#define GE_MSG_PEERS 0x11        /* host -> peer: the table, your slot, and start */
#define GE_PEER_ENTRY 7          /* slot(1) + host(4) + port(2) */
#define GE_RX_QUEUE 64

typedef struct GeEnetRx {
    unsigned char data[512];
    int len;
} GeEnetRx;

static struct {
    ENetHost *host;
    ENetPeer *peer[GE_NET_MAX_PEERS];   /* indexed by SLOT, not by arrival */
    int   slot_of_peer[GE_NET_MAX_PEERS];
    int   is_host;
    int   local_slot;
    int   want_players;
    int   delay;
    int   started;
    ENetAddress host_addr;

    GeEnetRx rx[GE_RX_QUEUE];
    int rx_head, rx_tail;
} ge_en;

static int ge_en_players(void)
{
    int i, n = 1;                                  /* ourselves */
    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        if (ge_en.peer[i] != NULL) { n++; }
    }
    return n;
}

/* ---------------------------------------------------------------- transport callbacks */

static int ge_en_send(void *ctx, const void *data, int len)
{
    ENetPacket *pkt;
    (void) ctx;
    if (ge_en.host == NULL) { return 0; }
    /* Unsequenced and unreliable: see the note at the top. Our own redundancy covers loss, and
     * reliability here would only add head-of-line blocking. */
    pkt = enet_packet_create(data, (size_t) len, ENET_PACKET_FLAG_UNSEQUENCED);
    if (pkt == NULL) { return 0; }
    enet_host_broadcast(ge_en.host, 0, pkt);
    return len;
}

static int ge_en_recv(void *ctx, void *data, int max)
{
    GeEnetRx *r;
    int len;
    (void) ctx;
    if (ge_en.rx_head == ge_en.rx_tail) { return 0; }
    r = &ge_en.rx[ge_en.rx_tail];
    ge_en.rx_tail = (ge_en.rx_tail + 1) % GE_RX_QUEUE;
    len = r->len < max ? r->len : max;
    memcpy(data, r->data, (size_t) len);
    return len;
}

static void ge_en_close(void *ctx)
{
    (void) ctx;
    if (ge_en.host != NULL) { enet_host_destroy(ge_en.host); ge_en.host = NULL; }
}

static void ge_en_queue(const unsigned char *data, int len)
{
    int next = (ge_en.rx_head + 1) % GE_RX_QUEUE;
    if (next == ge_en.rx_tail) { return; }          /* full: drop, redundancy will cover it */
    if (len > (int) sizeof ge_en.rx[0].data) { len = (int) sizeof ge_en.rx[0].data; }
    memcpy(ge_en.rx[ge_en.rx_head].data, data, (size_t) len);
    ge_en.rx[ge_en.rx_head].len = len;
    ge_en.rx_head = next;
}

/* ---------------------------------------------------------------- session */

static void ge_en_open_session(void)
{
    GeNetTransport tp;
    int i;
    if (geNetIsOpen()) { return; }

    /* ZERO IT FIRST. This struct is filled field by field, so anything added to GeNetTransport
     * later is stack garbage here until someone remembers to assign it -- and ge_net.c calls
     * function pointers it finds non-NULL. That very bug was introduced and caught during this
     * session by adding one optional callback to the struct. The cost of the memset is nothing;
     * the cost of omitting it lands on whoever extends the struct next, not on whoever left it
     * uninitialised. */
    memset(&tp, 0, sizeof tp);
    tp.ctx = NULL;
    tp.send = ge_en_send;
    tp.recv = ge_en_recv;
    tp.close = ge_en_close;
    geNetOpen(&tp, ge_en.local_slot, ge_en.delay);
    geNetSetSlotKind(ge_en.local_slot, GE_NET_SLOT_LOCAL);
    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        if (ge_en.peer[i] != NULL) { geNetSetSlotKind(i, GE_NET_SLOT_REMOTE); }
    }
    ge_en.started = 1;
    printf("[getv][enet] session starting: slot %d of %d players\n",
           ge_en.local_slot, ge_en_players());
    fflush(stdout);
}

/* Host: hand one peer the whole table, its slot, and whether to begin.
 *
 * Built per recipient because each is told a different "your slot" and must be told about
 * everyone EXCEPT itself. A machine holding itself in its own table would broadcast to itself
 * and count the echoes as a peer's input. */
static void ge_en_send_table(int to_slot, int start)
{
    unsigned char buf[4 + GE_NET_MAX_PEERS * GE_PEER_ENTRY];
    int i, n = 0, len;
    ENetPacket *pkt;

    if (ge_en.peer[to_slot] == NULL) { return; }

    /* The host, listed with a zero address: it cannot know which of its own addresses this peer
     * reached it on, and the peer does not need telling -- it is already connected to us. */
    buf[4] = (unsigned char) ge_en.local_slot;
    memset(buf + 5, 0, GE_PEER_ENTRY - 1);
    n++;

    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        unsigned char *e;
        if (ge_en.peer[i] == NULL || i == to_slot) { continue; }
        e = buf + 4 + n * GE_PEER_ENTRY;
        e[0] = (unsigned char) i;
        memcpy(e + 1, &ge_en.peer[i]->address.host, 4);
        memcpy(e + 5, &ge_en.peer[i]->address.port, 2);
        n++;
    }

    buf[0] = GE_MSG_PEERS;
    buf[1] = (unsigned char) n;
    buf[2] = (unsigned char) to_slot;
    buf[3] = (unsigned char) (start ? 1 : 0);
    len = 4 + n * GE_PEER_ENTRY;

    /* The table IS sent reliably. It is handshake, not input: it happens once, nothing repeats
     * it, and a peer that misses it never joins the mesh at all. */
    pkt = enet_packet_create(buf, (size_t) len, ENET_PACKET_FLAG_RELIABLE);
    if (pkt != NULL) { enet_peer_send(ge_en.peer[to_slot], 0, pkt); }
}

static void ge_en_broadcast_table(int start)
{
    int i;
    for (i = 0; i < GE_NET_MAX_PEERS; i++) {
        if (ge_en.peer[i] != NULL) { ge_en_send_table(i, start); }
    }
}

static int ge_en_free_slot(void)
{
    int i;
    for (i = 1; i < GE_NET_MAX_PEERS; i++) {          /* slot 0 is the host's */
        if (ge_en.peer[i] == NULL && i != ge_en.local_slot) { return i; }
    }
    return -1;
}

static void ge_en_handle_peers(const unsigned char *p, int len)
{
    int n, i;
    if (len < 4) { return; }
    n = p[1];
    if (len < 4 + n * GE_PEER_ENTRY) { return; }
    ge_en.local_slot = p[2];

    for (i = 0; i < n; i++) {
        const unsigned char *e = p + 4 + i * GE_PEER_ENTRY;
        int slot = e[0];
        ENetAddress a;

        if (slot == ge_en.local_slot) { continue; }       /* never ourselves */
        if (slot < 0 || slot >= GE_NET_MAX_PEERS) { continue; }
        if (ge_en.peer[slot] != NULL) { continue; }       /* already connected */

        memcpy(&a.host, e + 1, 4);
        memcpy(&a.port, e + 5, 2);
        if (a.host == 0) { continue; }                    /* the host, already connected */

        /* Only the lower slot dials, or every pair ends up with two connections. */
        if (ge_en.local_slot < slot) {
            ENetPeer *pr = enet_host_connect(ge_en.host, &a, 1, (enet_uint32) ge_en.local_slot);
            if (pr != NULL) {
                ge_en.peer[slot] = pr;
                printf("[getv][enet] dialling slot %d\n", slot);
            }
        }
    }

    if (p[3]) { ge_en_open_session(); }
    fflush(stdout);
}

/* ---------------------------------------------------------------- entry points */

int geNetEnetInit(void)
{
    const char *host_port = getenv("GETV_NET_HOST");
    const char *join      = getenv("GETV_NET_JOIN");
    const char *e;
    ENetAddress addr;

    if ((host_port == NULL || *host_port == '\0') && (join == NULL || *join == '\0')) {
        return 0;
    }
    if (enet_initialize() != 0) {
        printf("[getv][enet] enet_initialize failed\n");
        return 0;
    }

    memset(&ge_en, 0, sizeof ge_en);
    ge_en.delay = GE_NET_DEFAULT_DELAY;
    ge_en.want_players = 2;
    if ((e = getenv("GETV_NET_DELAY"))   != NULL) { ge_en.delay = atoi(e); }
    if ((e = getenv("GETV_NET_PLAYERS")) != NULL) { ge_en.want_players = atoi(e); }
    if (ge_en.want_players < 2) { ge_en.want_players = 2; }
    if (ge_en.want_players > GE_NET_MAX_PEERS) { ge_en.want_players = GE_NET_MAX_PEERS; }

    if (host_port != NULL && *host_port != '\0') {
        addr.host = ENET_HOST_ANY;
        addr.port = (enet_uint16) atoi(host_port);
        ge_en.host = enet_host_create(&addr, GE_NET_MAX_PEERS, 1, 0, 0);
        if (ge_en.host == NULL) {
            printf("[getv][enet] could not bind port %s\n", host_port);
            return 0;
        }
        ge_en.is_host = 1;
        ge_en.local_slot = 0;
        printf("[getv][enet] hosting on port %s, waiting for %d players\n",
               host_port, ge_en.want_players);
    } else {
        char hostbuf[128];
        char *colon;

        ge_en.host = enet_host_create(NULL, GE_NET_MAX_PEERS, 1, 0, 0);
        if (ge_en.host == NULL) {
            printf("[getv][enet] could not create client host\n");
            return 0;
        }
        strncpy(hostbuf, join, sizeof hostbuf - 1);
        hostbuf[sizeof hostbuf - 1] = '\0';
        colon = strchr(hostbuf, ':');
        if (colon == NULL) {
            printf("[getv][enet] GETV_NET_JOIN must be host:port, got '%s'\n", join);
            return 0;
        }
        *colon = '\0';
        if (enet_address_set_host(&ge_en.host_addr, hostbuf) != 0) {
            printf("[getv][enet] cannot resolve '%s'\n", hostbuf);
            return 0;
        }
        ge_en.host_addr.port = (enet_uint16) atoi(colon + 1);

        /* ENet retries the connection handshake itself, which is the whole reason the raw
         * socket version had to resend JOIN by hand. */
        if (enet_host_connect(ge_en.host, &ge_en.host_addr, 1, 0) == NULL) {
            printf("[getv][enet] no free connection slot\n");
            return 0;
        }
        ge_en.local_slot = -1;                       /* assigned by the host */
        printf("[getv][enet] connecting to %s:%d\n", hostbuf, (int) ge_en.host_addr.port);
    }
    fflush(stdout);
    return 1;
}

void geNetEnetPoll(void)
{
    ENetEvent ev;

    if (ge_en.host == NULL) { return; }

    while (enet_host_service(ge_en.host, &ev, 0) > 0) {
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (ge_en.is_host) {
                int slot = ge_en_free_slot();
                if (slot < 0) {
                    printf("[getv][enet] refusing connection: session is full\n");
                    enet_peer_disconnect(ev.peer, 0);
                    break;
                }
                ge_en.peer[slot] = ev.peer;
                printf("[getv][enet] peer connected as slot %d (%d of %d)\n",
                       slot, ge_en_players(), ge_en.want_players);
                /* Re-send to EVERYONE, not just the new peer: earlier joiners have to learn
                 * about later ones or the mesh stays half built. */
                ge_en_broadcast_table(ge_en_players() >= ge_en.want_players);
                if (ge_en_players() >= ge_en.want_players) { ge_en_open_session(); }
            } else if (ge_en.peer[0] == NULL) {
                ge_en.peer[0] = ev.peer;             /* the host answered */
            }
            fflush(stdout);
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            if (ev.packet->dataLength >= 1 && ev.packet->data[0] == GE_MSG_PEERS &&
                !ge_en.is_host) {
                ge_en_handle_peers(ev.packet->data, (int) ev.packet->dataLength);
            } else {
                /* Anything else is session traffic and belongs to ge_net.c. */
                ge_en_queue(ev.packet->data, (int) ev.packet->dataLength);
            }
            enet_packet_destroy(ev.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT: {
            int i;
            for (i = 0; i < GE_NET_MAX_PEERS; i++) {
                if (ge_en.peer[i] == ev.peer) {
                    printf("[getv][enet] slot %d disconnected\n", i);
                    ge_en.peer[i] = NULL;
                    /* Hand it to the session rather than freeing the slot here. Survivors
                     * notice a departure at different moments, so dropping on local detection
                     * makes the SURVIVORS diverge from each other -- geNetPeerLost runs the
                     * agreed procedure instead. */
                    geNetPeerLost(i);
                }
            }
            fflush(stdout);
            break;
        }

        default:
            break;
        }
    }

    /* Drain whatever the service loop queued into the session. */
    for (;;) {
        unsigned char buf[512];
        int got = ge_en_recv(NULL, buf, (int) sizeof buf);
        if (got <= 0) { break; }
        geNetDeliver(buf, got);
    }
}

#endif /* GE_WITH_ENET */
