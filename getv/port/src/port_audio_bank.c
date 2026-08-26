/* GoldenEye tvOS port - ALBankFile: 32-bit big-endian ROM data -> native structs.
 *
 * This is alBnkfNew()'s job, and alBnkfNew() cannot do it here.
 *
 * A `.ctl` bank file is a tree of ALBankFile / ALBank / ALInstrument / ALSound /
 * ALWaveTable / ALEnvelope / ALKeyMap / ALADPCMloop / ALADPCMBook records in which
 * every pointer field is stored as a 32-bit BIG-ENDIAN offset from the start of the
 * file. On the N64 the structs overlay the file bytes exactly, so alBnkfNew() just
 * walks the tree adding the load address to each slot in PLACE.
 *
 * Neither half of that survives at 64-bit little-endian:
 *   - the slots are 4 bytes and a native pointer is 8, so an in-place patch writes
 * over the following field. This is the same mistake PROMOTE() makes on model
 * blobs, and the rule from that work applies here too: converting an asset means
 * allocating new storage, never widening in place.
 *   - every scalar is byte-swapped.
 *
 * So this walks the file view and BUILDS a native tree beside it. Structure and
 * naming follow Perfect Dark's port/src/preprocess/segaudio.c, which solves exactly
 * this problem for the same file format; PD does it offline in its asset
 * preprocessor, we do it at load, which is the same choice already made for models
 * and animations.
 *
 * Shared records must stay shared. 261 sounds in sfx.ctl reference only 186
 * wavetables, and Rare's banks reuse envelopes and keymaps heavily. Converting a
 * record twice would be wasteful but otherwise harmless were it not that the AL
 * library uses record identity (`v->table == w`) to decide whether a voice is already
 * playing the right sample. Hence the offset->pointer map.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <PR/ultratypes.h>
#include <PR/libaudio.h>

/* ------------------------------------------------------------ file readers -- */

