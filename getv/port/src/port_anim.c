/* GoldenEye tvOS port — the animation-table loader.
 *
 * What the N64 did (initanitable.c, alloc_load_expand_ani_table):
 *   1. romCopy() one contiguous animation-data segment out of the cartridge,
 *   2. walk animation_table_ptrs1/2 -- arrays of byte offsets into that segment --
 *      rewriting each offset into a pointer,
 *   3. relocate two pointer fields inside each record (bitDescriptors, bitStream) by
 *      the segment base, and the `address` field by the animation-entries segment base.
 *
 * Step 2 cannot work natively. ModelAnimation has three pointers (address,
 * bitDescriptors, bitStream) and sub-word fields (u16 unk04, u8 unk06/unk07), so at
 * 64-bit the struct no longer overlays the 32-bit asset record at any offset. The
 * records must be converted, not relocated in place -- which is what
 * src/ge_asset_fileview.h exists for (ModelAnimation_file is the 32-bit view, and its
 * layout is asserted equal to the N64's).
 *
 * The two segments are rebuilt as contiguous blobs by tools/gen_anim_blobs.py, which
 * verifies every one of the 346 PTR_ANIM_* / PTR_ANIM_ENTRY_* offsets against the
 * concatenation before it will emit anything. They are stored big-endian because the
 * animation payload is read byte-wise through bitStream; decoding below is therefore
 * done with explicit big-endian reads and is endian-independent.
 */
#include <stdio.h>
#include <string.h>

#include <stdint.h>

#include <PR/ultratypes.h>

/* Generated blobs (assets/ge_animation_*_segment.c). */
extern unsigned char ge_animation_data_segment[];
extern const unsigned int ge_animation_data_segment_size;
extern unsigned char ge_animation_entries_segment[];
extern const unsigned int ge_animation_entries_segment_size;


/* Mirror of the fields this file fills in. Declared locally rather than including
 * bondtypes.h: the port TUs deliberately exclude the decomp's include/ (its
 * math.h/string.h/stddef.h shadow the system headers). The layout below must match
 * ModelAnimation in src/bondtypes.h -- gePortAnimSelfCheck() asserts the size the
 * caller sees, so a drift is caught at boot rather than silently mis-decoded. */
typedef struct GeAnim {
    u8  *address;
    u16  unk04;
    u8   unk06;
    u8   unk07;
    void *bitDescriptors;
    u16  unk0C;
    u16  unk0E;
    u8  *bitStream;
    s32  unk14, unk18, unk1c, unk20, unk24, unk28, unk2c, unk30, unk34, unk38, unk3c;
} GeAnim;

#define GE_ANIM_FILE_SIZE 64        /* sizeof(ModelAnimation_file), asserted 21/21 */
#define GE_ANIM_MAX       256

static GeAnim ge_anim_records[GE_ANIM_MAX];
static unsigned int ge_anim_offsets[GE_ANIM_MAX];
static int ge_anim_count = 0;

