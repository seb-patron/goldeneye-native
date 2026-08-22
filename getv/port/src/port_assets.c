/* GoldenEye tvOS port — the asset bridge.
 *
 * The decomp extracts every asset to C source (assets/, 1,324 files), so the data is
 * already linked into the binary. There is no cartridge to DMA from and no ROM loader
 * needed for this class of asset.
 *
 * What the game still expects is the N64 *shape*: each asset object was placed in
 * its own ROM segment, and the game copies the whole segment into RAM with
 * romCopy() before relocating pointers inside it (see textrelated.c). That works
 * only if the segment is contiguous.
 *
 * Separate C arrays have no adjacency guarantee -- the same link-order hazard that
 * already affected bg.c (specialportalarray), gunfire.c (D_80035D04) and
 * blood_animation.c. So rather than assume the arrays land back to back, the port
 * concatenates them into a real buffer once and hands the game that. The game's own
 * copy and pointer-relocation logic then runs unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exported from the asset TUs themselves, because sizeof() is only visible there. */
extern const unsigned int ge_fontBankGothic_sizes[3];
extern void *const        ge_fontBankGothic_parts[3];
extern const unsigned int ge_fontZurichBold_sizes[3];
extern void *const        ge_fontZurichBold_parts[3];
extern const unsigned int ge_fontDl_sizes[3];
extern void *const        ge_fontDl_parts[3];

static void *ge_pack(const char *name, void *const parts[3],
                     const unsigned int sizes[3], unsigned int *out_len)
{
    static struct { const void *key; void *buf; unsigned int len; } cache[4];
    static int cached = 0;

    unsigned int total = sizes[0] + sizes[1] + sizes[2];
    unsigned char *buf;
    int i;

    /* Built once and kept: the game may reload fonts between stages. */
    for (i = 0; i < cached; i++) {
        if (cache[i].key == parts) {
            if (out_len) *out_len = cache[i].len;
            return cache[i].buf;
        }
    }

    buf = malloc(total);
    if (buf == NULL) {
        printf("[getv] asset: failed to pack %s (%u bytes)\n", name, total);
        if (out_len) *out_len = 0;
        return NULL;
    }

    memcpy(buf,                         parts[0], sizes[0]);
    memcpy(buf + sizes[0],              parts[1], sizes[1]);
    memcpy(buf + sizes[0] + sizes[1],   parts[2], sizes[2]);

    printf("[getv] asset: packed %s -> %u bytes (%u+%u+%u)\n",
           name, total, sizes[0], sizes[1], sizes[2]);

    if (cached < (int)(sizeof(cache) / sizeof(cache[0]))) {
        cache[cached].key = parts;
        cache[cached].buf = buf;
        cache[cached].len = total;
        cached++;
    }
    if (out_len) *out_len = total;
    return buf;
}

void *gePortFontBankGothic(unsigned int *len)
{
    return ge_pack("fontBankGothic", ge_fontBankGothic_parts,
                   ge_fontBankGothic_sizes, len);
}

void *gePortFontZurichBold(unsigned int *len)
{
    return ge_pack("fontZurichBold", ge_fontZurichBold_parts,
                   ge_fontZurichBold_sizes, len);
}

void *gePortFontDl(unsigned int *len)
{
    return ge_pack("fontDl", ge_fontDl_parts, ge_fontDl_sizes, len);
}

/* ---- images segment ----------------------------------------------------- */

/* ge007.ld puts assets/images/combined/combined.o in the images segment; image.c
 * reads textures out of it with romCopy(&_imagesSegmentRomStart + offset). The blob
 * is emitted as C by assets/images/ge_images_segment.c, so this just hands back its
 * base -- no ROM, no DMA. */
extern unsigned char ge_images_segment[];

void *gePortImagesSegment(void)
{
    return ge_images_segment;
}

/* ---- audio segments ----------------------------------------------------- */

/* The five audio ROM segments (sfxctl, sfxtbl, instrumentsctl, instrumentstbl,
 * musicsampletbl) have no backing .o in ge007.ld -- they were raw blobs appended to
 * the ROM, so unlike the fonts there is no extracted C source to link.
 *
 * Audio is stubbed (see port_audio.c), so nothing ever interprets these bytes. What
 * music.c does need is well-defined address arithmetic: it sizes each bank as the
 * difference between consecutive segment starts. Five separate globals would have
 * unspecified relative order -- the difference could even come out negative -- so
 * they are slots in one array with a known layout, giving a positive, sane size.
 */
unsigned char gePortAudioSeg[5][1024];

/* ---- romCopy ------------------------------------------------------------ */

/* On the N64 this was a blocking PI DMA from the cartridge. Every asset the port
 * cares about is already in memory, so the "ROM address" is a real pointer and the
 * transfer is a memcpy. */
extern const unsigned int ge_images_segment_size;

void romCopy(void *dst, void *src, unsigned int len)  /* matches ramrom.h */
{
    if (dst == NULL || src == NULL || len == 0) {
        return;
    }

    /* Guard, not just a probe. On the N64 an over-long PI DMA read harmless cartridge
     * space; here it walks off the end of a C array. Clamp reads that leave the images
     * segment and say so, rather than SIGBUSing with no explanation. */
    {
        unsigned char *base = ge_images_segment;
        unsigned char *end  = base + ge_images_segment_size;
        unsigned char *s    = (unsigned char *)src;

        /* Only reads that start inside the images segment are checked. romCopy is
         * also used for fonts, obseg blobs and plain RAM-to-RAM moves, whose sources
         * are unrelated allocations -- rejecting those broke load_font_tables. */
        if (s >= base && s < end && (s + len) > end) {
            unsigned int avail = (unsigned int)(end - s);
            printf("[getv] romCopy CLAMPED: off=%u len=%u avail=%u (segment %u)\n",
                   (unsigned int)(s - base), len, avail, ge_images_segment_size);
            fflush(stdout);
            len = avail;
        }
    }

    memcpy(dst, src, len);
}
