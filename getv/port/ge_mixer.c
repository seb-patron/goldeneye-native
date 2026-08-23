/* GoldenEye tvOS port -- the RSP audio microcode (aspMain), in software.
 *
 * This file is sm64ex's src/pc/mixer.c (Emill's implementation of the N64 audio
 * microcode) with four changes, all noted inline. It drops in unmodified in every
 * other respect because GoldenEye's audio ABI is the same one:
 *
 *   diff <(grep -oE '^#define\s+a[A-Za-z0-9_]+\([^)]*\)' ge-decomp/include/PR/abi.h) \
 *        <(grep -oE '^#define\s+a[A-Za-z0-9_]+\([^)]*\)' sm64ex/include/PR/abi.h)
 *
 * is empty apart from sm64's extra aSetVolume32. Both games drive stock libultra
 * `aspMain`, so the command set, its argument order and its DMEM semantics match.
 * (Perfect Dark's port carries a variant of this same file, but PD uses Rare's
 * N-Audio ABI where the buffer parameters are folded into each command, so that
 * variant would not fit here.)
 *
 * Changes from sm64ex:
 *   1. Headers: the decomp's include/ is not on the port include path (it shadows
 *      the system headers), so pull the types from <PR/ultratypes.h> + <PR/abi.h>.
 *   2. The DMEM buffer is grown -- see GE_DMEM_SIZE below.
 *   3. aPoleFilterImpl added: GoldenEye's reverb uses A_POLEF, SM64 never did.
 *   4. aSetVolume32 is dropped (sm64-only, and nothing here emits it).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/abi.h>

#include "ge_mixer.h"

#ifdef __SSE4_1__
#include <immintrin.h>
#define HAS_SSE41 1
#define HAS_NEON 0
#elif __ARM_NEON
#include <arm_neon.h>
#define HAS_SSE41 0
#define HAS_NEON 1
#else
#define HAS_SSE41 0
#define HAS_NEON 0
#endif

#pragma GCC optimize ("unroll-loops")

#if HAS_SSE41
#define LOADLH(l, h) _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd((const double *)(l)), (const double *)(h)))
#endif

#define GE_DMEM_SIZE 4096
/* AL_MAX_RSP_SAMPLES(160) * 2 bytes -- the size of every buffer in GE's DMEM map, so
 * the largest span an A_AUX address can legally cover. */
#define AL_MAX_RSP_SAMPLES_BYTES 320

#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)

/* Probe: whether the synth actually ran. `cmdLen == 0` does not prove the early return
 * -- the a* macros discard their `pkt` argument, so the AL library's write cursor never
 * advances and cmdLen comes back 0 even on a full synthesis pass. */
unsigned long ge_mixer_ops = 0;
unsigned long ge_mixer_saves = 0;
unsigned long ge_mixer_hi = 0;
unsigned long ge_mixer_overflow = 0;
/* The FIRST aSetBuffer that exceeded DMEM, so an overflow count can be attributed
 * instead of guessed at. */
unsigned long ge_mixer_ovf_flags = 0;
unsigned long ge_mixer_ovf_in = 0;
unsigned long ge_mixer_ovf_out = 0;
unsigned long ge_mixer_ovf_nbytes = 0;

/* ---------------------------------------------------------------- signal probe --
 * When the synth runs and the sequence player reports playing voices but the PCM handed
 * to SDL is all zeros, the signal is dying somewhere inside this file. Rather than
 * guess which stage, measure every stage: each probe is the peak absolute sample that
 * stage wrote. The first zero in the chain, reading
 * adpcm -> resample -> envmix -> mix -> interleave -> save, is the stage that killed
 * it.
 *
 * `ge_mixer_probe` is set from port_audio.c's GETV_AUDIO_DEBUG, so this costs
 * nothing when the diagnostic is off. */
int ge_mixer_probe = 0;
unsigned long ge_mixer_n_adpcm = 0, ge_mixer_n_resample = 0, ge_mixer_n_envmix = 0;
unsigned long ge_mixer_n_mix = 0, ge_mixer_n_interleave = 0;
int ge_mixer_pk_adpcm = 0, ge_mixer_pk_resample = 0, ge_mixer_pk_envmix = 0;
int ge_mixer_pk_mix = 0, ge_mixer_pk_interleave = 0, ge_mixer_pk_save = 0;
int ge_mixer_pk_loadbuf = 0;
int ge_mixer_pk_polef = 0;
/* Envmix is the usual place the signal dies, so record its inputs on the first call of
 * each frame: the peak of the input span, and the four volume scalars aSetVolume left
 * in rspa. That splits "the input never arrived" from "the input arrived and a volume
 * of zero multiplied it away". */
int ge_mixer_pk_envmix_in = 0;
int ge_mixer_em_vol0 = -1, ge_mixer_em_vol1 = -1;
int ge_mixer_em_dry = -1, ge_mixer_em_wet = -1;
int ge_mixer_em_tgt0 = -1, ge_mixer_em_rate0 = -1;
int ge_mixer_em_flags = -1;

static void ge_probe_peak(const int16_t *p, int nsamples, int *dst)
{
    int i, pk = *dst;
    for (i = 0; i < nsamples; i++) {
        int v = p[i];
        if (v < 0) { v = -v; }
        if (v > pk) { pk = v; }
    }
    *dst = pk;
}

static struct {
    uint16_t in;
    uint16_t out;
    uint16_t nbytes;

    int16_t vol[2];

    uint16_t dry_right;
    uint16_t wet_left;
    uint16_t wet_right;

    int16_t target[2];
    int32_t rate[2];

    int16_t vol_dry;
    int16_t vol_wet;

    ADPCM_STATE *adpcm_loop_state;

    int16_t adpcm_table[8][2][8];
    /* GoldenEye's DMEM map (libultra src/audio/synthInternals.h) puts AL_AUX_R_OUT at
     * 2048 and each buffer is AL_MAX_RSP_SAMPLES(160)*2 = 320 bytes, so the highest
     * address touched is 2368 -- above SM64's 2512 only after alignment round-ups.
     * Perfect Dark's copy of this file uses 3072 "2720 + slack" for the same reason.
     * 4096 is the real RSP DMEM size; use it and stop worrying. */
    union {
        int16_t as_s16[GE_DMEM_SIZE / sizeof(int16_t)];
        uint8_t as_u8[GE_DMEM_SIZE];
    } buf;
} rspa;

