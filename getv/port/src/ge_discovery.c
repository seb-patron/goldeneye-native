/* Discovery: turn a description of a session into something a caller can act on, then get out of
 * the way. See ge_discovery.h for the division of labour this file is required to preserve. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_discovery.h"

static struct {
    GeDiscoverySource src;
    GeDiscoveryState  state;
    char              err[192];
    char              spec[GE_DISCOVERY_SPEC_MAX];   /* used by the static source */
    GeDiscoveryPeer   peer[GE_DISCOVERY_MAX_PEERS];
    int               peers;
    int               have_src;
} ge_disc;

/* One shape for every failure: what went wrong and which entry it was in. "discovery failed" on
 * its own sends someone reading a config file line by line.
 *
 * The format attribute is not decoration. The first version of this took fixed
 * (const char *, int) parameters while every call site passed whatever suited its message, so half
 * of them handed a pointer to a %d. That is undefined behaviour which usually prints a
 * plausible-looking number -- the worst way for an error reporter to fail. Varargs plus this
 * attribute makes the compiler check every one. */
#if defined(__GNUC__)
static void ge_disc_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

static void ge_disc_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ge_disc.err, sizeof ge_disc.err, fmt, ap);
    va_end(ap);
    ge_disc.state = GE_DISCOVERY_FAILED;
}

/* ---------------------------------------------------------------- the static source */

static int ge_disc_static_poll(void *ctx, char *out, int max)
{
    const char *spec = (const char *) ctx;
    if (spec == NULL || *spec == '\0') { return -1; }
    if ((int) strlen(spec) >= max)     { return -1; }
    strcpy(out, spec);   /* safe only because of the length check above, which is why it is there */
    return 1;
}

int geDiscoveryBeginStatic(const char *spec)
{
    GeDiscoverySource src;

    if (spec == NULL || *spec == '\0') { return 0; }
    if ((int) strlen(spec) >= GE_DISCOVERY_SPEC_MAX) {
        /* Refused, not truncated. A truncated spec is a session with a player silently missing,
         * which shows up later as a permanent stall on a slot nobody mentioned. */
        ge_disc_fail("session spec is %d bytes, max %d",
                     (int) strlen(spec), GE_DISCOVERY_SPEC_MAX - 1);
        return 0;
    }

    geDiscoveryReset();
    strcpy(ge_disc.spec, spec);

    memset(&src, 0, sizeof src);
    src.ctx  = ge_disc.spec;
    src.poll = ge_disc_static_poll;
    return geDiscoveryBegin(&src);
}

/* ---------------------------------------------------------------- the parser */

/* Trim spaces in place, returning the new start. Specs come from config files and command lines,
 * where a stray space around a comma is a typo rather than an error. */
static char *ge_disc_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') { s++; }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) { *--e = '\0'; }
    return s;
}

int geDiscoveryParse(const char *spec, GeDiscoveryPeer *out, int max)
{
    char work[GE_DISCOVERY_SPEC_MAX];
    char *cursor;
    int n = 0, entry = 0;

    if (spec == NULL || out == NULL || max <= 0) { return -1; }
    if ((int) strlen(spec) >= (int) sizeof work) {
        ge_disc_fail("session spec too long (max %d bytes)", (int) sizeof work - 1);
        return -1;
    }
    strcpy(work, spec);
    cursor = work;

    while (*cursor != '\0' && n < max) {
        char *comma, *at, *slot_text, *endpoint;
        long  slot;

        comma = strchr(cursor, ',');
        if (comma != NULL) { *comma = '\0'; }

        slot_text = ge_disc_trim(cursor);
        entry++;

        if (*slot_text == '\0') {           /* trailing or doubled comma: skip, do not fail */
            if (comma == NULL) { break; }
            cursor = comma + 1;
            continue;
        }

        /* '@' rather than ':' -- an endpoint is mostly colons when it is IPv6, and splitting on
         * one would mangle every address that is not bare IPv4. */
        at = strchr(slot_text, '@');
        if (at == NULL) {
            ge_disc_fail("session entry %d (\"%s\") has no '@'", entry, slot_text);
            return -1;
        }
        *at = '\0';
        endpoint  = ge_disc_trim(at + 1);
        slot_text = ge_disc_trim(slot_text);

        {   /* The slot must be digits and nothing else. strtol alone accepts "3x" as 3, and a typo
             * silently becoming a valid slot is how a player ends up in someone else's seat. */
            char *end = NULL;
            slot = strtol(slot_text, &end, 10);
            if (end == slot_text || (end != NULL && *end != '\0')) {
                ge_disc_fail("session entry %d has a non-numeric slot (\"%s\")", entry, slot_text);
                return -1;
            }
        }
        if (slot < 0 || slot >= GE_DISCOVERY_MAX_PEERS) {
            ge_disc_fail("session entry %d names slot %s, outside 0..%d",
                         entry, slot_text, GE_DISCOVERY_MAX_PEERS - 1);
            return -1;
        }
        if (*endpoint == '\0') {
            ge_disc_fail("session entry %d (slot %s) has an empty endpoint", entry, slot_text);
            return -1;
        }
        if ((int) strlen(endpoint) >= GE_DISCOVERY_ENDPOINT_MAX) {
            ge_disc_fail("session entry %d has a %d-byte endpoint, max %d",
                         entry, (int) strlen(endpoint), GE_DISCOVERY_ENDPOINT_MAX - 1);
            return -1;
        }

        memset(&out[n], 0, sizeof out[n]);
        out[n].slot = (int) slot;

        if (strcmp(endpoint, "bot") == 0) {
            out[n].is_bot = 1;              /* simulated everywhere; nothing to connect to */
        } else {
            strcpy(out[n].endpoint, endpoint);
        }

        n++;
        if (comma == NULL) { break; }
        cursor = comma + 1;
    }

    if (n == 0) {
        ge_disc_fail("session spec parsed to no entries");
        return -1;
    }
    return n;
}

