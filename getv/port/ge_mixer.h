/* GoldenEye tvOS redirect libultra's audio command macros at a software RSP.
 *
 * On the n64 each a* macro packs one 8-byte Acmd into a list that the RSP later
 * executes running the `aspMain` microcode. There is no RSP here, so each macro
 * becomes a direct call into ge_mixer.c, which performs the operation immediately.
 * The `pkt` argument (the Acmd write cursor) is ignored; the AL library still
 * increments it, which is harmless (it just ends up counting commands).
 *
 * This header is pulled in from the BOTTOM of <PR/abi.h>, and only when the TU
 * defines GE_AUDIO_MIXER, so it must #undef the macros abi.h just defined. Only the
 * libultra/libultrare audio TUs are compiled with that define; every other TU sees
 * the stock command-building macros, exactly as before.
 *
 * Addresses arrive here as real 64-bit pointers. That works only because the port
 * already made K0_TO_PHYS / osVirtualToPhysical the identity (PR/R4300.h, PR/os.h) and
 * because ALSave.dramout / ALLoadFilter.memin / ALDMAproc were widened to intptr_t.
 * Anything that still routes an address through an s32 will arrive truncated and fault
 * here rather than at the site of the bug.
 */
#ifndef GE_MIXER_H
#define GE_MIXER_H

#include <stdint.h>

#undef aSegment
#undef aClearBuffer
#undef aSetBuffer
#undef aLoadBuffer
#undef aSaveBuffer
#undef aDMEMMove
#undef aMix
#undef aEnvMixer
#undef aResample
#undef aInterleave
#undef aSetVolume
#undef aSetLoop
#undef aLoadADPCM
#undef aADPCMdec
#undef aPoleFilter
#undef aPan

void aClearBufferImpl(uint16_t addr, int nbytes);
void aLoadBufferImpl(const void *source_addr);
void aSaveBufferImpl(int16_t *dest_addr);
void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr);
void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes);
void aSetVolumeImpl(uint8_t flags, int16_t v, int16_t t, int16_t r);
void aInterleaveImpl(uint16_t left, uint16_t right);
void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes);
void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state);
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state);
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state);
void aEnvMixerImpl(uint8_t flags, ENVMIX_STATE state);
void aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr);
void aPoleFilterImpl(uint8_t flags, int16_t gain, void *state);

/* aSegment programmed the RSP's segment table for the audio task. Every address
 * that reaches this layer is already absolute, so there is nothing to resolve. */
#define aSegment(pkt, s, b)        do { } while (0)

#define aClearBuffer(pkt, d, c)    aClearBufferImpl(d, c)
#define aSetBuffer(pkt, f, i, o, c) aSetBufferImpl(f, i, o, c)
#define aLoadBuffer(pkt, s)        aLoadBufferImpl((const void *)(s))
#define aSaveBuffer(pkt, s)        aSaveBufferImpl((int16_t *)(s))
#define aLoadADPCM(pkt, c, d)      aLoadADPCMImpl(c, (const int16_t *)(d))
#define aDMEMMove(pkt, i, o, c)    aDMEMMoveImpl(i, o, c)
#define aSetLoop(pkt, a)           aSetLoopImpl((ADPCM_STATE *)(a))
#define aADPCMdec(pkt, f, s)       aADPCMdecImpl(f, (int16_t *)(s))
#define aResample(pkt, f, p, s)    aResampleImpl(f, p, (int16_t *)(s))
#define aEnvMixer(pkt, f, s)       aEnvMixerImpl(f, (int16_t *)(s))
#define aMix(pkt, f, g, i, o)      aMixImpl(g, i, o)
#define aSetVolume(pkt, f, v, t, r) aSetVolumeImpl(f, v, t, r)
#define aInterleave(pkt, l, r)     aInterleaveImpl(l, r)
#define aPoleFilter(pkt, f, g, s)  aPoleFilterImpl(f, g, (void *)(s))

/* ------------------------------------------------------------ env.c guards --
 * Rare's own build deliberately kept asserts enabled in env.c and nowhere else in
 * audio: `src/libultrare/Makefile.libultrare:353-354` reads
 *
 *     # assert is used in env.c
 *     $(BUILD_DIR)/src/libultrare/audio/env.o: ASSERT_FLAG :=
 *
 * while every other audio object gets -DNDEBUG. This build passes -DNDEBUG to the
 * whole audio file set, so `assert(samples >= 0)`, `assert(samples <= 160)` and
 * `assert(source)` are compiled out, leaving it less defended than the retail ROM.
 *
 * The asserts cannot simply be restored: on tvOS assert leads to abort and SIGABRT,
 * which per PORTING_PLAYBOOK.md 1.4 presents as a silent hang with no final log line.
 * These are the counted, non-fatal equivalents, and they produce evidence where the
 * assert produced only a crash. `samples` becomes an aSetBuffer byte count into a 4 KB
 * DMEM buffer, so an out-of-range value passing silently is exactly the shape of a
 * launch-to-launch variance bug.
 *
 * GETV_ENVCLAMP=0 counts and reports but does not clamp, so the clamp itself can be
 * A/B'd rather than assumed harmless. */
int  geEnvSamplesGuard(int samples, int maxsamples);
int  geEnvSourceGuard(const void *source);

/* A_PAN is declared by the ABI but no GoldenEye code path emits it. Left undefined
 * on purpose: a call site would then fail to COMPILE rather than silently do
 * nothing at runtime. */

#endif /* GE_MIXER_H */
