/* Small, self-contained SHA-1 (FIPS 180-1). No dependency on any other file in this tree --
 * the wizard has to run before anything else exists, including the toolchain fetch that would
 * bring in a library that could otherwise have done this. */
#ifndef GE_WIZARD_SHA1_H
#define GE_WIZARD_SHA1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[5];
    uint64_t bitlen;
    unsigned char buf[64];
    size_t buflen;
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const void *data, size_t len);
void sha1_final(sha1_ctx *ctx, unsigned char digest[20]);

/* Formats as 40 lowercase hex chars + '\0' into out (must hold >= 41 bytes). */
void sha1_hex(const unsigned char digest[20], char *out);

#ifdef __cplusplus
}
#endif

#endif