/* ---------------------------------------------------------------- the state machine */

int geDiscoveryBegin(const GeDiscoverySource *src)
{
    if (src == NULL || src->poll == NULL) { return 0; }

    /* Deliberately does not clear ge_disc.spec: geDiscoveryBeginStatic fills it and then calls
     * here, and the source it passes points into it. */
    ge_disc.src      = *src;
    ge_disc.have_src = 1;
    ge_disc.state    = GE_DISCOVERY_PENDING;
    ge_disc.peers    = 0;
    ge_disc.err[0]   = '\0';
    return 1;
}

GeDiscoveryState geDiscoveryPoll(void)
{
    char spec[GE_DISCOVERY_SPEC_MAX];
    int r, n;

    if (!ge_disc.have_src)                     { return ge_disc.state; }
    if (ge_disc.state != GE_DISCOVERY_PENDING) { return ge_disc.state; }

    spec[0] = '\0';
    r = ge_disc.src.poll(ge_disc.src.ctx, spec, (int) sizeof spec);

    if (r == 0) { return GE_DISCOVERY_PENDING; }       /* still in flight */
    if (r < 0) {
        ge_disc_fail("discovery source reported failure");
        return ge_disc.state;
    }

    n = geDiscoveryParse(spec, ge_disc.peer, GE_DISCOVERY_MAX_PEERS);
    if (n < 0) { return ge_disc.state; }               /* geDiscoveryParse set the message */

    ge_disc.peers = n;
    ge_disc.state = GE_DISCOVERY_READY;
    printf("[getv][discovery] session described: %d slots\n", n);
    fflush(stdout);
    return ge_disc.state;
}

GeDiscoveryState geDiscoveryState(void) { return ge_disc.state; }
const char      *geDiscoveryError(void) { return ge_disc.err; }

int geDiscoveryPeerCount(void)
{
    return (ge_disc.state == GE_DISCOVERY_READY) ? ge_disc.peers : 0;
}

int geDiscoveryPeer(int i, GeDiscoveryPeer *out)
{
    if (out == NULL || ge_disc.state != GE_DISCOVERY_READY) { return 0; }
    if (i < 0 || i >= ge_disc.peers)                        { return 0; }
    *out = ge_disc.peer[i];
    return 1;
}

const char *geDiscoveryEndpointFor(int slot)
{
    int i;
    if (ge_disc.state != GE_DISCOVERY_READY) { return NULL; }
    for (i = 0; i < ge_disc.peers; i++) {
        if (ge_disc.peer[i].slot == slot) {
            /* A bot has no endpoint, and returning "" would read as one. NULL says "no address
             * exists for this slot", which is the true answer and a different one from "the
             * address is empty". */
            return ge_disc.peer[i].is_bot ? NULL : ge_disc.peer[i].endpoint;
        }
    }
    return NULL;
}

void geDiscoveryReset(void)
{
    if (ge_disc.have_src && ge_disc.src.close != NULL) {
        ge_disc.src.close(ge_disc.src.ctx);
    }
    memset(&ge_disc, 0, sizeof ge_disc);
    ge_disc.state = GE_DISCOVERY_IDLE;
}