static int16_t resample_table[64][4] = {
    {0x0c39, 0x66ad, 0x0d46, 0xffdf}, {0x0b39, 0x6696, 0x0e5f, 0xffd8},
    {0x0a44, 0x6669, 0x0f83, 0xffd0}, {0x095a, 0x6626, 0x10b4, 0xffc8},
    {0x087d, 0x65cd, 0x11f0, 0xffbf}, {0x07ab, 0x655e, 0x1338, 0xffb6},
    {0x06e4, 0x64d9, 0x148c, 0xffac}, {0x0628, 0x643f, 0x15eb, 0xffa1},
    {0x0577, 0x638f, 0x1756, 0xff96}, {0x04d1, 0x62cb, 0x18cb, 0xff8a},
    {0x0435, 0x61f3, 0x1a4c, 0xff7e}, {0x03a4, 0x6106, 0x1bd7, 0xff71},
    {0x031c, 0x6007, 0x1d6c, 0xff64}, {0x029f, 0x5ef5, 0x1f0b, 0xff56},
    {0x022a, 0x5dd0, 0x20b3, 0xff48}, {0x01be, 0x5c9a, 0x2264, 0xff3a},
    {0x015b, 0x5b53, 0x241e, 0xff2c}, {0x0101, 0x59fc, 0x25e0, 0xff1e},
    {0x00ae, 0x5896, 0x27a9, 0xff10}, {0x0063, 0x5720, 0x297a, 0xff02},
    {0x001f, 0x559d, 0x2b50, 0xfef4}, {0xffe2, 0x540d, 0x2d2c, 0xfee8},
    {0xffac, 0x5270, 0x2f0d, 0xfedb}, {0xff7c, 0x50c7, 0x30f3, 0xfed0},
    {0xff53, 0x4f14, 0x32dc, 0xfec6}, {0xff2e, 0x4d57, 0x34c8, 0xfebd},
    {0xff0f, 0x4b91, 0x36b6, 0xfeb6}, {0xfef5, 0x49c2, 0x38a5, 0xfeb0},
    {0xfedf, 0x47ed, 0x3a95, 0xfeac}, {0xfece, 0x4611, 0x3c85, 0xfeab},
    {0xfec0, 0x4430, 0x3e74, 0xfeac}, {0xfeb6, 0x424a, 0x4060, 0xfeaf},
    {0xfeaf, 0x4060, 0x424a, 0xfeb6}, {0xfeac, 0x3e74, 0x4430, 0xfec0},
    {0xfeab, 0x3c85, 0x4611, 0xfece}, {0xfeac, 0x3a95, 0x47ed, 0xfedf},
    {0xfeb0, 0x38a5, 0x49c2, 0xfef5}, {0xfeb6, 0x36b6, 0x4b91, 0xff0f},
    {0xfebd, 0x34c8, 0x4d57, 0xff2e}, {0xfec6, 0x32dc, 0x4f14, 0xff53},
    {0xfed0, 0x30f3, 0x50c7, 0xff7c}, {0xfedb, 0x2f0d, 0x5270, 0xffac},
    {0xfee8, 0x2d2c, 0x540d, 0xffe2}, {0xfef4, 0x2b50, 0x559d, 0x001f},
    {0xff02, 0x297a, 0x5720, 0x0063}, {0xff10, 0x27a9, 0x5896, 0x00ae},
    {0xff1e, 0x25e0, 0x59fc, 0x0101}, {0xff2c, 0x241e, 0x5b53, 0x015b},
    {0xff3a, 0x2264, 0x5c9a, 0x01be}, {0xff48, 0x20b3, 0x5dd0, 0x022a},
    {0xff56, 0x1f0b, 0x5ef5, 0x029f}, {0xff64, 0x1d6c, 0x6007, 0x031c},
    {0xff71, 0x1bd7, 0x6106, 0x03a4}, {0xff7e, 0x1a4c, 0x61f3, 0x0435},
    {0xff8a, 0x18cb, 0x62cb, 0x04d1}, {0xff96, 0x1756, 0x638f, 0x0577},
    {0xffa1, 0x15eb, 0x643f, 0x0628}, {0xffac, 0x148c, 0x64d9, 0x06e4},
    {0xffb6, 0x1338, 0x655e, 0x07ab}, {0xffbf, 0x11f0, 0x65cd, 0x087d},
    {0xffc8, 0x10b4, 0x6626, 0x095a}, {0xffd0, 0x0f83, 0x6669, 0x0a44},
    {0xffd8, 0x0e5f, 0x6696, 0x0b39}, {0xffdf, 0x0d46, 0x66ad, 0x0c39}
};

static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000) {
        return -0x8000;
    } else if (v > 0x7fff) {
        return 0x7fff;
    }
    return (int16_t)v;
}

static inline int32_t clamp32(int64_t v) {
    if (v < -0x7fffffff - 1) {
        return -0x7fffffff - 1;
    } else if (v > 0x7fffffff) {
        return 0x7fffffff;
    }
    return (int32_t)v;
}

void aClearBufferImpl(uint16_t addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memset(rspa.buf.as_u8 + addr, 0, nbytes);
}

void aLoadBufferImpl(const void *source_addr) {
    memcpy(rspa.buf.as_u8 + rspa.in, source_addr, ROUND_UP_8(rspa.nbytes));
    /* Probed as s16 even though ADPCM is nibble-packed: this is only asking "did any
     * non-zero BYTES arrive from DRAM", which answers whether the DMA/wavetable
     * pointer is right, and that question is upstream of every codec detail. */
    if (ge_mixer_probe) {
        ge_probe_peak(rspa.buf.as_s16 + rspa.in / sizeof(int16_t),
                      ROUND_UP_8(rspa.nbytes) / 2, &ge_mixer_pk_loadbuf);
    }
}

void aSaveBufferImpl(int16_t *dest_addr) {
    ge_mixer_saves++;
    memcpy(dest_addr, rspa.buf.as_s16 + rspa.out / sizeof(int16_t), ROUND_UP_8(rspa.nbytes));
    if (ge_mixer_probe) {
        ge_probe_peak(dest_addr, ROUND_UP_8(rspa.nbytes) / 2, &ge_mixer_pk_save);
    }
}

void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr) {
    memcpy(rspa.adpcm_table, book_source_addr, num_entries_times_16);
}

void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    ge_mixer_ops++;
    /* Probe: the RSP's DMEM is a fixed 4 KB and every Impl below indexes rspa.buf with
     * these three values. GoldenEye's DMEM map tops out at AL_AUX_R_OUT(2048)+320, so
     * this should never fire -- which is exactly why it is worth checking rather than
     * assuming. An overrun here writes straight past rspa.buf into whatever BSS the
     * linker put next, which is the one mechanism that would explain a NULL appearing
     * in another translation unit's statics. */
    {
        /* The three arguments mean different things depending on A_AUX, so each form
         * has to be measured on its own terms. Without A_AUX they are
         * (in, out, nbytes) and the top address touched is addr+nbytes. With A_AUX
         * they are three separate DMEM addresses (dry_right, wet_left, wet_right);
         * treating the third as a length and adding it to the second reports a top of
         * 4192 for the entirely legal aSetBuffer(A_AUX, 1408, 1728, 2048), i.e. the
         * probe manufactures its own overflow. */
        unsigned hi;
        if (flags & A_AUX) {
            unsigned a = (unsigned)in, b = (unsigned)out, c = (unsigned)nbytes;
            hi = a > b ? a : b;
            if (c > hi) { hi = c; }
            hi += AL_MAX_RSP_SAMPLES_BYTES + 32;
        } else {
            unsigned hi_in  = (unsigned)in  + (unsigned)nbytes + 32;
            unsigned hi_out = (unsigned)out + (unsigned)nbytes + 32;
            hi = hi_in > hi_out ? hi_in : hi_out;
        }
        if (hi > ge_mixer_hi) { ge_mixer_hi = hi; }
        if (hi > GE_DMEM_SIZE) {
            if (ge_mixer_overflow == 0) {
                ge_mixer_ovf_flags  = flags;
                ge_mixer_ovf_in     = in;
                ge_mixer_ovf_out    = out;
                ge_mixer_ovf_nbytes = nbytes;
            }
            ge_mixer_overflow++;
        }
    }
    if (flags & A_AUX) {
        rspa.dry_right = in;
        rspa.wet_left = out;
        rspa.wet_right = nbytes;
    } else {
        rspa.in = in;
        rspa.out = out;
        rspa.nbytes = nbytes;
    }
}

void aSetVolumeImpl(uint8_t flags, int16_t v, int16_t t, int16_t r) {
    if (flags & A_AUX) {
        rspa.vol_dry = v;
        rspa.vol_wet = r;
    } else if (flags & A_VOL) {
        if (flags & A_LEFT) {
            rspa.vol[0] = v;
        } else {
            rspa.vol[1] = v;
        }
    } else {
        if (flags & A_LEFT) {
            rspa.target[0] = v;
            rspa.rate[0] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
        } else {
            rspa.target[1] = v;
            rspa.rate[1] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
        }
    }
}

