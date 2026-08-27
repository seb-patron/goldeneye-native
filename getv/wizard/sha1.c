#include "sha1.h"
#include <string.h>

static uint32_t rol(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }

static void sha1_block(sha1_ctx *ctx, const unsigned char block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t) block[i * 4]     << 24) |
               ((uint32_t) block[i * 4 + 1] << 16) |
               ((uint32_t) block[i * 4 + 2] << 8)  |
               ((uint32_t) block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2],
             d = ctx->state[3], e = ctx->state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }

        uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

void sha1_init(sha1_ctx *ctx)
{
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void sha1_update(sha1_ctx *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *) data;
    ctx->bitlen += (uint64_t) len * 8;

    while (len > 0) {
        size_t take = 64 - ctx->buflen;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == 64) {
            sha1_block(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void sha1_final(sha1_ctx *ctx, unsigned char digest[20])
{
    uint64_t bitlen = ctx->bitlen;

    unsigned char pad = 0x80;
    sha1_update(ctx, &pad, 1);

    unsigned char zero = 0x00;
    while (ctx->buflen != 56) sha1_update(ctx, &zero, 1);

    unsigned char lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (unsigned char) (bitlen >> (56 - 8 * i));
    /* Appends exactly 8 bytes to a 56-byte buffer -- always completes the final block,
     * never recurses into padding again. */
    memcpy(ctx->buf + ctx->buflen, lenbuf, 8);
    sha1_block(ctx, ctx->buf);

    for (int i = 0; i < 5; i++) {
        digest[i * 4]     = (unsigned char) (ctx->state[i] >> 24);
        digest[i * 4 + 1] = (unsigned char) (ctx->state[i] >> 16);
        digest[i * 4 + 2] = (unsigned char) (ctx->state[i] >> 8);
        digest[i * 4 + 3] = (unsigned char) (ctx->state[i]);
    }
}

void sha1_hex(const unsigned char digest[20], char *out)
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[i * 2]     = hexd[digest[i] >> 4];
        out[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    out[40] = '\0';
}
