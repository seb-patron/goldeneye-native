/* The discovery client: turning a session description into something a caller can act on.
 *
 * Most of this file is the parser, because that is where the bugs are. A spec comes from a config
 * file, a command line or a lobby server, and every one of those can hand over something slightly
 * wrong. The interesting assertions are the malformed ones.
 *
 * No sockets, no lobby server, and -- since the session-level roster was removed -- no ge_net
 * either. This file needs nothing but the unit under test, which is what a discovery layer with no
 * opinion about transports should cost to verify.
 */

#include <stdio.h>
#include <string.h>

#include "ge_discovery.c"

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %-52s %d\n", what, got);
    } else {
        printf("  FAIL  %-52s got %d want %d\n", what, got, want);
        failures++;
    }
}

/* A source that stalls for N polls before answering -- what a real lobby request looks like. */
static int pend_left;
static int pending_poll(void *ctx, char *out, int max)
{
    (void) max;
    if (pend_left-- > 0) { return 0; }
    strcpy(out, (const char *) ctx);
    return 1;
}
static int failing_poll(void *ctx, char *out, int max)
{ (void) ctx; (void) out; (void) max; return -1; }

int main(void)
{
    GeDiscoveryPeer p[GE_DISCOVERY_MAX_PEERS], one;
    GeDiscoverySource src;
    const char *ep;
    int n;

    printf("discovery client\n\n");

    /* ---------------- the parser ---------------- */

    n = geDiscoveryParse("0@10.0.0.1:5000,1@10.0.0.2:5000", p, 4);
    check("two peers parsed",            n, 2);
    check("first slot",                  p[0].slot, 0);
    check("second slot",                 p[1].slot, 1);
    check("not a bot",                   p[0].is_bot, 0);
    check("endpoint kept verbatim",      strcmp(p[0].endpoint, "10.0.0.1:5000") == 0, 1);

    /* '@' rather than ':' as the separator is what makes this work. An IPv6 literal is mostly
     * colons; splitting on one would mangle every address that is not bare IPv4. */
    n = geDiscoveryParse("2@[fe80::1a2b:3c4d]:5000", p, 4);
    check("IPv6 endpoint parsed",        n, 1);
    check("IPv6 slot",                   p[0].slot, 2);
    check("IPv6 kept its colons",        strcmp(p[0].endpoint, "[fe80::1a2b:3c4d]:5000") == 0, 1);

    /* Bots are simulated on every machine and have nothing to connect to. */
    n = geDiscoveryParse("0@10.0.0.1:5000,3@bot", p, 4);
    check("bot entry parsed",            n, 2);
    check("bot flagged",                 p[1].is_bot, 1);
    check("bot has no endpoint",         p[1].endpoint[0], 0);

    /* Specs come from files and command lines, where a stray space is a typo, not an error. */
    n = geDiscoveryParse("  0 @ 10.0.0.1:5000 , 1@bot ,", p, 4);
    check("whitespace tolerated",        n, 2);
    check("trailing comma tolerated",    p[1].is_bot, 1);
    check("  and the space was trimmed", strcmp(p[0].endpoint, "10.0.0.1:5000") == 0, 1);

    /* the strtol TRAP. "3x" parses as 3 if you only check the return value, so a typo silently
     * becomes a valid slot and a player ends up in someone else's seat. */
    check("non-numeric slot rejected",   geDiscoveryParse("3x@10.0.0.1:5000", p, 4), -1);
    check("  and it says so",            strstr(geDiscoveryError(), "non-numeric") != NULL, 1);

    check("missing @ rejected",          geDiscoveryParse("0-10.0.0.1", p, 4), -1);
    check("  and names the entry",       strstr(geDiscoveryError(), "'@'") != NULL, 1);

    check("out-of-range slot rejected",  geDiscoveryParse("9@10.0.0.1:5000", p, 4), -1);
    check("negative slot rejected",      geDiscoveryParse("-1@10.0.0.1:5000", p, 4), -1);
    check("empty endpoint rejected",     geDiscoveryParse("0@", p, 4), -1);
    check("empty spec rejected",         geDiscoveryParse("", p, 4), -1);
    check("only commas rejected",        geDiscoveryParse(",,,", p, 4), -1);

    /* An over-long endpoint is refused rather than truncated: a truncated address is a plausible
     * address pointing somewhere else. */
    {
        char big[GE_DISCOVERY_ENDPOINT_MAX + 32];
        strcpy(big, "0@");
        memset(big + 2, 'h', sizeof big - 3);
        big[sizeof big - 1] = '\0';
        check("over-long endpoint rejected", geDiscoveryParse(big, p, 4), -1);
    }

    /* ---------------- the state machine ---------------- */

    geDiscoveryReset();
    check("starts IDLE",                 geDiscoveryState(), GE_DISCOVERY_IDLE);
    check("poll with no source is IDLE", geDiscoveryPoll(), GE_DISCOVERY_IDLE);
    check("no peers when idle",          geDiscoveryPeerCount(), 0);

    check("begin static",                geDiscoveryBeginStatic("0@10.0.0.1:5000,1@10.0.0.2:5000,2@bot"), 1);
    check("pending before poll",         geDiscoveryState(), GE_DISCOVERY_PENDING);
    check("peers hidden while pending",  geDiscoveryPeerCount(), 0);
    check("poll reaches READY",          geDiscoveryPoll(), GE_DISCOVERY_READY);
    check("three peers available",       geDiscoveryPeerCount(), 3);

    check("peer readback",               geDiscoveryPeer(1, &one) && one.slot == 1, 1);
    check("readback out of range",       geDiscoveryPeer(9, &one), 0);

    /* What a transport being configured actually wants. */
    ep = geDiscoveryEndpointFor(1);
    check("endpoint for slot 1",         ep != NULL && strcmp(ep, "10.0.0.2:5000") == 0, 1);
    /* A bot has no address, and "" would read as one. NULL is the true answer and a different
     * answer from "the address is empty". */
    check("endpoint for a bot is NULL",  geDiscoveryEndpointFor(2) == NULL, 1);
    check("endpoint for absent slot",    geDiscoveryEndpointFor(3) == NULL, 1);

    /* Polling again is harmless -- it is called every frame. */
    check("re-poll stays READY",         geDiscoveryPoll(), GE_DISCOVERY_READY);

    geDiscoveryReset();
    check("reset returns to IDLE",       geDiscoveryState(), GE_DISCOVERY_IDLE);
    check("  and drops the peers",       geDiscoveryPeerCount(), 0);

    /* A source still in flight must report PENDING and never block. */
    pend_left = 3;
    memset(&src, 0, sizeof src);
    src.ctx  = (void *) "0@10.0.0.1:5000,1@10.0.0.2:5000";
    src.poll = pending_poll;
    check("begin pending source",        geDiscoveryBegin(&src), 1);
    check("poll 1 pending",              geDiscoveryPoll(), GE_DISCOVERY_PENDING);
    check("poll 2 pending",              geDiscoveryPoll(), GE_DISCOVERY_PENDING);
    check("poll 3 pending",              geDiscoveryPoll(), GE_DISCOVERY_PENDING);
    check("poll 4 ready",                geDiscoveryPoll(), GE_DISCOVERY_READY);
    check("  and the peers landed",      geDiscoveryPeerCount(), 2);

    /* A failing source fails the discovery, not the process. */
    geDiscoveryReset();
    memset(&src, 0, sizeof src);
    src.poll = failing_poll;
    geDiscoveryBegin(&src);
    check("failing source -> FAILED",    geDiscoveryPoll(), GE_DISCOVERY_FAILED);
    check("  error is set",              geDiscoveryError()[0] != '\0', 1);
    check("  and it stays failed",       geDiscoveryPoll(), GE_DISCOVERY_FAILED);
    check("  with no peers",             geDiscoveryPeerCount(), 0);

    /* An oversized spec is refused rather than truncated: a truncated session description is one
     * with a player silently missing, which shows up as a permanent stall. */
    {
        char big[GE_DISCOVERY_SPEC_MAX + 64];
        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        geDiscoveryReset();
        check("oversized spec refused",  geDiscoveryBeginStatic(big), 0);
    }

    printf("\n%s -- %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