void aInterleaveImpl(uint16_t left, uint16_t right) {
    int count = ROUND_UP_16(rspa.nbytes) / sizeof(int16_t) / 8;
    int16_t *l = rspa.buf.as_s16 + left / sizeof(int16_t);
    int16_t *r = rspa.buf.as_s16 + right / sizeof(int16_t);
    int16_t *d = rspa.buf.as_s16 + rspa.out / sizeof(int16_t);
    while (count > 0) {
        int16_t l0 = *l++;
        int16_t l1 = *l++;
        int16_t l2 = *l++;
        int16_t l3 = *l++;
        int16_t l4 = *l++;
        int16_t l5 = *l++;
        int16_t l6 = *l++;
        int16_t l7 = *l++;
        int16_t r0 = *r++;
        int16_t r1 = *r++;
        int16_t r2 = *r++;
        int16_t r3 = *r++;
        int16_t r4 = *r++;
        int16_t r5 = *r++;
        int16_t r6 = *r++;
        int16_t r7 = *r++;
        *d++ = l0;
        *d++ = r0;
        *d++ = l1;
        *d++ = r1;
        *d++ = l2;
        *d++ = r2;
        *d++ = l3;
        *d++ = r3;
        *d++ = l4;
        *d++ = r4;
        *d++ = l5;
        *d++ = r5;
        *d++ = l6;
        *d++ = r6;
        *d++ = l7;
        *d++ = r7;
        --count;
    }
    if (ge_mixer_probe) {
        ge_mixer_n_interleave++;
        ge_probe_peak(rspa.buf.as_s16 + rspa.out / sizeof(int16_t), ROUND_UP_16(rspa.nbytes), &ge_mixer_pk_interleave);
    }
}

void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memmove(rspa.buf.as_u8 + out_addr, rspa.buf.as_u8 + in_addr, nbytes);
}

void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state) {
    rspa.adpcm_loop_state = adpcm_loop_state;
}

void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) {
#if HAS_SSE41
    const __m128i tblrev = _mm_setr_epi8(12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1, -1, -1);
    const __m128i pos0 = _mm_set_epi8(3, -1, 3, -1, 2, -1, 2, -1, 1, -1, 1, -1, 0, -1, 0, -1);
    const __m128i pos1 = _mm_set_epi8(7, -1, 7, -1, 6, -1, 6, -1, 5, -1, 5, -1, 4, -1, 4, -1);
    const __m128i mult = _mm_set_epi16(0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01);
    const __m128i mask = _mm_set1_epi16((int16_t)0xf000);
#elif HAS_NEON
    static const int8_t pos0_data[] = {-1, 0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3};
    static const int8_t pos1_data[] = {-1, 4, -1, 4, -1, 5, -1, 5, -1, 6, -1, 6, -1, 7, -1, 7};
    static const int16_t mult_data[] = {0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10};
    static const int16_t table_prefix_data[] = {0, 0, 0, 0, 0, 0, 0, 1 << 11};
    const int8x16_t pos0 = vld1q_s8(pos0_data);
    const int8x16_t pos1 = vld1q_s8(pos1_data);
    const int16x8_t mult = vld1q_s16(mult_data);
    const int16x8_t mask = vdupq_n_s16((int16_t)0xf000);
    const int16x8_t table_prefix = vld1q_s16(table_prefix_data);
#endif
    uint8_t *in = rspa.buf.as_u8 + rspa.in;
    int16_t *out = rspa.buf.as_s16 + rspa.out / sizeof(int16_t);
    int nbytes = ROUND_UP_32(rspa.nbytes);
    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;
#if HAS_SSE41
    __m128i prev_interleaved = _mm_set1_epi32((uint16_t)out[-2] | ((uint16_t)out[-1] << 16));
    //__m128i prev_interleaved = _mm_shuffle_epi32(_mm_loadu_si32(out - 2), 0); // GCC misses this?
#elif HAS_NEON
    int16x8_t result = vld1q_s16(out - 8);
#endif
    while (nbytes > 0) {
        int shift = *in >> 4; // should be in 0..12
        int table_index = *in++ & 0xf; // should be in 0..7
        int16_t (*tbl)[8] = rspa.adpcm_table[table_index];
        int i;
#if HAS_SSE41
        // The _mm_loadu_si64 instruction was added in GCC 9, and results in the same
        // asm as the following instructions, so better be compatible with old GCC.
        //__m128i inv = _mm_loadu_si64(in);
        uint64_t v; memcpy(&v, in, 8);
        __m128i inv = _mm_set_epi64x(0, v);
        __m128i invec[2] = {_mm_shuffle_epi8(inv, pos0), _mm_shuffle_epi8(inv, pos1)};
        __m128i tblvec0 = _mm_loadu_si128((const __m128i *)tbl[0]);
        __m128i tblvec1 = _mm_loadu_si128((const __m128i *)(tbl[1]));
        __m128i tbllo = _mm_unpacklo_epi16(tblvec0, tblvec1);
        __m128i tblhi = _mm_unpackhi_epi16(tblvec0, tblvec1);
        __m128i shiftcount = _mm_set_epi64x(0, 12 - shift); // _mm_cvtsi64_si128 does not exist on 32-bit x86
        __m128i tblvec1_rev[8];

        tblvec1_rev[0] = _mm_insert_epi16(_mm_shuffle_epi8(tblvec1, tblrev), 1 << 11, 7);
        tblvec1_rev[1] = _mm_bsrli_si128(tblvec1_rev[0], 2);
        tblvec1_rev[2] = _mm_bsrli_si128(tblvec1_rev[0], 4);
        tblvec1_rev[3] = _mm_bsrli_si128(tblvec1_rev[0], 6);
        tblvec1_rev[4] = _mm_bsrli_si128(tblvec1_rev[0], 8);
        tblvec1_rev[5] = _mm_bsrli_si128(tblvec1_rev[0], 10);
        tblvec1_rev[6] = _mm_bsrli_si128(tblvec1_rev[0], 12);
        tblvec1_rev[7] = _mm_bsrli_si128(tblvec1_rev[0], 14);
        in += 8;
        for (i = 0; i < 2; i++) {
            __m128i acc0 = _mm_madd_epi16(prev_interleaved, tbllo);
            __m128i acc1 = _mm_madd_epi16(prev_interleaved, tblhi);
            __m128i muls[8];
            __m128i result;
            invec[i] = _mm_sra_epi16(_mm_and_si128(_mm_mullo_epi16(invec[i], mult), mask), shiftcount);

            muls[7] = _mm_madd_epi16(tblvec1_rev[0], invec[i]);
            muls[6] = _mm_madd_epi16(tblvec1_rev[1], invec[i]);
            muls[5] = _mm_madd_epi16(tblvec1_rev[2], invec[i]);
            muls[4] = _mm_madd_epi16(tblvec1_rev[3], invec[i]);
            muls[3] = _mm_madd_epi16(tblvec1_rev[4], invec[i]);
            muls[2] = _mm_madd_epi16(tblvec1_rev[5], invec[i]);
            muls[1] = _mm_madd_epi16(tblvec1_rev[6], invec[i]);
            muls[0] = _mm_madd_epi16(tblvec1_rev[7], invec[i]);

            acc0 = _mm_add_epi32(acc0, _mm_hadd_epi32(_mm_hadd_epi32(muls[0], muls[1]), _mm_hadd_epi32(muls[2], muls[3])));
            acc1 = _mm_add_epi32(acc1, _mm_hadd_epi32(_mm_hadd_epi32(muls[4], muls[5]), _mm_hadd_epi32(muls[6], muls[7])));

            acc0 = _mm_srai_epi32(acc0, 11);
            acc1 = _mm_srai_epi32(acc1, 11);

            result = _mm_packs_epi32(acc0, acc1);
            _mm_storeu_si128((__m128i *)out, result);
            out += 8;

            prev_interleaved = _mm_shuffle_epi32(result, _MM_SHUFFLE(3, 3, 3, 3));
        }
#elif HAS_NEON
        int8x8_t inv = vld1_s8((int8_t *)in);
        int16x8_t tblvec[2] = {vld1q_s16(tbl[0]), vld1q_s16(tbl[1])};
        int16x8_t invec[2] = {vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos0)),
                                                               vtbl1_s8(inv, vget_high_s8(pos0)))),
                              vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos1)),
                                                               vtbl1_s8(inv, vget_high_s8(pos1))))};
        int16x8_t shiftcount = vdupq_n_s16(shift - 12); // negative means right shift
        int16x8_t tblvec1[8];

        in += 8;
        tblvec1[0] = vextq_s16(table_prefix, tblvec[1], 7);
        invec[0] = vmulq_s16(invec[0], mult);
        tblvec1[1] = vextq_s16(table_prefix, tblvec[1], 6);
        invec[1] = vmulq_s16(invec[1], mult);
        tblvec1[2] = vextq_s16(table_prefix, tblvec[1], 5);
        tblvec1[3] = vextq_s16(table_prefix, tblvec[1], 4);
        invec[0] = vandq_s16(invec[0], mask);
        tblvec1[4] = vextq_s16(table_prefix, tblvec[1], 3);
        invec[1] = vandq_s16(invec[1], mask);
        tblvec1[5] = vextq_s16(table_prefix, tblvec[1], 2);
        tblvec1[6] = vextq_s16(table_prefix, tblvec[1], 1);
        invec[0] = vqshlq_s16(invec[0], shiftcount);
        invec[1] = vqshlq_s16(invec[1], shiftcount);
        tblvec1[7] = table_prefix;
        for (i = 0; i < 2; i++) {
            int32x4_t acc0;
            int32x4_t acc1;

            acc1 = vmull_lane_s16(vget_high_s16(tblvec[0]), vget_high_s16(result), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec[1]), vget_high_s16(result), 3);
            acc0 = vmull_lane_s16(vget_low_s16(tblvec[0]), vget_high_s16(result), 2);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec[1]), vget_high_s16(result), 3);

            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[0]), vget_low_s16(invec[i]), 0);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[1]), vget_low_s16(invec[i]), 1);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[2]), vget_low_s16(invec[i]), 2);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[3]), vget_low_s16(invec[i]), 3);

            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[0]), vget_low_s16(invec[i]), 0);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[1]), vget_low_s16(invec[i]), 1);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[2]), vget_low_s16(invec[i]), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[3]), vget_low_s16(invec[i]), 3);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[4]), vget_high_s16(invec[i]), 0);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[5]), vget_high_s16(invec[i]), 1);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[6]), vget_high_s16(invec[i]), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[7]), vget_high_s16(invec[i]), 3);

            result = vcombine_s16(vqshrn_n_s32(acc0, 11), vqshrn_n_s32(acc1, 11));
            vst1q_s16(out, result);
            out += 8;
        }