static u32 be32(const unsigned char *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* Decode one 32-bit asset record at `off` in the data segment into a native record.
 * Field positions come from ModelAnimation_file, i.e. the N64 layout. */
static void ge_anim_decode(GeAnim *out, unsigned int off)
{
    const unsigned char *r = ge_animation_data_segment + off;
    u32 w0 = be32(r + 0), w1 = be32(r + 4), w2 = be32(r + 8);
    u32 w3 = be32(r + 12), w4 = be32(r + 16);
    int i;

    memset(out, 0, sizeof(*out));

    /* w0 is a byte offset into the entries segment. */
    out->address        = (w0 < ge_animation_entries_segment_size)
                          ? ge_animation_entries_segment + w0 : NULL;
    out->unk04          = (u16)(w1 >> 16);
    out->unk06          = (u8)((w1 >> 8) & 0xFF);
    out->unk07          = (u8)(w1 & 0xFF);
    /* w2 / w4 are byte offsets into the data segment (what expand_ani_table_entries
     * relocated by &ptr_animation_table->data). */
    /* Do not add a zero-check here. Offset 0 is a valid offset: ANIM_DATA_idle's bit
     * descriptors live at the start of the segment, and the N64 relocation was an
     * unconditional `unk08 += base`. Treating 0 as "absent" returns NULL, and
     * modelAnimReadRootMotionValue() does `desc = anim->bitDescriptors + fieldIndex`
     * followed by `desc->bitCount` -- an immediate SIGSEGV on the first animation. */
    out->bitDescriptors = (w2 < ge_animation_data_segment_size)
                          ? (void *)(ge_animation_data_segment + w2) : NULL;
    out->unk0C          = (u16)(w3 >> 16);
    out->unk0E          = (u16)(w3 & 0xFFFF);
    out->bitStream      = (w4 < ge_animation_data_segment_size)
                          ? ge_animation_data_segment + w4 : NULL;


    for (i = 0; i < 11; i++) {
        (&out->unk14)[i] = (s32)be32(r + 20 + i * 4);
    }
}

/* Offset -> native record. Converts on first use and caches, so repeated lookups from
 * the ANIM_PTR()-style call sites are cheap and always return the same pointer (the
 * game compares animation pointers for identity, e.g. bondview2's death-anim check). */
void *gePortAnimFromOffset(unsigned int off);

/* Every call site now passes a genuine byte offset: PTR_ANIM_* constants, or the
 * generated GE_ANIMOFF_<symbol> macros (see tools/gen_anim_blobs.py). The old
 * `&ANIM_DATA_x` form is gone -- those symbols are not linked in this build and were
 * resolving to poisoned link stubs, which is what made ANIM_PTR() hand out garbage.
 * Anything at or beyond the segment size is therefore a bug, and says so. */
void *gePortAnimFromValue(uintptr_t v)
{
    if (v >= ge_animation_data_segment_size) {
        static int warned = 0;
        if (warned < 8) {
            printf("[getv] anim: value 0x%lx is not a valid animation offset "
                   "(segment is %u bytes)\n",
                   (unsigned long)v, ge_animation_data_segment_size);
            fflush(stdout);
            warned++;
        }
        return NULL;
    }
    return gePortAnimFromOffset((unsigned int)v);
}

void *gePortAnimFromOffset(unsigned int off)
{
    int i;

    if (off >= ge_animation_data_segment_size) {
        return NULL;
    }
    for (i = 0; i < ge_anim_count; i++) {
        if (ge_anim_offsets[i] == off) {
            return &ge_anim_records[i];
        }
    }
    if (ge_anim_count >= GE_ANIM_MAX) {
        printf("[getv] anim: table full (%d), offset 0x%x dropped\n", GE_ANIM_MAX, off);
        fflush(stdout);
        return NULL;
    }
    ge_anim_offsets[ge_anim_count] = off;
    ge_anim_decode(&ge_anim_records[ge_anim_count], off);
    return &ge_anim_records[ge_anim_count++];
}

void *gePortAnimDataSegment(void)    { return ge_animation_data_segment; }
void *gePortAnimEntriesSegment(void) { return ge_animation_entries_segment; }

/* Called from the port's loader. `animsize` is what the caller believes
 * sizeof(ModelAnimation) to be; a mismatch means bondtypes.h drifted from GeAnim above
 * and every decoded field would be silently wrong. */
void gePortAnimInit(unsigned int animsize)
{
    ge_anim_count = 0;
    printf("[getv] anim table: data %u bytes, entries %u bytes, record %u bytes\n",
           ge_animation_data_segment_size, ge_animation_entries_segment_size,
           (unsigned)sizeof(GeAnim));
    if (animsize != sizeof(GeAnim)) {
        printf("[getv] *** ANIM LAYOUT DRIFT: caller says sizeof(ModelAnimation)=%u, "
               "port GeAnim=%u -- port_anim.c must be updated to match bondtypes.h\n",
               animsize, (unsigned)sizeof(GeAnim));
    }
    fflush(stdout);
}

/* ---- decoder bounds ------------------------------------------------------ */

/* The animation decoder (modelAnimReadRootMotionValue) indexes
 *   desc    = anim->bitDescriptors + fieldIndex
 *   byteptr = anim->bitStream + byteIndex
 * with no bound check -- correct on the N64 because the data guaranteed the ranges. Here
 * a mis-decoded record or an out-of-range joint would read past the segment, so callers
 * can ask whether a pointer is still inside it.
 *
 * Returns 1 if [p, p+len) lies within the animation data segment. */
int gePortAnimDataContains(const void *p, unsigned int len)
{
    const unsigned char *q = (const unsigned char *)p;

    if (q == NULL) {
        return 0;
    }
    return q >= ge_animation_data_segment
        && (q + len) <= (ge_animation_data_segment + ge_animation_data_segment_size);
}
