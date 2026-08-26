/* GoldenEye tvOS port - the PRNG, ported from MIPS assembly.
 *
 * Some functions in the decomp are still raw MIPS assembly and have no C form at all.
 * `src/random.s` is one (see also boot.s, tlb_*.s, gspboot.s). The build compiles only
 * .c, so these symbols do not exist natively.
 *
 * This is a faithful transcription, not a substitute PRNG. GoldenEye seeds guard
 * behaviour, weapon spread, and object placement from this sequence -- swapping in
 * a different generator would silently change how the game plays.
 *
 * The original (src/random.s, randomGetNext):
 *  ld a0, g_randomSeed ; 64-bit seed
 *  dsll32 a2, a0, 0x1f ; a2 = s << 63 (dsll32 shifts by sa+32)
 *  dsll a1, a0, 0x1f ; a1 = s << 31
 *  dsrl a2, a2, 0x1f ; a2 = (s << 63) >> 31
 *  dsrl32 a1, a1, 0 ; a1 = (s << 31) >> 32
 *  dsll32 a0, a0, 0xc ; a0 = s << 44
 *  or a2, a2, a1
 *  dsrl32 a0, a0, 0 ; a0 = (s << 44) >> 32
 *  xor a2, a2, a0
 *  dsrl a0, a2, 0x14 ; a0 = a2 >> 20
 *  andi a0, a0, 0xfff
 *  xor a0, a0, a2
 *  dsll32 v0, a0, 0 ; v0 = new << 32
 *  sd a0, g_randomSeed
 *  dsra32 v0, v0, 0 ; return sign-extended low 32 bits
 *
 * Every shift is 64-bit (dsll/dsrl), so the state must be u64. Doing this in 32 bits
 * would produce a completely different sequence.
 */
#include <PR/ultratypes.h>

/* src/random.s: .word 0xAB8D9F77, .word 0x81280783 -- big-endian, so one 64-bit
 * value. */
u64 g_randomSeed = 0xAB8D9F7781280783ULL;

static u64 ge_random_step(u64 s)
{
    u64 mixed = ((s << 63) >> 31) | ((s << 31) >> 32);
    mixed ^= (s << 44) >> 32;
    return ((mixed >> 20) & 0xfff) ^ mixed;
}

u32 randomGetNext(void)
{
    g_randomSeed = ge_random_step(g_randomSeed);
    /* dsll32 then dsra32 is a sign-extension of the low 32 bits; as a u32 return
     * that is simply the low word. */
    return (u32)g_randomSeed;
}

u32 randomGetNextFrom(u64 *seed)
{
    *seed = ge_random_step(*seed);
    return (u32)*seed;
}

/* randomSetSeed: daddiu a0, a0, 1 ; sd a0, g_randomSeed -- the seed stored is the
 * argument plus one. Defined below, with the GETV_SEED diagnostic wrapper. */

/* ---------------------------------------------------------------------------
 * Port diagnostic: make the RNG seed observable and optionally deterministic.
 *
 * boss.c:402 does `randomSetSeed(osGetCount())`. On the N64 osGetCount() is a real
 * hardware counter, so the seed genuinely varied per boot. In this port osGetCount()
 * (getv/port/src/port_os.c:164) is
 *  static u32 count; return count += 1000;
 * a plain non-atomic static that is also called from the audio thread
 * (src/libultra/audio/synthesizer.c:170,186). So the seed at boss.c:402 is 1000 times
 * the number of osGetCount calls that happened first, which depends on how many audio
 * frames the audio thread got through before bossMainloop reached that line. That is a
 * thread race, and a candidate cause of launch-to-launch differences in output.
 *
 * The RNG's first consumer at level boot is the intro cinema camera:
 * bondview_r.c:384-390 picks `randomGetNext() % g_SetupIntroCameraCount`, and the whole
 * first ~300 frames of every solo level are rendered from that camera. A different
 * camera is a different view, therefore a different triangle count.
 *
 *  GETV_SEED=<n> force randomSetSeed to use n (decimal or 0x hex) instead of
 *  whatever boss.c passes. Makes a run reproducible.
 *  GETV_SEEDTRACE=1 print every randomSetSeed call and the resulting state.
 * ------------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>

void randomSetSeed(u32 seed)
{
    static int mode = -1;      /* -1 unread, 0 none, 1 override */
    static unsigned long forced;
    static int trace;

    if (mode < 0) {
        const char *e = getenv("GETV_SEED");
        const char *t = getenv("GETV_SEEDTRACE");
        trace = (t && *t == '1');
        if (e && *e) { forced = strtoul(e, NULL, 0); mode = 1; }
        else         { mode = 0; }
    }

    if (mode == 1) {
        if (trace)
            printf("[getv][rng] randomSetSeed(%u) OVERRIDDEN -> %lu\n",
                   (unsigned)seed, forced);
        seed = (u32)forced;
    } else if (trace) {
        printf("[getv][rng] randomSetSeed(%u)\n", (unsigned)seed);
    }
    if (trace) fflush(stdout);

    g_randomSeed = (u64)seed + 1;
}