#else
        for (i = 0; i < 2; i++) {
            int16_t ins[8];
            int16_t prev1 = out[-1];
            int16_t prev2 = out[-2];
            int j, k;
            for (j = 0; j < 4; j++) {
                ins[j * 2] = (((*in >> 4) << 28) >> 28) << shift;
                ins[j * 2 + 1] = (((*in++ & 0xf) << 28) >> 28) << shift;
            }
            for (j = 0; j < 8; j++) {
                int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);
                for (k = 0; k < j; k++) {
                    acc += tbl[1][((j - k) - 1)] * ins[k];
                }
                acc >>= 11;
                *out++ = clamp16(acc);
            }
        }
#endif
        nbytes -= 16 * sizeof(int16_t);
    }
    memcpy(state, out - 16, 16 * sizeof(int16_t));
    if (ge_mixer_probe) {
        ge_mixer_n_adpcm++;
        ge_probe_peak(rspa.buf.as_s16 + rspa.out / sizeof(int16_t), ROUND_UP_32(rspa.nbytes) / 2, &ge_mixer_pk_adpcm);
    }

}

void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    int16_t tmp[16];
    int16_t *in_initial = rspa.buf.as_s16 + rspa.in / sizeof(int16_t);
    int16_t *in = in_initial;
    int16_t *out = rspa.buf.as_s16 + rspa.out / sizeof(int16_t);
    int nbytes = ROUND_UP_16(rspa.nbytes);
    uint32_t pitch_accumulator;
    int i;
#if !HAS_SSE41 && !HAS_NEON
    int16_t *tbl;
    int32_t sample;
#endif
    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(int16_t));
    } else {
        memcpy(tmp, state, 16 * sizeof(int16_t));
    }
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / sizeof(int16_t);
    }
    in -= 4;
    pitch_accumulator = (uint16_t)tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));

#if HAS_SSE41
    __m128i multiples = _mm_setr_epi16(0, 2, 4, 6, 8, 10, 12, 14);
    __m128i pitchvec = _mm_set1_epi16((int16_t)pitch);
    __m128i pitchvec_8_steps = _mm_set1_epi32((pitch << 1) * 8);
    __m128i pitchacclo_vec = _mm_set1_epi32((uint16_t)pitch_accumulator);
    __m128i pl = _mm_mullo_epi16(multiples, pitchvec);
    __m128i ph = _mm_mulhi_epu16(multiples, pitchvec);
    __m128i acc_a = _mm_add_epi32(_mm_unpacklo_epi16(pl, ph), pitchacclo_vec);
    __m128i acc_b = _mm_add_epi32(_mm_unpackhi_epi16(pl, ph), pitchacclo_vec);

    do {
        __m128i tbl_positions = _mm_srli_epi16(_mm_packus_epi32(
            _mm_and_si128(acc_a, _mm_set1_epi32(0xffff)),
            _mm_and_si128(acc_b, _mm_set1_epi32(0xffff))), 10);

        __m128i in_positions = _mm_packus_epi32(_mm_srli_epi32(acc_a, 16), _mm_srli_epi32(acc_b, 16));
        __m128i tbl_entries[4];
        __m128i samples[4];

        /*for (i = 0; i < 4; i++) {
            tbl_entries[i] = _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd(
                (const double *)resample_table[_mm_extract_epi16(tbl_positions, 2 * i)]),
                (const double *)resample_table[_mm_extract_epi16(tbl_positions, 2 * i + 1)]));

            samples[i] = _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd(
                (const double *)&in[_mm_extract_epi16(in_positions, 2 * i)]),
                (const double *)&in[_mm_extract_epi16(in_positions, 2 * i + 1)]));

            samples[i] = _mm_mulhrs_epi16(samples[i], tbl_entries[i]);
        }*/
        tbl_entries[0] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 0)], resample_table[_mm_extract_epi16(tbl_positions, 1)]);
        tbl_entries[1] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 2)], resample_table[_mm_extract_epi16(tbl_positions, 3)]);
        tbl_entries[2] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 4)], resample_table[_mm_extract_epi16(tbl_positions, 5)]);
        tbl_entries[3] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 6)], resample_table[_mm_extract_epi16(tbl_positions, 7)]);
        samples[0] = LOADLH(&in[_mm_extract_epi16(in_positions, 0)], &in[_mm_extract_epi16(in_positions, 1)]);
        samples[1] = LOADLH(&in[_mm_extract_epi16(in_positions, 2)], &in[_mm_extract_epi16(in_positions, 3)]);
        samples[2] = LOADLH(&in[_mm_extract_epi16(in_positions, 4)], &in[_mm_extract_epi16(in_positions, 5)]);
        samples[3] = LOADLH(&in[_mm_extract_epi16(in_positions, 6)], &in[_mm_extract_epi16(in_positions, 7)]);
        samples[0] = _mm_mulhrs_epi16(samples[0], tbl_entries[0]);
        samples[1] = _mm_mulhrs_epi16(samples[1], tbl_entries[1]);
        samples[2] = _mm_mulhrs_epi16(samples[2], tbl_entries[2]);
        samples[3] = _mm_mulhrs_epi16(samples[3], tbl_entries[3]);

        _mm_storeu_si128((__m128i *)out, _mm_hadds_epi16(_mm_hadds_epi16(samples[0], samples[1]), _mm_hadds_epi16(samples[2], samples[3])));

        acc_a = _mm_add_epi32(acc_a, pitchvec_8_steps);
        acc_b = _mm_add_epi32(acc_b, pitchvec_8_steps);
        out += 8;
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
    in += (uint16_t)_mm_extract_epi16(acc_a, 1);
    pitch_accumulator = (uint16_t)_mm_extract_epi16(acc_a, 0);