static u16 geBeU16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static s16 geBeS16(const u8 *p) { return (s16)geBeU16(p); }
static u32 geBeU32(const u8 *p)
{
 return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* Offsets of the file-view fields. Written out rather than derived from a mirror
 * struct because the file layout is fixed by the ROM and must not track whatever the
 * native structs happen to be. These match PD's n64_* structs in segaudio.c. */
#define F_BANKFILE_BANKARRAY    4       /* after s16 revision, s16 bankCount */

#define F_BANK_INSTCOUNT        0
#define F_BANK_FLAGS            2
#define F_BANK_PAD              3
#define F_BANK_SAMPLERATE       4
#define F_BANK_PERCUSSION       8
#define F_BANK_INSTARRAY        12

#define F_INST_BENDRANGE        12
#define F_INST_SOUNDCOUNT       14
#define F_INST_SOUNDARRAY       16

#define F_SOUND_ENVELOPE        0
#define F_SOUND_KEYMAP          4
#define F_SOUND_WAVETABLE       8
#define F_SOUND_SAMPLEPAN       12
#define F_SOUND_SAMPLEVOLUME    13
#define F_SOUND_FLAGS           14

#define F_WAVE_BASE             0
#define F_WAVE_LEN              4
#define F_WAVE_TYPE             8
#define F_WAVE_FLAGS            9
#define F_WAVE_ADPCM_LOOP       12
#define F_WAVE_ADPCM_BOOK       16
#define F_WAVE_RAW_LOOP         12

/* ---------------------------------------------------------- offset safety net --
 * Every F_* constant above is hand-derived from include/PR/libaudio.h under a 32-bit
 * ABI. Model/asset structs get _Static_asserts from tools/gen_asset_fileview.py, but
 * that tool reads only src/bondtypes.h, so audio is outside its scope. A wrong audio
 * offset introduced by a future edit would be completely silent: the loader would read
 * plausible garbage and the bank would come up subtly wrong rather than failing.
 *
 * These mirrors reproduce the on-disk 32-bit layout (pointers as u32), so offsetof on
 * them is exactly the file offset. They are declaration-only -- no instance is ever
 * created and no code below uses them; they exist so the compiler checks the
 * constants. Do not "simplify" them to use real pointer types: on arm64 that makes
 * them 64-bit and the asserts would then certify the wrong layout.
 *
 * Alignment note, because it is the one non-obvious row: ALWaveTable's union of
 * pointer-bearing structs has 4-byte alignment, and the header ends at offset 10
 * (base 4 + len 4 + type 1 + flags 1), so the union is padded to 12. That is why
 * F_WAVE_ADPCM_LOOP is 12 and not 10. */
typedef struct { u32 base; s32 len; u8 type; u8 flags;
 union { struct { u32 loop; u32 book; } adpcmWave;
 struct { u32 loop; } rawWave; } waveInfo; } GeFileWaveTable;
typedef struct { u32 envelope; u32 keyMap; u32 wavetable;
 u8 samplePan; u8 sampleVolume; u8 flags; } GeFileSound;
typedef struct { u8 volume, pan, priority, flags, tremType, tremRate, tremDepth,
 tremDelay, vibType, vibRate, vibDepth, vibDelay;
 s16 bendRange; s16 soundCount; u32 soundArray[1]; } GeFileInstrument;
typedef struct { s16 instCount; u8 flags; u8 pad; s32 sampleRate;
 u32 percussion; u32 instArray[1]; } GeFileBank;
typedef struct { s16 revision; s16 bankCount; u32 bankArray[1]; } GeFileBankFile;
typedef struct { s32 attackTime, decayTime, releaseTime;
 u8 attackVolume, decayVolume; } GeFileEnvelope;
typedef struct { u8 velocityMin, velocityMax, keyMin, keyMax, keyBase;
 s8 detune; } GeFileKeyMap;
typedef struct { u32 start, end, count; s16 state[16]; } GeFileADPCMloop;
typedef struct { s32 order; s32 npredictors; s16 book[1]; } GeFileADPCMBook;

_Static_assert(offsetof(GeFileBankFile, bankArray)  == F_BANKFILE_BANKARRAY, "F_BANKFILE_BANKARRAY");
_Static_assert(offsetof(GeFileBank, instCount)      == F_BANK_INSTCOUNT, "F_BANK_INSTCOUNT");
_Static_assert(offsetof(GeFileBank, flags)          == F_BANK_FLAGS, "F_BANK_FLAGS");
_Static_assert(offsetof(GeFileBank, pad)            == F_BANK_PAD, "F_BANK_PAD");
_Static_assert(offsetof(GeFileBank, sampleRate)     == F_BANK_SAMPLERATE, "F_BANK_SAMPLERATE");
_Static_assert(offsetof(GeFileBank, percussion)     == F_BANK_PERCUSSION, "F_BANK_PERCUSSION");
_Static_assert(offsetof(GeFileBank, instArray)      == F_BANK_INSTARRAY, "F_BANK_INSTARRAY");
_Static_assert(offsetof(GeFileInstrument, bendRange)  == F_INST_BENDRANGE, "F_INST_BENDRANGE");
_Static_assert(offsetof(GeFileInstrument, soundCount) == F_INST_SOUNDCOUNT, "F_INST_SOUNDCOUNT");
_Static_assert(offsetof(GeFileInstrument, soundArray) == F_INST_SOUNDARRAY, "F_INST_SOUNDARRAY");
_Static_assert(offsetof(GeFileSound, envelope)     == F_SOUND_ENVELOPE, "F_SOUND_ENVELOPE");
_Static_assert(offsetof(GeFileSound, keyMap)       == F_SOUND_KEYMAP, "F_SOUND_KEYMAP");
_Static_assert(offsetof(GeFileSound, wavetable)    == F_SOUND_WAVETABLE, "F_SOUND_WAVETABLE");
_Static_assert(offsetof(GeFileSound, samplePan)    == F_SOUND_SAMPLEPAN, "F_SOUND_SAMPLEPAN");
_Static_assert(offsetof(GeFileSound, sampleVolume) == F_SOUND_SAMPLEVOLUME, "F_SOUND_SAMPLEVOLUME");
_Static_assert(offsetof(GeFileSound, flags)        == F_SOUND_FLAGS, "F_SOUND_FLAGS");
_Static_assert(offsetof(GeFileWaveTable, base)  == F_WAVE_BASE, "F_WAVE_BASE");
_Static_assert(offsetof(GeFileWaveTable, len)   == F_WAVE_LEN, "F_WAVE_LEN");
_Static_assert(offsetof(GeFileWaveTable, type)  == F_WAVE_TYPE, "F_WAVE_TYPE");
_Static_assert(offsetof(GeFileWaveTable, flags) == F_WAVE_FLAGS, "F_WAVE_FLAGS");
_Static_assert(offsetof(GeFileWaveTable, waveInfo.adpcmWave.loop) == F_WAVE_ADPCM_LOOP, "F_WAVE_ADPCM_LOOP");
_Static_assert(offsetof(GeFileWaveTable, waveInfo.adpcmWave.book) == F_WAVE_ADPCM_BOOK, "F_WAVE_ADPCM_BOOK");
_Static_assert(offsetof(GeFileWaveTable, waveInfo.rawWave.loop)   == F_WAVE_RAW_LOOP, "F_WAVE_RAW_LOOP");

/* The literals the code passes to geValid(). The invariant is not "literal == * sizeof": sizeof(GeFileEnvelope) is 16, not 14, because three s32 fields give the
 * struct 4-byte alignment and its 14 bytes of fields round up to 16. That trailing pad
 * is real on disk but the converter never reads it. What matters is that the bounds
 * check covers every field the converter does read, i.e. literal >= last field offset
 * + 1. Asserted that way below. */
_Static_assert(offsetof(GeFileEnvelope, attackVolume) == 12, "ALEnvelope attackVolume");
_Static_assert(offsetof(GeFileEnvelope, decayVolume)  == 13, "ALEnvelope decayVolume");
_Static_assert(offsetof(GeFileEnvelope, decayVolume) + 1 <= 14, "geValid(c, env, 14) must not under-check");
_Static_assert(offsetof(GeFileKeyMap, detune) == 5, "ALKeyMap detune");
_Static_assert(sizeof(GeFileKeyMap) == 6, "ALKeyMap is six u8/s8 -- and therefore needs NO byte swapping");
_Static_assert(offsetof(GeFileKeyMap, detune) + 1 <= 8, "geValid(c, key, 8) must not under-check");
_Static_assert(offsetof(GeFileADPCMloop, state) == 12, "ALADPCMloop: u32 x3 then s16[16] -- mixed widths in ONE struct");
_Static_assert(sizeof(GeFileADPCMloop) == 44, "ALADPCMloop file size");
_Static_assert(offsetof(GeFileADPCMBook, book)  == 8, "geValid(c, book, 8) header, then order*npredictors*8 s16");

/* ------------------------------------------------------- conversion context -- */

#define GE_MAP_MAX 2048

struct geBankCtx {
 const u8   *src;            /* the .ctl file bytes, big-endian              */
 u32 srclen;
 u8         *dst;            /* the native arena being filled                */
 u32 dstpos;
 u32 dstlen;
 u8         *tbl;            /* the .tbl sample bank, for ALWaveTable.base   */
 u32 tbllen;
    /* offset -> already-converted pointer, so shared records convert once */
 u32 mapsrc[GE_MAP_MAX];
 void       *mapdst[GE_MAP_MAX];
 s32 mapcount;
 s32 overflow;
};

static void *geMapFind(struct geBankCtx *c, u32 srcoff)
{
 s32 i;
 for (i = 0; i < c->mapcount; i++) {
 if (c->mapsrc[i] == srcoff) {
 return c->mapdst[i];
        }
    }
 return NULL;
}

static void geMapAdd(struct geBankCtx *c, u32 srcoff, void *p)
{
 if (c->mapcount < GE_MAP_MAX) {
 c->mapsrc[c->mapcount] = srcoff;
 c->mapdst[c->mapcount] = p;
 c->mapcount++;
    } else {
 c->overflow++;
    }
}

/* Carve `size` bytes out of the arena. Returns NULL once the arena is exhausted;
 * every caller checks, so a too-small arena degrades to "some instruments missing"* and a loud message rather than a heap smash. */
static void *geAlloc(struct geBankCtx *c, u32 size)
{
 void *p;
 size = (size + 15) & ~15u;
 if (c->dstpos + size > c->dstlen) {
 c->overflow++;
 return NULL;
    }
 p = c->dst + c->dstpos;
 c->dstpos += size;
 return p;
}

/* A file offset is valid if it is non-zero (0 means NULL -- the N64 patcher skipped
 * falsy slots) and inside the file. The bound check is what turns a corrupt or
 * mis-parsed bank into a missing sound instead of a wild read. */
static int geValid(struct geBankCtx *c, u32 off, u32 need)
{
 return off != 0 && off + need <= c->srclen;
}

/* -------------------------------------------------------------- converters -- */

static ALWaveTable *geConvWave(struct geBankCtx *c, u32 off)
{
 ALWaveTable *w;
 u32 base, loop, book;

 if (!geValid(c, off, 20)) {
 return NULL;
    }
 w = (ALWaveTable *)geMapFind(c, off);
 if (w != NULL) {
 return w;
    }
 w = (ALWaveTable *)geAlloc(c, sizeof(ALWaveTable));
 if (w == NULL) {
 return NULL;
    }
 geMapAdd(c, off, w);

 base    = geBeU32(c->src + off + F_WAVE_BASE);
 w->len  = (s32)geBeU32(c->src + off + F_WAVE_LEN);
 w->type = c->src[off + F_WAVE_TYPE];
 w->flags = c->src[off + F_WAVE_FLAGS];

    /* base is an offset into the .tbl sample bank. On the N64 alBnkfNew added the
     * tbl's ROM address here and the DMA callback fetched from it; the port's DMA
     * callback is the identity, so this has to become a real RAM pointer. */
 if (base + (u32)w->len > c->tbllen) {
 printf("[getv] audio: wavetable at +%u runs past the sample bank ""(%u+%d > %u)\n", off, base, w->len, c->tbllen);
 w->base = NULL;
 return w;
    }
 w->base = c->tbl + base;

 if (w->type == AL_ADPCM_WAVE) {
 loop = geBeU32(c->src + off + F_WAVE_ADPCM_LOOP);
 book = geBeU32(c->src + off + F_WAVE_ADPCM_BOOK);

 w->waveInfo.adpcmWave.loop = NULL;
 w->waveInfo.adpcmWave.book = NULL;

 if (geValid(c, loop, 12 + ADPCMFSIZE * 2)) {
 ALADPCMloop *l = (ALADPCMloop *)geMapFind(c, loop);
 if (l == NULL) {
 l = (ALADPCMloop *)geAlloc(c, sizeof(ALADPCMloop));
 if (l != NULL) {
 s32 i;
 geMapAdd(c, loop, l);
 l->start = geBeU32(c->src + loop + 0);
 l->end   = geBeU32(c->src + loop + 4);
 l->count = geBeU32(c->src + loop + 8);
 for (i = 0; i < ADPCMFSIZE; i++) {
 l->state[i] = geBeS16(c->src + loop + 12 + i * 2);
                    }
                }
            }
 w->waveInfo.adpcmWave.loop = l;
        }

 if (geValid(c, book, 8)) {
 ALADPCMBook *b = (ALADPCMBook *)geMapFind(c, book);
 if (b == NULL) {
 s32 order = (s32)geBeU32(c->src + book + 0);
 s32 npred = (s32)geBeU32(c->src + book + 4);
 s32 n = order * npred * 8;
                /* ALADPCMBook declares book[1] and is really variable length: the
                 * predictors follow inline. aLoadADPCMImpl memcpy's these straight
                 * into the mixer's coefficient table, so they must be native-endian
                 * by the time it runs -- this loop is the only place that happens. */
 if (order > 0 && npred > 0 && n <= 8 * 2 * 8 &&
 geValid(c, book, (u32)(8 + n * 2))) {
 b = (ALADPCMBook *)geAlloc(c, (u32)(sizeof(ALADPCMBook) + (n - 1) * 2));
 if (b != NULL) {
 s32 i;
 geMapAdd(c, book, b);
 b->order = order;
 b->npredictors = npred;
 for (i = 0; i < n; i++) {
 b->book[i] = geBeS16(c->src + book + 8 + i * 2);
                        }
                    }
                } else {
 printf("[getv] audio: implausible ADPCM book at +%u ""(order=%d npredictors=%d)\n", book, order, npred);
                }
            }
 w->waveInfo.adpcmWave.book = b;
        }
    } else if (w->type == AL_RAW16_WAVE) {
 loop = geBeU32(c->src + off + F_WAVE_RAW_LOOP);
 w->waveInfo.rawWave.loop = NULL;
 if (geValid(c, loop, 12)) {
 ALRawLoop *l = (ALRawLoop *)geMapFind(c, loop);
 if (l == NULL) {
 l = (ALRawLoop *)geAlloc(c, sizeof(ALRawLoop));
 if (l != NULL) {
 geMapAdd(c, loop, l);
 l->start = geBeU32(c->src + loop + 0);
 l->end   = geBeU32(c->src + loop + 4);
 l->count = geBeU32(c->src + loop + 8);
                }
            }
 w->waveInfo.rawWave.loop = l;
        }

        /* RAW16 sample data is big-endian s16 in ROM, and the mixer treats DMEM as
         * native s16. ADPCM is nibble-packed and therefore endian-neutral, which is
         * why only this branch needs the swap. instruments.ctl has exactly one such
         * wave and sfx.ctl has none. Swapped in place in the sample bank, once,
         * guarded by the wavetable map so a shared wave is not swapped twice back to
         * big-endian. */
 if (w->base != NULL) {
 s32 i, n = w->len / 2;
 u8 *p = w->base;
 for (i = 0; i < n; i++) {
 u8 t = p[i * 2];
 p[i * 2] = p[i * 2 + 1];
 p[i * 2 + 1] = t;
            }
 printf("[getv] audio: byte-swapped a RAW16 wave, %d samples\n", n);
        }
    }

 return w;
}

static ALSound *geConvSound(struct geBankCtx *c, u32 off)
{
 ALSound *s;
 u32 env, key, wav;

 if (!geValid(c, off, 15)) {
 return NULL;
    }
 s = (ALSound *)geMapFind(c, off);
 if (s != NULL) {
 return s;
    }
 s = (ALSound *)geAlloc(c, sizeof(ALSound));
 if (s == NULL) {
 return NULL;
    }
 geMapAdd(c, off, s);

 env = geBeU32(c->src + off + F_SOUND_ENVELOPE);
 key = geBeU32(c->src + off + F_SOUND_KEYMAP);
 wav = geBeU32(c->src + off + F_SOUND_WAVETABLE);

 s->envelope = NULL;
 if (geValid(c, env, 14)) {
 ALEnvelope *e = (ALEnvelope *)geMapFind(c, env);
 if (e == NULL) {
 e = (ALEnvelope *)geAlloc(c, sizeof(ALEnvelope));
 if (e != NULL) {
 geMapAdd(c, env, e);
 e->attackTime   = (s32)geBeU32(c->src + env + 0);
 e->decayTime    = (s32)geBeU32(c->src + env + 4);
 e->releaseTime  = (s32)geBeU32(c->src + env + 8);
 e->attackVolume = c->src[env + 12];
 e->decayVolume  = c->src[env + 13];
            }
        }
 s->envelope = e;
    }

 s->keyMap = NULL;
 if (geValid(c, key, 8)) {
 ALKeyMap *k = (ALKeyMap *)geMapFind(c, key);
 if (k == NULL) {
 k = (ALKeyMap *)geAlloc(c, sizeof(ALKeyMap));
 if (k != NULL) {
 geMapAdd(c, key, k);
 k->velocityMin = c->src[key + 0];
 k->velocityMax = c->src[key + 1];
 k->keyMin      = c->src[key + 2];
 k->keyMax      = c->src[key + 3];
 k->keyBase     = c->src[key + 4];
 k->detune      = (s8)c->src[key + 5];
            }
        }
 s->keyMap = k;
    }

 s->wavetable    = geConvWave(c, wav);
 s->samplePan    = c->src[off + F_SOUND_SAMPLEPAN];
 s->sampleVolume = c->src[off + F_SOUND_SAMPLEVOLUME];
    /* Not copied from the file: `flags` is alBnkfNew's own "already patched" marker,
     * and this converter uses the offset map for that instead. Leave it clear. */
 s->flags = 0;

 return s;
}

static ALInstrument *geConvInst(struct geBankCtx *c, u32 off)
{
 ALInstrument *inst;
 s16 soundCount;
 s32 i;

 if (!geValid(c, off, 16)) {
 return NULL;
    }
 inst = (ALInstrument *)geMapFind(c, off);
 if (inst != NULL) {
 return inst;
    }

 soundCount = geBeS16(c->src + off + F_INST_SOUNDCOUNT);
 if (soundCount < 0 || !geValid(c, off, (u32)(F_INST_SOUNDARRAY + soundCount * 4))) {
 printf("[getv] audio: instrument at +%u has an implausible soundCount %d\n",
 off, soundCount);
 return NULL;
    }

    /* soundArray[1] is declared, soundCount are stored. */
 inst = (ALInstrument *)geAlloc(c, (u32)(sizeof(ALInstrument) +
                                            (soundCount > 0 ? soundCount - 1 : 0) * sizeof(ALSound *)));
 if (inst == NULL) {
 return NULL;
    }
 geMapAdd(c, off, inst);

 inst->volume    = c->src[off + 0];
 inst->pan       = c->src[off + 1];
 inst->priority  = c->src[off + 2];
 inst->flags     = 0;                    /* patch marker, see above */
 inst->tremType  = c->src[off + 4];
 inst->tremRate  = c->src[off + 5];
 inst->tremDepth = c->src[off + 6];
 inst->tremDelay = c->src[off + 7];
 inst->vibType   = c->src[off + 8];
 inst->vibRate   = c->src[off + 9];
 inst->vibDepth  = c->src[off + 10];
 inst->vibDelay  = c->src[off + 11];
 inst->bendRange = geBeS16(c->src + off + F_INST_BENDRANGE);
 inst->soundCount = soundCount;

 for (i = 0; i < soundCount; i++) {
 inst->soundArray[i] = geConvSound(c, geBeU32(c->src + off + F_INST_SOUNDARRAY + i * 4));
    }

 return inst;
}

static ALBank *geConvBank(struct geBankCtx *c, u32 off)
{
 ALBank *bank;
 s16 instCount;
 s32 i;

 if (!geValid(c, off, F_BANK_INSTARRAY)) {
 return NULL;
    }
 bank = (ALBank *)geMapFind(c, off);
 if (bank != NULL) {
 return bank;
    }

 instCount = geBeS16(c->src + off + F_BANK_INSTCOUNT);
 if (instCount < 0 || !geValid(c, off, (u32)(F_BANK_INSTARRAY + instCount * 4))) {
 printf("[getv] audio: bank at +%u has an implausible instCount %d\n",
 off, instCount);
 return NULL;
    }

 bank = (ALBank *)geAlloc(c, (u32)(sizeof(ALBank) +
                                      (instCount > 0 ? instCount - 1 : 0) * sizeof(ALInstrument *)));
 if (bank == NULL) {
 return NULL;
    }
 geMapAdd(c, off, bank);

 bank->instCount  = instCount;
 bank->flags      = 0;                   /* patch marker, see above */
 bank->pad        = c->src[off + F_BANK_PAD];
 bank->sampleRate = (s32)geBeU32(c->src + off + F_BANK_SAMPLERATE);
 bank->percussion = geConvInst(c, geBeU32(c->src + off + F_BANK_PERCUSSION));

 for (i = 0; i < instCount; i++) {
 bank->instArray[i] = geConvInst(c, geBeU32(c->src + off + F_BANK_INSTARRAY + i * 4));
    }

 return bank;
}

/* ------------------------------------------------------------------- entry -- */

/* The port's replacement for `romCopy(bank, ctl, size); alBnkfNew(bank, tbl);`.
 *
 * `ctl` / `tbl` point into the linked-in geAudioSegment; `ctllen` / `tbllen` are the
 * sizes music.c derived from the gaps between consecutive segment starts.
 *
 * Returns a native ALBankFile, or NULL. The storage is malloc'd rather than taken
 * from g_musicHeap: the native tree is roughly 3x the file (8-byte
 * pointers plus the alignment of each record), and MUSIC_ALLOCATION_BYTES is a fixed
 * 0x2E000 that also has to cover the synth's voices, the FX delay lines and the
 * three sequence buffers. Growing it to fit two converted banks would be a bigger
 * change to the game's own memory map than this is worth. The banks are allocated
 * once at musicSeqPlayerInit() and live for the whole run.
 */
ALBankFile *gePortAudioBankNew(const void *ctl, u32 ctllen, void *tbl, u32 tbllen)
{
 struct geBankCtx c;
 ALBankFile *out;
 s16 revision, bankCount;
 s32 i;
 u32 arena;

 if (ctl == NULL || ctllen < 8) {
 printf("[getv] audio: bank file missing or too small (%u bytes)\n", ctllen);
 return NULL;
    }

 revision  = geBeS16((const u8 *)ctl + 0);
 bankCount = geBeS16((const u8 *)ctl + 2);

    /* alBnkfNew checks this too (ALFailIf ERR_ALBNKFNEW). Worth keeping loudly:
     * getting it wrong is the signature of reading the file at the wrong offset or
     * with the wrong endianness, and everything downstream would be garbage. */
 if (revision != AL_BANK_VERSION) {
 printf("[getv] audio: bank revision 0x%04x, expected 0x%04x (AL_BANK_VERSION) ""-- wrong offset or wrong byte order\n",
               (unsigned)(u16)revision, AL_BANK_VERSION);
 return NULL;
    }
 if (bankCount <= 0 || (u32)(4 + bankCount * 4) > ctllen) {
 printf("[getv] audio: implausible bankCount %d\n", bankCount);
 return NULL;
    }

 memset(&c, 0, sizeof(c));
 c.src    = (const u8 *)ctl;
 c.srclen = ctllen;
 c.tbl    = (u8 *)tbl;
 c.tbllen = tbllen;

    /* 4x the file, which PD's converter overshoots at 3x. The records grow by at
     * most one pointer width per pointer field plus 16-byte alignment slack, and the
     * arena is checked on every allocation regardless. */
 arena = ctllen * 4 + 4096;
 c.dst = (u8 *)calloc(1, arena);
 if (c.dst == NULL) {
 printf("[getv] audio: could not allocate %u bytes to convert a bank\n", arena);
 return NULL;
    }
 c.dstlen = arena;

 out = (ALBankFile *)geAlloc(&c, (u32)(sizeof(ALBankFile) +
                                          (bankCount > 0 ? bankCount - 1 : 0) * sizeof(ALBank *)));
 if (out == NULL) {
 free(c.dst);
 return NULL;
    }
 out->revision  = revision;
 out->bankCount = bankCount;

 for (i = 0; i < bankCount; i++) {
 out->bankArray[i] = geConvBank(&c, geBeU32(c.src + F_BANKFILE_BANKARRAY + i * 4));
    }

 printf("[getv] audio: bank converted -- %d bank(s), %d records, %u/%u arena bytes\n",
 bankCount, c.mapcount, c.dstpos, c.dstlen);

 if (c.overflow) {
        /* Never silent. An exhausted arena or map means some instruments are simply
         * absent, which presents as "most sounds work" and is otherwise very hard to
         * tell from a synth bug. */
 printf("[getv] audio: %d records dropped -- arena or offset map too small ""(map %d/%d)\n", c.overflow, c.mapcount, GE_MAP_MAX);
    }

 return out;
}