#elif HAS_NEON
    static const uint16_t multiples_data[8] = {0, 2, 4, 6, 8, 10, 12, 14};
    uint16x8_t multiples = vld1q_u16(multiples_data);
    uint32x4_t pitchvec_8_steps = vdupq_n_u32((pitch << 1) * 8);
    uint32x4_t pitchacclo_vec = vdupq_n_u32((uint16_t)pitch_accumulator);
    uint32x4_t acc_a = vmlal_n_u16(pitchacclo_vec, vget_low_u16(multiples), pitch);
    uint32x4_t acc_b = vmlal_n_u16(pitchacclo_vec, vget_high_u16(multiples), pitch);

    do {
        uint16x8x2_t unzipped = vuzpq_u16(vreinterpretq_u16_u32(acc_a), vreinterpretq_u16_u32(acc_b));
        uint16x8_t tbl_positions = vshrq_n_u16(unzipped.val[0], 10);
        uint16x8_t in_positions = unzipped.val[1];
        int16x8_t tbl_entries[4];
        int16x8_t samples[4];
        int16x8x2_t unzipped1;
        int16x8x2_t unzipped2;

        tbl_entries[0] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 0)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 1)]));
        tbl_entries[1] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 2)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 3)]));
        tbl_entries[2] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 4)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 5)]));
        tbl_entries[3] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 6)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 7)]));
        samples[0] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 0)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 1)]));
        samples[1] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 2)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 3)]));
        samples[2] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 4)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 5)]));
        samples[3] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 6)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 7)]));
        samples[0] = vqrdmulhq_s16(samples[0], tbl_entries[0]);
        samples[1] = vqrdmulhq_s16(samples[1], tbl_entries[1]);
        samples[2] = vqrdmulhq_s16(samples[2], tbl_entries[2]);
        samples[3] = vqrdmulhq_s16(samples[3], tbl_entries[3]);

        unzipped1 = vuzpq_s16(samples[0], samples[1]);
        unzipped2 = vuzpq_s16(samples[2], samples[3]);
        samples[0] = vqaddq_s16(unzipped1.val[0], unzipped1.val[1]);
        samples[1] = vqaddq_s16(unzipped2.val[0], unzipped2.val[1]);
        unzipped1 = vuzpq_s16(samples[0], samples[1]);
        samples[0] = vqaddq_s16(unzipped1.val[0], unzipped1.val[1]);

        vst1q_s16(out, samples[0]);

        acc_a = vaddq_u32(acc_a, pitchvec_8_steps);
        acc_b = vaddq_u32(acc_b, pitchvec_8_steps);
        out += 8;
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
    in += vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 1);
    pitch_accumulator = vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 0);
#else
    do {
        for (i = 0; i < 8; i++) {
            tbl = resample_table[pitch_accumulator * 64 >> 16];
            sample = ((in[0] * tbl[0] + 0x4000) >> 15) +
                     ((in[1] * tbl[1] + 0x4000) >> 15) +
                     ((in[2] * tbl[2] + 0x4000) >> 15) +
                     ((in[3] * tbl[3] + 0x4000) >> 15);
            *out++ = clamp16(sample);

            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            pitch_accumulator %= 0x10000;
        }
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
#endif

    state[4] = (int16_t)pitch_accumulator;
    memcpy(state, in, 4 * sizeof(int16_t));
    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(int16_t));
    if (ge_mixer_probe) {
        ge_mixer_n_resample++;
        ge_probe_peak(rspa.buf.as_s16 + rspa.out / sizeof(int16_t), ROUND_UP_16(rspa.nbytes) / 2, &ge_mixer_pk_resample);
    }
}


/* GoldenEye uses a linear envelope ramp, not the stock exponential one.
 *
 * Rare replaced libultra's audio library wholesale -- only three files ship in
 * `src/libultrare/audio/` where stock libultra has 43 -- and `env.c` computes the
 * `(ratem, ratel)` pair completely differently from stock:
 *
 *     env.c:_getRate()  tempf2 = ((tgt - vol) / count) * 8.0;     <- linear delta
 *     env.c:_getVol()   ivol  += (rate * samples) * 0.125;        <- linear accumulate
 *
 * Stock libultra instead computes a ratio `(tgt/vol)^(1/count)` and the RSP multiplies
 * by it. Both laws are packed into the same 32-bit `(ratem << 16) | ratel` field, so
 * nothing about the command encoding reveals which one is meant.
 *
 * So `rspa.rate[c]` here is a signed 16.16 fixed-point volume delta per 8 samples. Per
 * sample the step is `rate/8`; across one 8-sample group the volume moves by exactly
 * `rate`. `rspa.target[c]` clamps it, and the direction is the sign of rate, not
 * `rate >= 1.0`, which is the ratio-law test.
 *
 * This mixer was inherited from sm64ex, which implements the stock exponential law.
 * That is wrong here: with GE's values `rate_float` is nowhere near 1.0, so
 * `vol * (rate_float - 1)` and the repeated multiply drive the volume to the segment
 * target inside the first 8-sample group. Because `env.c` only re-issues A_INIT when a
 * control event starts a new segment (`e->first`), that collapses every multi-frame
 * attack, decay, fade and pan ramp in the game into a step.
 *
 * Cross-check: `mupen64plus-rsp-hle` identifies GE by ucode checksum (`hle.c`,
 * `case 0x1dc8138c`) and routes ENVMIXER to `alist_envmix_ge` -- linear, `step = rate/8`
 * per sample -- rather than `alist_envmix_exp`. Ship of Harkinian's mixer is not a valid
 * reference for this game: it is ABI2 (`aEnvSetup1/2`), which GE does not use.
 */
void aEnvMixerImpl(uint8_t flags, ENVMIX_STATE state) {
    int16_t *in = rspa.buf.as_s16 + rspa.in / sizeof(int16_t);
    if (ge_mixer_probe) {
        ge_probe_peak(in, ROUND_UP_16(rspa.nbytes) / 2, &ge_mixer_pk_envmix_in);
        if (rspa.vol[0]  > ge_mixer_em_vol0) { ge_mixer_em_vol0 = rspa.vol[0]; }
        if (rspa.vol[1]  > ge_mixer_em_vol1) { ge_mixer_em_vol1 = rspa.vol[1]; }
        if (rspa.vol_dry > ge_mixer_em_dry)  { ge_mixer_em_dry  = rspa.vol_dry; }
        if (rspa.vol_wet > ge_mixer_em_wet)  { ge_mixer_em_wet  = rspa.vol_wet; }
        if (rspa.target[0] > ge_mixer_em_tgt0) { ge_mixer_em_tgt0 = rspa.target[0]; }
        if (rspa.rate[0]   > ge_mixer_em_rate0){ ge_mixer_em_rate0 = (int)rspa.rate[0]; }
        ge_mixer_em_flags |= (int)flags;
    }
    int16_t *dry[2] = {rspa.buf.as_s16 + rspa.out / sizeof(int16_t), rspa.buf.as_s16 + rspa.dry_right / sizeof(int16_t)};
    int16_t *wet[2] = {rspa.buf.as_s16 + rspa.wet_left / sizeof(int16_t), rspa.buf.as_s16 + rspa.wet_right / sizeof(int16_t)};
    int nbytes = ROUND_UP_16(rspa.nbytes);

#if HAS_SSE41
    __m128 vols[2][2];
    __m128i dry_factor;
    __m128i wet_factor;
    __m128 target[2];
    __m128 rate[2];
    __m128i in_loaded;
    __m128i vol_s16;
    bool increasing[2];

    int c;

    if (flags & A_INIT) {
        float vol_init[2] = {rspa.vol[0], rspa.vol[1]};
        float rate_float[2] = {(float)rspa.rate[0] * (1.0f / 65536.0f), (float)rspa.rate[1] * (1.0f / 65536.0f)};
        /* GE linear law: rspa.rate is a signed 16.16 volume delta per 8 samples, so the
         * change across one 8-sample group is rate_float itself. See the block comment
         * on aEnvMixerImpl. The sm64/stock form was vol_init * (rate_float - 1). */
        float step_diff[2] = {rate_float[0], rate_float[1]};

        for (c = 0; c < 2; c++) {
            vols[c][0] = _mm_add_ps(
                _mm_set_ps1(vol_init[c]),
                _mm_mul_ps(_mm_set1_ps(step_diff[c]), _mm_setr_ps(1.0f / 8.0f, 2.0f / 8.0f, 3.0f / 8.0f, 4.0f / 8.0f)));
            vols[c][1] = _mm_add_ps(
                _mm_set_ps1(vol_init[c]),
                _mm_mul_ps(_mm_set1_ps(step_diff[c]), _mm_setr_ps(5.0f / 8.0f, 6.0f / 8.0f, 7.0f / 8.0f, 8.0f / 8.0f)));

            increasing[c] = rate_float[c] >= 0.0f;
            target[c] = _mm_set1_ps(rspa.target[c]);
            rate[c] = _mm_set1_ps(rate_float[c]);
        }

        dry_factor = _mm_set1_epi16(rspa.vol_dry);
        wet_factor = _mm_set1_epi16(rspa.vol_wet);

        memcpy(state + 32, &rate_float[0], 4);
        memcpy(state + 34, &rate_float[1], 4);
        state[36] = rspa.target[0];
        state[37] = rspa.target[1];
        state[38] = rspa.vol_dry;
        state[39] = rspa.vol_wet;
    } else {
        float floats[2];
        vols[0][0] = _mm_loadu_ps((const float *)state);
        vols[0][1] = _mm_loadu_ps((const float *)(state + 8));
        vols[1][0] = _mm_loadu_ps((const float *)(state + 16));
        vols[1][1] = _mm_loadu_ps((const float *)(state + 24));
        memcpy(floats, state + 32, 8);
        rate[0] = _mm_set1_ps(floats[0]);
        rate[1] = _mm_set1_ps(floats[1]);
        increasing[0] = floats[0] >= 0.0f;
        increasing[1] = floats[1] >= 0.0f;
        target[0] = _mm_set1_ps(state[36]);
        target[1] = _mm_set1_ps(state[37]);
        dry_factor = _mm_set1_epi16(state[38]);
        wet_factor = _mm_set1_epi16(state[39]);
    }
    do {
        in_loaded = _mm_loadu_si128((const __m128i *)in);
        in += 8;
        for (c = 0; c < 2; c++) {
            if (increasing[c]) {
                vols[c][0] = _mm_min_ps(vols[c][0], target[c]);
                vols[c][1] = _mm_min_ps(vols[c][1], target[c]);
            } else {
                vols[c][0] = _mm_max_ps(vols[c][0], target[c]);
                vols[c][1] = _mm_max_ps(vols[c][1], target[c]);
            }

            vol_s16 = _mm_packs_epi32(_mm_cvtps_epi32(vols[c][0]), _mm_cvtps_epi32(vols[c][1]));
            _mm_storeu_si128((__m128i *)dry[c],
                             _mm_adds_epi16(
                                 _mm_loadu_si128((const __m128i *)dry[c]),
                                 _mm_mulhrs_epi16(in_loaded, _mm_mulhrs_epi16(vol_s16, dry_factor))));
            dry[c] += 8;

            if (flags & A_AUX) {
                _mm_storeu_si128((__m128i *)wet[c],
                                 _mm_adds_epi16(
                                     _mm_loadu_si128((const __m128i *)wet[c]),
                                     _mm_mulhrs_epi16(in_loaded, _mm_mulhrs_epi16(vol_s16, wet_factor))));
                wet[c] += 8;
            }

            /* GE linear law: advance by one 8-sample group's delta (was _mm_mul_ps). */
            vols[c][0] = _mm_add_ps(vols[c][0], rate[c]);
            vols[c][1] = _mm_add_ps(vols[c][1], rate[c]);
        }

        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);

    _mm_storeu_ps((float *)state, vols[0][0]);
    _mm_storeu_ps((float *)(state + 8), vols[0][1]);
    _mm_storeu_ps((float *)(state + 16), vols[1][0]);
    _mm_storeu_ps((float *)(state + 24), vols[1][1]);
#elif HAS_NEON
    float32x4_t vols[2][2];
    int16_t dry_factor;
    int16_t wet_factor;
    float32x4_t target[2];
    float rate[2];
    int16x8_t in_loaded;
    int16x8_t vol_s16;
    bool increasing[2];

    int c;

    if (flags & A_INIT) {
        float vol_init[2] = {rspa.vol[0], rspa.vol[1]};
        float rate_float[2] = {(float)rspa.rate[0] * (1.0f / 65536.0f), (float)rspa.rate[1] * (1.0f / 65536.0f)};
        /* GE linear law -- see the SSE path above and the block comment on aEnvMixerImpl. */
        float step_diff[2] = {rate_float[0], rate_float[1]};
        static const float step_dividers_data[2][4] = {{1.0f / 8.0f, 2.0f / 8.0f, 3.0f / 8.0f, 4.0f / 8.0f},
                                                      {5.0f / 8.0f, 6.0f / 8.0f, 7.0f / 8.0f, 8.0f / 8.0f}};
        float32x4_t step_dividers[2] = {vld1q_f32(step_dividers_data[0]), vld1q_f32(step_dividers_data[1])};

        for (c = 0; c < 2; c++) {
            vols[c][0] = vaddq_f32(vdupq_n_f32(vol_init[c]), vmulq_n_f32(step_dividers[0], step_diff[c]));
            vols[c][1] = vaddq_f32(vdupq_n_f32(vol_init[c]), vmulq_n_f32(step_dividers[1], step_diff[c]));
            increasing[c] = rate_float[c] >= 0.0f;
            target[c] = vdupq_n_f32(rspa.target[c]);
            rate[c] = rate_float[c];
        }

        dry_factor = rspa.vol_dry;
        wet_factor = rspa.vol_wet;

        memcpy(state + 32, &rate_float[0], 4);
        memcpy(state + 34, &rate_float[1], 4);
        state[36] = rspa.target[0];
        state[37] = rspa.target[1];
        state[38] = rspa.vol_dry;
        state[39] = rspa.vol_wet;
    } else {
        vols[0][0] = vreinterpretq_f32_s16(vld1q_s16(state));
        vols[0][1] = vreinterpretq_f32_s16(vld1q_s16(state + 8));
        vols[1][0] = vreinterpretq_f32_s16(vld1q_s16(state + 16));
        vols[1][1] = vreinterpretq_f32_s16(vld1q_s16(state + 24));
        memcpy(&rate[0], state + 32, 4);
        memcpy(&rate[1], state + 34, 4);
        increasing[0] = rate[0] >= 0.0f;
        increasing[1] = rate[1] >= 0.0f;
        target[0] = vdupq_n_f32(state[36]);
        target[1] = vdupq_n_f32(state[37]);
        dry_factor = state[38];
        wet_factor = state[39];
    }

    do {
        in_loaded = vld1q_s16(in);
        in += 8;
        for (c = 0; c < 2; c++) {
            if (increasing[c]) {
                vols[c][0] = vminq_f32(vols[c][0], target[c]);
                vols[c][1] = vminq_f32(vols[c][1], target[c]);
            } else {
                vols[c][0] = vmaxq_f32(vols[c][0], target[c]);
                vols[c][1] = vmaxq_f32(vols[c][1], target[c]);
            }

            vol_s16 = vcombine_s16(vqmovn_s32(vcvtq_s32_f32(vols[c][0])), vqmovn_s32(vcvtq_s32_f32(vols[c][1])));
            vst1q_s16(dry[c], vqaddq_s16(vld1q_s16(dry[c]), vqrdmulhq_s16(in_loaded, vqrdmulhq_n_s16(vol_s16, dry_factor))));
            dry[c] += 8;
            if (flags & A_AUX) {
                vst1q_s16(wet[c], vqaddq_s16(vld1q_s16(wet[c]), vqrdmulhq_s16(in_loaded, vqrdmulhq_n_s16(vol_s16, wet_factor))));
                wet[c] += 8;
            }
            /* GE linear law: advance by one 8-sample group's delta (was vmulq_n_f32). */
            vols[c][0] = vaddq_f32(vols[c][0], vdupq_n_f32(rate[c]));
            vols[c][1] = vaddq_f32(vols[c][1], vdupq_n_f32(rate[c]));
        }

        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);

    vst1q_s16(state, vreinterpretq_s16_f32(vols[0][0]));
    vst1q_s16(state + 8, vreinterpretq_s16_f32(vols[0][1]));
    vst1q_s16(state + 16, vreinterpretq_s16_f32(vols[1][0]));
    vst1q_s16(state + 24, vreinterpretq_s16_f32(vols[1][1]));
#else
    int16_t target[2];
    int32_t rate[2];
    int16_t vol_dry, vol_wet;

    int32_t step_diff[2];
    int32_t vols[2][8];

    int c, i;

    if (flags & A_INIT) {
        target[0] = rspa.target[0];
        target[1] = rspa.target[1];
        rate[0] = rspa.rate[0];
        rate[1] = rspa.rate[1];
        vol_dry = rspa.vol_dry;
        vol_wet = rspa.vol_wet;
        /* GE linear law: per-sample delta is rate/8, in the same 16.16 units as
         * vols[][]. The stock form was vol * (rate - 1.0) / 8, and it read vol[0] for
         * both channels; moot here, since the delta does not depend on vol at all. */
        step_diff[0] = rate[0] / 8;
        step_diff[1] = rate[1] / 8;

        for (i = 0; i < 8; i++) {
            vols[0][i] = clamp32((int64_t)(rspa.vol[0] << 16) + step_diff[0] * (i + 1));
            vols[1][i] = clamp32((int64_t)(rspa.vol[1] << 16) + step_diff[1] * (i + 1));
        }
    } else {
        memcpy(vols[0], state, 32);
        memcpy(vols[1], state + 16, 32);
        target[0] = state[32];
        target[1] = state[35];
        rate[0] = (state[33] << 16) | (uint16_t)state[34];
        rate[1] = (state[36] << 16) | (uint16_t)state[37];
        vol_dry = state[38];
        vol_wet = state[39];
    }

    do {
        for (c = 0; c < 2; c++) {
            for (i = 0; i < 8; i++) {
                if (rate[c] >= 0) {
                    // Increasing volume
                    if ((vols[c][i] >> 16) > target[c]) {
                        vols[c][i] = target[c] << 16;
                    }
                } else {
                    // Decreasing volume
                    if ((vols[c][i] >> 16) < target[c]) {
                        vols[c][i] = target[c] << 16;
                    }
                }
                dry[c][i] = clamp16((dry[c][i] * 0x7fff + in[i] * (((vols[c][i] >> 16) * vol_dry + 0x4000) >> 15) + 0x4000) >> 15);
                if (flags & A_AUX) {
                    wet[c][i] = clamp16((wet[c][i] * 0x7fff + in[i] * (((vols[c][i] >> 16) * vol_wet + 0x4000) >> 15) + 0x4000) >> 15);
                }
                /* GE linear law: advance by one 8-sample group's delta (was a multiply). */
                vols[c][i] = clamp32((int64_t)vols[c][i] + rate[c]);
            }

            dry[c] += 8;
            if (flags & A_AUX) {
                wet[c] += 8;
            }
        }

        nbytes -= 16;
        in += 8;
    } while (nbytes > 0);

    memcpy(state, vols[0], 32);
    memcpy(state + 16, vols[1], 32);
    state[32] = target[0];
    state[35] = target[1];
    state[33] = (int16_t)(rate[0] >> 16);
    state[34] = (int16_t)rate[0];
    state[36] = (int16_t)(rate[1] >> 16);
    state[37] = (int16_t)rate[1];
    state[38] = vol_dry;
    state[39] = vol_wet;
#endif
    if (ge_mixer_probe) {
        ge_mixer_n_envmix++;
        ge_probe_peak(rspa.buf.as_s16 + rspa.out / sizeof(int16_t), ROUND_UP_16(rspa.nbytes) / 2, &ge_mixer_pk_envmix);
    }
}

void aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    int nbytes = ROUND_UP_32(rspa.nbytes);
    int16_t *in = rspa.buf.as_s16 + in_addr / sizeof(int16_t);
    int16_t *out = rspa.buf.as_s16 + out_addr / sizeof(int16_t);
#if HAS_SSE41
    __m128i gain_vec = _mm_set1_epi16(gain);
#elif !HAS_NEON
    int i;
    int32_t sample;
#endif

#if !HAS_NEON
    if (gain == -0x8000) {
        while (nbytes > 0) {
#if HAS_SSE41
            __m128i out1, out2, in1, in2;
            out1 = _mm_loadu_si128((const __m128i *)out);
            out2 = _mm_loadu_si128((const __m128i *)(out + 8));
            in1 = _mm_loadu_si128((const __m128i *)in);
            in2 = _mm_loadu_si128((const __m128i *)(in + 8));

            out1 = _mm_subs_epi16(out1, in1);
            out2 = _mm_subs_epi16(out2, in2);

            _mm_storeu_si128((__m128i *)out, out1);
            _mm_storeu_si128((__m128i *)(out + 8), out2);

            out += 16;
            in += 16;
#else
            for (i = 0; i < 16; i++) {
                sample = *out - *in++;
                *out++ = clamp16(sample);
            }
#endif

            nbytes -= 16 * sizeof(int16_t);
        }
    }
#endif

    while (nbytes > 0) {
#if HAS_SSE41
        __m128i out1, out2, in1, in2;
        out1 = _mm_loadu_si128((const __m128i *)out);
        out2 = _mm_loadu_si128((const __m128i *)(out + 8));
        in1 = _mm_loadu_si128((const __m128i *)in);
        in2 = _mm_loadu_si128((const __m128i *)(in + 8));

        out1 = _mm_adds_epi16(out1, _mm_mulhrs_epi16(in1, gain_vec));
        out2 = _mm_adds_epi16(out2, _mm_mulhrs_epi16(in2, gain_vec));

        _mm_storeu_si128((__m128i *)out, out1);
        _mm_storeu_si128((__m128i *)(out + 8), out2);

        out += 16;
        in += 16;
#elif HAS_NEON
        int16x8_t out1, out2, in1, in2;
        out1 = vld1q_s16(out);
        out2 = vld1q_s16(out + 8);
        in1 = vld1q_s16(in);
        in2 = vld1q_s16(in + 8);

        out1 = vqaddq_s16(out1, vqrdmulhq_n_s16(in1, gain));
        out2 = vqaddq_s16(out2, vqrdmulhq_n_s16(in2, gain));

        vst1q_s16(out, out1);
        vst1q_s16(out + 8, out2);

        out += 16;
        in += 16;
#else
        for (i = 0; i < 16; i++) {
            sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;
            *out++ = clamp16(sample);
        }
#endif

        nbytes -= 16 * sizeof(int16_t);
    }
    if (ge_mixer_probe) {
        ge_mixer_n_mix++;
        ge_probe_peak(rspa.buf.as_s16 + out_addr / sizeof(int16_t), ROUND_UP_32(rspa.nbytes) / 2, &ge_mixer_pk_mix);
    }
}

/* A_POLEF -- the IIR lowpass run over the DMEM buffer, used by the reverb
 * (src/libultrare/audio/reverb.c `_filterBuffer`) to damp each delay line. SM64
 * never emitted this command, so sm64ex's mixer has no implementation and Perfect
 * Dark's copy has an empty one marked "this never gets called?".
 *
 * The implementation is not a guessed filter: it is pinned by GoldenEye's own source,
 * `src/libultrare/audio/drvrNew.c init_lpfilter()`:
 *
 *     temp = lp->fc * SCALE;  fc = temp >> 15;      SCALE = 16384
 *     lp->fgain = SCALE - fc;
 *     fccoef[0..7] = 0;  fccoef[8] = fc;
 *     fcoef = ffc = fc/SCALE;  for (i=9..15) { fcoef *= ffc; fccoef[i] = fcoef*SCALE; }
 *
 * That is a ONE-pole lowpass whose coefficients have been pre-expanded into the
 * geometric series a^1..a^8 so the RSP can evaluate eight samples in parallel. The
 * first eight coefficients are all zero, so the second pole is disabled outright:
 * whatever aspMain's general two-pole kernel does, on GoldenEye's data it reduces to
 *
 *     y[n] = (fgain * x[n] + a * y[n-1]) >> 14        a = fccoef[8]
 *
 * The shift is forced rather than chosen. A lowpass must have unity DC gain, i.e.
 * fgain / (SCALE - a) == 1, and `init_lpfilter` sets fgain = SCALE - fc with a == fc
 * exactly. That identity holds for SCALE = 16384 = 1<<14 and for no other shift. The
 * one-shot report below prints gain + a under GETV_AUDIO_DEBUG and should always read
 * 16384.
 *
 * The sequential recursion is mathematically identical to the parallel 8-tap form;
 * only the per-step rounding differs, so this is an implementation of GE's filter,
 * not an approximation of it. No GPL source was consulted or copied.
 *
 * `state` is a POLEF_STATE (short[4]) that drvrNew.c alHeapAllocs and NOTHING in the
 * C tree ever reads -- it is opaque carry state between audio frames -- so the two
 * slots used here are free to define. flags carries `lp->first`, which is A_INIT (1) on
 * the first frame after the filter is built and 0 thereafter.
 *
 * `aLoadADPCM(ptr++, 32, fccoef)` immediately precedes every A_POLEF and clobbers the
 * ADPCM codebook. That is true on hardware too, since they share the DMEM table region,
 * which is why alAdpcmPull reloads the codebook for every voice. Do not "fix" LOADADPCM
 * by special-casing it as ADPCM-only; that breaks this filter.
 *
 * GETV_POLEF=0 restores the pass-through for A/B comparison. To confirm at runtime
 * which path is active, use GETV_AUDIO_DEBUG=1, which prints one `polef:` line;
 * checking for a symbol with `strings` does not work, because this inlines.
 */
int ge_mixer_polef = -1;            /* -1 = not yet read from the environment */
unsigned ge_mixer_n_polef = 0;      /* frames filtered; the runtime discriminator */

void aPoleFilterImpl(uint8_t flags, int16_t gain, void *state) {
    int16_t *st = (int16_t *)state;
    const int16_t *in;
    int16_t *buf;
    int32_t a, y;
    int i, n;

    if (ge_mixer_polef < 0) {
        const char *e = getenv("GETV_POLEF");
        ge_mixer_polef = (e != NULL && e[0] == '0') ? 0 : 1;
    }

    /* fccoef[8] -- aLoadADPCMImpl memcpy'd the 16 s16 linearly into adpcm_table,
     * which is [8][2][8], so linear index 8 is [0][1][0]. */
    a = rspa.adpcm_table[0][1][0];

    if (ge_mixer_n_polef == 0 && ge_mixer_probe) {
        printf("[getv] polef: gain=%d a=%d gain+a=%d (expect 16384) nbytes=%u in=%u out=%u\n",
               (int)gain, (int)a, (int)gain + (int)a,
               (unsigned)rspa.nbytes, (unsigned)rspa.in, (unsigned)rspa.out);
        fflush(stdout);
    }
    ge_mixer_n_polef++;

    if (!ge_mixer_polef) {
        return;
    }

    /* _filterBuffer issues aSetBuffer(0, buff, buff, count<<1): in == out, filtered
     * in place. Honour rspa.out anyway so the command stays correct in general. */
    buf = rspa.buf.as_s16 + rspa.out / sizeof(int16_t);
    in  = rspa.buf.as_s16 + rspa.in  / sizeof(int16_t);
    n = (int)(rspa.nbytes / sizeof(int16_t));

    if (flags & A_INIT) {
        st[0] = st[1] = st[2] = st[3] = 0;
    }
    y = st[3];

    for (i = 0; i < n; i++) {
        int32_t x = (int32_t)in[i];
        y = clamp16((int32_t)(((int64_t)gain * x + (int64_t)a * y) >> 14));
        buf[i] = (int16_t)y;
    }

    st[2] = (n > 1) ? buf[n - 2] : st[3];
    st[3] = (int16_t)y;

    if (ge_mixer_probe) {
        ge_probe_peak(buf, n, &ge_mixer_pk_polef);
    }
}

/* ---------------------------------------------------------- env.c guards --
 * See ge_mixer.h for why these exist: Rare's build kept asserts ON in env.c and
 * only env.c, this build compiles them out, and restoring them literally would
 * trade a silent corruption for a silent hang (assert -> abort -> SIGABRT).
 *
 * Counters are visible so a caller can report them; the print is rate-limited to
 * the first 8 occurrences and then powers of two, so a per-audio-frame fault
 * cannot flood the log the way an unbounded printf in this path would.
 */
unsigned long ge_env_guard_lo   = 0;   /* samples < 0                  */
unsigned long ge_env_guard_hi   = 0;   /* samples > AL_MAX_RSP_SAMPLES */
unsigned long ge_env_guard_null = 0;   /* NULL upstream filter         */
int ge_env_clamp = -1;                 /* -1 = not yet read from env   */

static int ge_guard_should_report(unsigned long n) {
    if (n <= 8) {
        return 1;
    }
    return (n & (n - 1)) == 0;   /* powers of two thereafter */
}

int geEnvSamplesGuard(int samples, int maxsamples) {
    if (ge_env_clamp < 0) {
        const char *e = getenv("GETV_ENVCLAMP");
        ge_env_clamp = (e != NULL && e[0] == '0') ? 0 : 1;
    }

    if (samples >= 0 && samples <= maxsamples) {
        return samples;
    }

    if (samples < 0) {
        ge_env_guard_lo++;
        if (ge_guard_should_report(ge_env_guard_lo)) {
            printf("[getv] envguard: samples=%d < 0 (occurrence %lu)%s\n",
                   samples, ge_env_guard_lo, ge_env_clamp ? " -> clamped to 0" : " -> PASSED THROUGH");
            fflush(stdout);
        }
        return ge_env_clamp ? 0 : samples;
    }

    ge_env_guard_hi++;
    if (ge_guard_should_report(ge_env_guard_hi)) {
        printf("[getv] envguard: samples=%d > AL_MAX_RSP_SAMPLES(%d) (occurrence %lu)%s\n",
               samples, maxsamples, ge_env_guard_hi,
               ge_env_clamp ? " -> clamped" : " -> PASSED THROUGH");
        fflush(stdout);
    }
    return ge_env_clamp ? maxsamples : samples;
}

int geEnvSourceGuard(const void *source) {
    if (source != NULL) {
        return 1;
    }
    ge_env_guard_null++;
    if (ge_guard_should_report(ge_env_guard_null)) {
        printf("[getv] envguard: NULL upstream filter in _pullSubFrame "
               "(occurrence %lu) -> subframe skipped\n", ge_env_guard_null);
        fflush(stdout);
    }
    return 0;
}
