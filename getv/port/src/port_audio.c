/* GoldenEye tvOS port - the audio manager, replaced.
 *
 * libultra's AL audio library (src/libultra/audio + Rare's src/libultrare/audio)
 * compiles natively and is linked for real - see build.sh's audio_sources(). So
 * alInit / alAudioFrame / alSynAllocVoice / alHeapInit / alLink and the rest are the
 * game's own code, not stubs, and must not be defined here: they would collide.
 *
 * What is left for the port is the two ends of that library:
 *
 *  below it - the RSP that executed the command list. That is getv/port/audio/
 *  ge_mixer.c, wired in by <PR/abi.h> under -DGE_AUDIO_MIXER.
 *  above it - src/audi.c, the audio manager. That file is pure N64: an audio
 *  thread, OSScTask scheduling, AI double-buffering, and a 64-entry DMA
 *  cache that streamed sample bytes off the cartridge. None of it
 *  survives a port, so audi.c is not compiled and this file replaces it.
 *
 * The replacement is much smaller than the original for one reason: with the mixer
 * in place, alAudioFrame() no longer builds a command list for someone else to run
 * later - every a* macro executes immediately, so alAudioFrame() returns with the
 * samples already in outBuf. There is nothing to schedule and nothing to wait for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <PR/ultratypes.h>
#include <PR/libaudio.h>

#include <SDL.h>

/* ------------------------------------------------------------------ config -- */

/* audi.c: OUTPUT_RATE 0x5622. The N64 could only approximate this (osAiSetFrequency
 * returned what the AI hardware actually managed and the game used that); SDL gives
 * us the exact rate, which is strictly better. */
#define GE_OUTPUT_RATE 22050

/* audi.c sized a frame as (outputRate << FRAMES_PER_FIELD_AS_POW2) / MAYBE_FRAME_RATE
 * rounded up to 16 - i.e. two fields' worth at 60 Hz. Same arithmetic here. */
#define GE_FRAME_SAMPLES (((GE_OUTPUT_RATE * 2 / 60) + 15) & ~15) /* 736 */
#define GE_MIN_FRAME (GE_FRAME_SAMPLES - 16)
#define GE_MAX_FRAME (GE_FRAME_SAMPLES + 0x25 + 16) /* + EXTRA_SAMPLES */

/* audi.c: NUMBER_ACMD_LISTS 2, MAX_ACMD_SIZE 3000. Only one list is needed now (the
 * commands are executed as they are "written", so no list is ever in flight), but
 * the AL library still walks a write cursor across it, so it must be real storage of
 * the right size. */
#define GE_ACMD_SIZE 3000

/* How much audio to keep queued on the device, in stereo frames. Two video frames'
 * worth is enough to ride out a slow frame without adding audible latency; the game
 * pushes ~736 samples per frame at 60 Hz. */
#define GE_QUEUE_TARGET (GE_FRAME_SAMPLES * 4)

/* Mirrors GE_DMEM_SIZE in port/audio/ge_mixer.c -- reporting only. */
#define GE_DMEM_REPORT 4096

/* --------------------------------------------------------------------- state */

static ALGlobals    geAlGlobals;
static SDL_AudioDeviceID geAudioDev = 0;
static Acmd        *geCmdList  = NULL;
static s16         *geOutBuf   = NULL;
static s32          geCmdLen   = 0;
static int          geAudioReady = 0;
static int          geAudioStarted = 0;
static unsigned long long geFrames = 0;
/* Our own substitute for SDL_GetQueuedAudioSize -- see gePortAudioFrame. */
static unsigned long long geSubmitted = 0;
static Uint32 geClockStart = 0;

/* Rare's reverb configuration, copied verbatim out of src/audi.c (CUSTOM_FX_PARAMS_N).
 * music.c asks for AL_FX_CUSTOM but never fills in ALSynConfig.params - on the N64
 * audi.c supplied them, so the port has to. Without this alFxNew() reads param[0] off
 * a null pointer.
 *
 * `ms` is audi.c's own macro: `*(((s32)((f32)44.1)) & ~0x7)`, i.e. x40. Expanded here
 * rather than redefined, because a macro that starts with `*` is a trap. */
#define GE_MS(n) ((n) * 40)
static s32 geCustomFxParams[6 * 8 + 2] = {
    /* sections, length */
    6, GE_MS(160),
    /* input output fbcoef ffcoef gain chorus rate chorus depth filter */
    0,           GE_MS(4),     9830,  -9830,        0,      0,     0,  0x0000,
    GE_MS(4),    GE_MS(8),     9830,  -9830,   0x2B84,      0,     0,  0x2500,
    GE_MS(20),   GE_MS(64),   16384, -16384,   0x11EB,      0,     0,  0x3000,
    GE_MS(80),   GE_MS(140),  16384, -16384,   0x11EB,      0,     0,  0x3500,
    GE_MS(84),   GE_MS(120),   8192,  -8192,        0,      0,     0,  0x4000,
    0,           GE_MS(148),  13000, -13000,        0, 0x017C,   0xA,  0x4500,
};


/* ------------------------------------------------------------- GETV_AUDIO_DEBUG */

/* The simulator produces no audible output, so audio has to be verified by
 * measurement. This block is that measurement: it reports, per frame, exactly what was
 * handed to SDL and what the engine's own state was when it was produced.
 *
 *  GETV_AUDIO_DEBUG=N report every N audio frames. 1 = every frame, 60 ~= once
 *  per two seconds of video. Any other non-zero value works.
 *
 * The necessary numbers are rmsL/rmsR and pk. RMS is computed over the same buffer
 * that is passed to SDL_QueueAudio, after the mixer has run, so a non-zero RMS is
 * proof that non-silent PCM reached the device -- not an inference from "the sequence
 * player says it is playing".
 *
 * Non-zero RMS proves signal, not correctness: noise, a stuck DC offset and the right
 * music all have a non-zero RMS. `nz` (frames whose RMS was above the floor) and `pk`
 * (peak absolute sample) separate a click at startup from continuous audio; whether it
 * is the right audio still has to be judged by listening.
 */
static int geDbgEvery = -1;              /* -1 = not yet read from the environment */
static unsigned long long geDbgNonSilent = 0;
static unsigned long long geDbgSamples   = 0;
static int geDbgPeakAll = 0;
static double geDbgRmsAcc = 0.0;
static unsigned long geDbgLastOps = 0;
static int geDbgProbeArmed = 0;
static unsigned long geSavesBefore = 0;

/* music.c's own state. Declared here rather than included because music.h is not on the
 * port layer's include path (the decomp ships headers that shadow the system ones -- see
 * docs/ROADMAP.md). Types are from <PR/libaudio.h>, which IS included above. */
extern ALCSPlayer *g_musicXTrack1SeqPlayer;
extern ALCSPlayer *g_musicXTrack2SeqPlayer;
extern ALCSPlayer *g_musicXTrack3SeqPlayer;
extern s32 g_musicXTrack1CurrentTrackNum;
extern s32 g_musicXTrack2CurrentTrackNum;
extern s32 g_musicXTrack3CurrentTrackNum;
/* The PRE-SCALE volume music.c keeps, and the fade state machine that writes it.
 * Comparing this against the player's own seqp->vol splits "the game asked for zero"
 * from "the game asked for full volume and the event never landed". */
extern unsigned short g_musicXTrack1Volume;
extern s32 g_musicXTrack1Fade;
extern s32 g_musicXTrack1FadeRemainingFrames;

/* Set by music.c / snd.c when the GAME asks for audio, so a silent run can be split into
 * "the game never asked" and "the game asked and nothing came out". */
unsigned long gePortAudioReqMusic = 0;
unsigned long gePortAudioReqSfx   = 0;

int gePortAudioDebugLevel(void)
{
    if (geDbgEvery < 0) {
        const char *e = getenv("GETV_AUDIO_DEBUG");
        if (e == NULL || *e == '\0') {
            geDbgEvery = 0;
        } else {
            geDbgEvery = (int)strtol(e, NULL, 0);
            if (geDbgEvery < 0) { geDbgEvery = 0; }
        }
    }
    if (geDbgEvery > 0 && !geDbgProbeArmed) {
        extern int ge_mixer_probe;
        ge_mixer_probe = 1;
        geDbgProbeArmed = 1;
    }
    return geDbgEvery;
}

/* Counts live voices the way the AL library itself would: walk the player's allocated
 * voice-state list. Cheap, and it is the number that says whether the sequence player
 * turned MIDI events into actual synth voices. */
static int geDbgVoices(ALCSPlayer *p)
{
    int n = 0;
    ALVoiceState *v;
    if (p == NULL) { return -1; }
    for (v = p->vAllocHead; v != NULL && n < 256; v = v->next) {
        n++;
    }
    return n;
}

/* __vsVol (libultra/audio/seqplayer.c) is the only producer of the voice volume that
 * reaches ALStartParamAlt.volume, and it is a product of six factors:
 *
 *  t1 = (tremelo * velocity * envGain) >> 6
 *  t2 = (sound->sampleVolume * seqp->vol * chanState[channel].vol) >> 14
 *  vol = (t1 * t2) >> 15
 *
 * A single zero factor zeroes the whole thing, so when the envelope target arrives as 0
 * with a full-scale input, one of these six is zero. Print all six per live voice
 * rather than guessing which. */
static void geDbgVoiceVols(ALCSPlayer *p, int which)
{
    ALVoiceState *v;
    int n = 0;

    if (p == NULL) { return; }
    printf("[getv] audiodbg vsvol t%d: seqp->vol=%d g_vol=%u fade=%d fadeframes=%d",
           which, (int)p->vol, (unsigned)g_musicXTrack1Volume,
           (int)g_musicXTrack1Fade, (int)g_musicXTrack1FadeRemainingFrames);
    for (v = p->vAllocHead; v != NULL && n < 8; v = v->next, n++) {
        int chvol = (p->chanState != NULL) ? (int)p->chanState[v->channel].vol : -1;
        printf(" | v%d ch=%d trem=%d vel=%d env=%d samp=%d chvol=%d",
               n, (int)v->channel, (int)v->tremelo, (int)v->velocity,
               (int)v->envGain,
               (v->sound != NULL) ? (int)v->sound->sampleVolume : -1,
               chvol);
    }
    printf("\n");
}

/* ------------------------------------------------------------ SFX self-test -- */

/* A Dam boot never fires a weapon, so `req=[...,s0]` -- the SFX path is untested by
 * simply booting. This drives the game's own entry point, the exact call the weapon
 * code makes:
 *
 *  sndPlaySfx(g_musicSfxBufferPtr, <id>, NULL)
 *
 * so it exercises the real sfx bank, sndSetupSound, voice allocation and the same
 * synth path as music -- no shortcut, nothing invented.
 *
 *  GETV_AUDIO_TESTSFX=<id> fire sound <id> every 60 audio frames
 *  107 = GUN_B2_HEAVY_SFX (the PPK), 109 = AK47
 *
 * A non-NULL return means the player allocated an ALSoundState and started it; NULL
 * means it refused (no free voice, or the sound index is empty). Both are reported --
 * "it returned NULL" is a completely different finding from "it played and was
 * inaudible".
 *
 * g_musicSfxBufferPtr is `ALBank *` and sndPlaySfx takes `struct ALBankAlt_s *`; the
 * game itself casts between them at its own call sites. Declared with void * here
 * because snd.h is not on the port layer's include path -- ABI-identical for pointer
 * arguments on arm64.
 */
extern void *g_musicSfxBufferPtr;
extern void *sndPlaySfx(void *soundBank, s16 soundIndex, void *pendingState);

static int geSfxTestId = -1;

static void geSfxTest(void)
{
    if (geSfxTestId < 0) {
        const char *e = getenv("GETV_AUDIO_TESTSFX");
        geSfxTestId = (e != NULL && *e != '\0') ? (int)strtol(e, NULL, 0) : 0;
    }
    if (geSfxTestId <= 0 || g_musicSfxBufferPtr == NULL) {
        return;
    }
    if ((geFrames % 60ULL) == 20ULL) {
        void *st = sndPlaySfx(g_musicSfxBufferPtr, (s16)geSfxTestId, NULL);
        printf("[getv] audiodbg: TESTSFX id=%d -> state=%p (%s)\n",
               geSfxTestId, st, (st != NULL) ? "started" : "REFUSED");
        fflush(stdout);
    }
}

static void geDbgFrame(const s16 *buf, s32 frames, s32 queued, s32 cmdLen)
{
    double sl = 0.0, sr = 0.0;
    int pk = 0;
    s32 i;
    int every = gePortAudioDebugLevel();
    double rmsL, rmsR;

    for (i = 0; i < frames; i++) {
        int l = buf[i * 2];
        int r = buf[i * 2 + 1];
        sl += (double)l * (double)l;
        sr += (double)r * (double)r;
        if (l < 0) { l = -l; }
        if (r < 0) { r = -r; }
        if (l > pk) { pk = l; }
        if (r > pk) { pk = r; }
    }
    rmsL = (frames > 0) ? sqrt(sl / (double)frames) : 0.0;
    rmsR = (frames > 0) ? sqrt(sr / (double)frames) : 0.0;

    geDbgSamples += (unsigned long long)frames;
    if (pk > geDbgPeakAll) { geDbgPeakAll = pk; }
    /* "Non-silent" means an RMS above 0.5 LSB, i.e. at least a few non-zero samples --
     * a floor on the raw value, not a dB threshold, so it cannot be tuned
     * into reporting success. */
    if (rmsL > 0.5 || rmsR > 0.5) { geDbgNonSilent++; }
    geDbgRmsAcc += (rmsL + rmsR) * 0.5;

    if (every <= 0 || (geFrames % (unsigned long long)every) != 0) {
        return;
    }

    {
        extern unsigned long ge_mixer_ops, ge_mixer_saves, ge_mixer_hi, ge_mixer_overflow;
        extern unsigned long ge_mixer_ovf_flags, ge_mixer_ovf_in, ge_mixer_ovf_out,
                             ge_mixer_ovf_nbytes;
        if (ge_mixer_overflow != 0) {
            printf("[getv] audiodbg: DMEM OVERFLOW x%lu, first was "
                   "aSetBuffer(flags=0x%lx in=%lu out=%lu nbytes=%lu)\n",
                   ge_mixer_overflow, ge_mixer_ovf_flags, ge_mixer_ovf_in,
                   ge_mixer_ovf_out, ge_mixer_ovf_nbytes);
        }
        printf("[getv] audiodbg f=%llu n=%d q=%d cmd=%d | rmsL=%.1f rmsR=%.1f pk=%d "
               "nz=%llu/%llu pkAll=%d | mixops=%lu(+%lu) saves=%lu dmem=%lu/%d ovf=%lu | "
               "trk=[%d,%d,%d] state=[%d,%d,%d] voices=[%d,%d,%d] req=[m%lu,s%lu]\n",
               geFrames, (int)frames, (int)queued, (int)cmdLen,
               rmsL, rmsR, pk,
               geDbgNonSilent, geFrames + 1, geDbgPeakAll,
               ge_mixer_ops, ge_mixer_ops - geDbgLastOps, ge_mixer_saves,
               ge_mixer_hi, GE_DMEM_REPORT, ge_mixer_overflow,
               (int)g_musicXTrack1CurrentTrackNum, (int)g_musicXTrack2CurrentTrackNum,
               (int)g_musicXTrack3CurrentTrackNum,
               g_musicXTrack1SeqPlayer ? (int)alCSPGetState(g_musicXTrack1SeqPlayer) : -1,
               g_musicXTrack2SeqPlayer ? (int)alCSPGetState(g_musicXTrack2SeqPlayer) : -1,
               g_musicXTrack3SeqPlayer ? (int)alCSPGetState(g_musicXTrack3SeqPlayer) : -1,
               geDbgVoices(g_musicXTrack1SeqPlayer),
               geDbgVoices(g_musicXTrack2SeqPlayer),
               geDbgVoices(g_musicXTrack3SeqPlayer),
               gePortAudioReqMusic, gePortAudioReqSfx);
        {
            /* The chain, in signal order. The first zero is the stage that killed the
             * audio; everything downstream of it is zero for the trivial reason that
             * its input was. A stage with count 0 was never called, which is a
             * different finding from a stage that ran and produced silence. */
            extern int ge_mixer_pk_loadbuf, ge_mixer_pk_adpcm, ge_mixer_pk_resample;
            extern int ge_mixer_pk_envmix, ge_mixer_pk_mix, ge_mixer_pk_interleave;
            extern int ge_mixer_pk_save;
            extern unsigned long ge_mixer_n_adpcm, ge_mixer_n_resample;
            extern unsigned long ge_mixer_n_envmix, ge_mixer_n_mix, ge_mixer_n_interleave;
            printf("[getv] audiodbg chain: load=%d adpcm=%d/%lu resample=%d/%lu "
                   "envmix=%d/%lu mix=%d/%lu interleave=%d/%lu save=%d   (peak/calls)\n",
                   ge_mixer_pk_loadbuf,
                   ge_mixer_pk_adpcm, ge_mixer_n_adpcm,
                   ge_mixer_pk_resample, ge_mixer_n_resample,
                   ge_mixer_pk_envmix, ge_mixer_n_envmix,
                   ge_mixer_pk_mix, ge_mixer_n_mix,
                   ge_mixer_pk_interleave, ge_mixer_n_interleave,
                   ge_mixer_pk_save);
            {
                extern int ge_mixer_pk_envmix_in, ge_mixer_em_vol0, ge_mixer_em_vol1;
                extern int ge_mixer_em_dry, ge_mixer_em_wet, ge_mixer_em_tgt0;
                extern int ge_mixer_em_rate0, ge_mixer_em_flags;
                printf("[getv] audiodbg envmix: in_pk=%d vol=[%d,%d] dry=%d wet=%d "
                       "target0=%d rate0=%d flags=0x%x\n",
                       ge_mixer_pk_envmix_in, ge_mixer_em_vol0, ge_mixer_em_vol1,
                       ge_mixer_em_dry, ge_mixer_em_wet, ge_mixer_em_tgt0,
                       ge_mixer_em_rate0, ge_mixer_em_flags);
            }
        }
        geDbgVoiceVols(g_musicXTrack1SeqPlayer, 1);
        fflush(stdout);
        geDbgLastOps = ge_mixer_ops;
    }
}


/* ------------------------------------------------------------------ WAV dump -- */

/* The one thing measurement cannot settle is whether the output sounds like GoldenEye.
 * RMS proves "not silence"; it cannot tell music from noise. So write the exact bytes
 * handed to SDL to a .wav inside the app sandbox, which can be pulled off the simulator
 * with
 *  xcrun simctl get_app_container <udid> org.goldeneyenative.getv data
 * and listened to, or analysed offline, without anyone sitting in front of the TV.
 *
 *  GETV_AUDIO_WAV=1 dump to $HOME/Documents/getv_audio.wav
 *  GETV_AUDIO_WAV=<path> dump there instead
 *
 * The header is written with a placeholder length and patched on every flush, so the
 * file is playable even if the process is killed mid-run.
 */
static FILE *geWavFp = NULL;
static int   geWavTried = 0;
static unsigned long long geWavFrames = 0;

static void geWavPutU32(FILE *f, unsigned v)
{
    fputc((int)(v & 0xff), f);        fputc((int)((v >> 8) & 0xff), f);
    fputc((int)((v >> 16) & 0xff), f); fputc((int)((v >> 24) & 0xff), f);
}

static void geWavPutU16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xff), f); fputc((int)((v >> 8) & 0xff), f);
}

static void geWavOpen(void)
{
    const char *e = getenv("GETV_AUDIO_WAV");
    char path[1024];

    geWavTried = 1;
    if (e == NULL || *e == '\0' || (e[0] == '0' && e[1] == '\0')) {
        return;
    }
    if (e[0] == '/') {
        snprintf(path, sizeof(path), "%s", e);
    } else {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/Documents/getv_audio.wav",
                 (home != NULL) ? home : ".");
    }

    geWavFp = fopen(path, "wb");
    if (geWavFp == NULL) {
        printf("[getv] audiodbg: WAV open FAILED for '%s'\n", path);
        fflush(stdout);
        return;
    }
    fwrite("RIFF", 1, 4, geWavFp);  geWavPutU32(geWavFp, 36);
    fwrite("WAVEfmt ", 1, 8, geWavFp);
    geWavPutU32(geWavFp, 16);
    geWavPutU16(geWavFp, 1);                       /* PCM        */
    geWavPutU16(geWavFp, 2);                       /* stereo     */
    geWavPutU32(geWavFp, GE_OUTPUT_RATE);
    geWavPutU32(geWavFp, GE_OUTPUT_RATE * 4);      /* byte rate  */
    geWavPutU16(geWavFp, 4);                       /* block align*/
    geWavPutU16(geWavFp, 16);                      /* bits       */
    fwrite("data", 1, 4, geWavFp);  geWavPutU32(geWavFp, 0);
    printf("[getv] audiodbg: WAV -> %s\n", path);
    fflush(stdout);
}

static void geWavWrite(const s16 *buf, s32 frames)
{
    long data_bytes;

    if (!geWavTried) { geWavOpen(); }
    if (geWavFp == NULL) { return; }

    fwrite(buf, 4, (size_t)frames, geWavFp);
    geWavFrames += (unsigned long long)frames;

    /* Patch both length fields every 16,384 sample-frames (~0.74 s at 22,050 Hz) so a
     * killed run still leaves a valid, playable file. Timed runs are terminated by
     * `timeout`, so this is the common case rather than the exception. */
    if ((geWavFrames & 0x3fff) < (unsigned long long)frames) {
        data_bytes = (long)(geWavFrames * 4ULL);
        fflush(geWavFp);
        fseek(geWavFp, 4, SEEK_SET);  geWavPutU32(geWavFp, (unsigned)(36 + data_bytes));
        fseek(geWavFp, 40, SEEK_SET); geWavPutU32(geWavFp, (unsigned)data_bytes);
        fseek(geWavFp, 0, SEEK_END);
        fflush(geWavFp);
    }
}

/* ------------------------------------------------------------------ the DMA -- */

/* On the N64 this callback was the whole reason audi.c had 64 DMA buffers: sample
 * data lived on the cartridge and had to be pulled into RAM a chunk at a time, and
 * the return value was the RAM address the RSP should read from.
 *
 * Here every sample byte is already in memory - the .tbl banks are linked-in data and
 * gePortAudioBankNew() has already turned each ALWaveTable.base into a real pointer
 * into them. So the "DMA" is the identity, and the DMA cache disappears entirely.
 *
 * This only works because ALDMAproc was widened from s32 to intptr_t. As declared by
 * the original SDK it would truncate a 64-bit address to 32 bits, and the sound that
 * came out would be whatever happened to live at the bottom 4 GB.
 */
static intptr_t gePortAudioDma(intptr_t addr, s32 len, void *state)
{
    (void)len;
    (void)state;
    return addr;
}

static ALDMAproc gePortAudioDmaNew(void *state)
{
    (void)state;
    return gePortAudioDma;
}

/* ------------------------------------------------------------- audio manager */

void amCreateAudioManager(ALSynConfig *alconf)
{
    SDL_AudioSpec want, have;

    alconf->dmaproc    = (void *)gePortAudioDmaNew;
    alconf->outputRate = GE_OUTPUT_RATE;

    /* music.c leaves params unset and maxFXbusses uninitialised. maxFXbusses is
     * harmless - ge's alSynNew hardcodes maxAuxBusses = 1 and never reads it - but
     * params is dereferenced immediately by alFxNew(). */
    if (alconf->fxType == AL_FX_CUSTOM) {
        alconf->params = geCustomFxParams;
    }

    alInit(&geAlGlobals, alconf);

    /* Both of these come out of the game's own audio heap, as audi.c did, so
     * MUSIC_ALLOCATION_BYTES still accounts for them. */
    geCmdList = (Acmd *)alHeapAlloc(alconf->heap, 1, GE_ACMD_SIZE * sizeof(Acmd));
    geOutBuf  = (s16 *)alHeapAlloc(alconf->heap, 1, GE_MAX_FRAME * 4);

    printf("[getv] audio: heap base=%p cur=%p len=%d | cmdList=%p (%d B) outBuf=%p (%d B) end=%p\n",
           (void *)alconf->heap->base, (void *)alconf->heap->cur, alconf->heap->len,
           (void *)geCmdList, (int)(GE_ACMD_SIZE * sizeof(Acmd)),
           (void *)geOutBuf, GE_MAX_FRAME * 4,
           (void *)(alconf->heap->base + alconf->heap->len));
    fflush(stdout);

    if (geCmdList == NULL || geOutBuf == NULL) {
        printf("[getv] audio: out of audio heap (cmdList=%p outBuf=%p) -- staying silent\n",
               (void *)geCmdList, (void *)geOutBuf);
        return;
    }

    SDL_zero(want);
    want.freq     = GE_OUTPUT_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 512;
    want.callback = NULL;          /* queue-driven, like Perfect Dark's port */

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        printf("[getv] audio: SDL_INIT_AUDIO failed: %s\n", SDL_GetError());
        return;
    }

    geAudioDev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (geAudioDev == 0) {
        printf("[getv] audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }

    SDL_PauseAudioDevice(geAudioDev, 0);
    geAudioReady = 1;

    printf("[getv] audio: %d Hz stereo, %d samples/frame, device %u\n",
           GE_OUTPUT_RATE, GE_FRAME_SAMPLES, (unsigned)geAudioDev);
}

/* On the N64 this started the audio thread, which then drove itself off the video
 * retrace. This port is single-threaded and the frame loop drives it instead, so all
 * this does is arm gePortAudioFrame(). */
void amStartAudioThread(void)
{
    geAudioStarted = 1;
    printf("[getv] audio: frame driver armed\n");
}

/* ----------------------------------------------------------------- the frame */

/* Called once per video frame from the tvOS harness, in place of audi.c's amMain()
 * thread loop.
 *
 * Must be called after the game's own logic for the frame, for the same reason
 * amMain() ran off the retrace message: the sequence and sound players are driven
 * from inside alAudioFrame(), so anything the game does to them afterwards lands a
 * frame late.
 */
void gePortAudioFrame(void)
{
    s32 queued, want;

    if (!geAudioReady || !geAudioStarted) {
        return;
    }

    /* Playback is on by default. It was opt-in while two 64-bit layout bugs made it
     * crash the next video frame; both are fixed and named in docs/ROADMAP.md
     * (ALDelay's unsigned backwards index, and ALParam's union-by-casting pool record).
     *
     * GETV_NO_AUDIO=1 turns playback off again without touching init, which keeps the
     * subsystem all-real either way. A half-initialised subsystem is the thing that
     * breaks; a switch that skips only the output stage does not. */
    if (getenv("GETV_NO_AUDIO")) {
        if (geFrames == 0) {
            printf("[getv] audio: playback disabled by GETV_NO_AUDIO (init is still real)\n");
            fflush(stdout);
        }
        geFrames++;
        return;
    }

    /* audi.c recalculated this every frame to keep the DAC exactly full, because audio
     * was clocked off the video interrupt and the two drift. That feedback is not
     * optional here either: GE_FRAME_SAMPLES is two fields' worth (audi.c's
     * FRAMES_PER_FIELD_AS_POW2), so submitting one per 60 Hz video frame is twice as
     * much audio as the device consumes, and the queue would grow without bound.
     *
     * Tracked here rather than read back with SDL_GetQueuedAudioSize(). Both work; this
     * one has no dependency on the audio backend's bookkeeping, and it drifts only as
     * far as SDL_GetTicks does. SDL_GetQueuedAudioSize was once suspected of causing
     * the frame-1 crash described at the head of gePortAudioFrame; it does not, and
     * replacing it with the accounting below changed nothing. */
    {
        Uint32 now = SDL_GetTicks();
        unsigned long long consumed;

        if (geClockStart == 0) {
            geClockStart = now;
        }
        consumed = (unsigned long long)(now - geClockStart) * GE_OUTPUT_RATE / 1000ULL;

        queued = (geSubmitted > consumed) ? (s32)(geSubmitted - consumed) : 0;
    }

    want = GE_QUEUE_TARGET - queued;
    if (want > GE_MAX_FRAME) {
        want = GE_MAX_FRAME;
    }
    want &= ~0xf;                    /* the synth requires a 16-sample boundary */
    if (want < GE_MIN_FRAME) {
        /* Already comfortably ahead. Rendering nothing is correct: the sequence
         * players advance by samples, so skipping a frame here does not drop events,
         * it just defers them. */
        if (queued >= GE_QUEUE_TARGET) {
            return;
        }
        want = GE_MIN_FRAME;
    }

    if (geFrames < 4) {
        printf("[getv] af%llu: want=%d queued=%d buf=%p cmd=%p\n",
               geFrames, want, queued, (void *)geOutBuf, (void *)geCmdList);
        fflush(stdout);
    }

    geSfxTest();

    geCmdLen = 0;
    /* Snapshot the mixer's save counter so we can tell afterwards whether this call
     * rendered anything -- see the memset below for why cmdLen cannot. */
    {
        extern unsigned long ge_mixer_saves;
        geSavesBefore = ge_mixer_saves;
    }
    alAudioFrame(geCmdList, &geCmdLen, geOutBuf, want);

    if (geFrames < 4) {
        extern unsigned long ge_mixer_ops, ge_mixer_saves, ge_mixer_hi, ge_mixer_overflow;
        printf("[getv] af%llu: alAudioFrame ok, %d cmds, mixer ops=%lu saves=%lu "
               "dmem_hi=%lu/%d overflow=%lu\n",
               geFrames, geCmdLen, ge_mixer_ops, ge_mixer_saves,
               ge_mixer_hi, 4096, ge_mixer_overflow);
        fflush(stdout);
    }

    /* alAudioFrame returns immediately, writing nothing, when no player has been added
     * yet (drvr->head == 0). The buffer then still holds the previous frame, and
     * queueing it would loop the last 16 ms of audio forever. So it must be cleared --
     * but only in that case.
     *
     * Do not test `geCmdLen == 0` here. cmdLen is always 0 under the software mixer:
     * the a* macros execute immediately and discard their `pkt` argument, so the AL
     * library's write cursor never advances and alAudioFrame always reports 0 commands,
     * even on a full synthesis pass. Testing it makes the condition always true, and
     * this memset then zeroes every freshly-rendered frame immediately before it goes
     * to SDL -- i.e. total silence.
     *
     * The reliable signal is whether the mixer saved anything: aSaveBuffer is the only
     * thing that writes to the DRAM output, so if its counter did not move, nothing was
     * rendered. */
    {
        extern unsigned long ge_mixer_saves;
        int rendered = (ge_mixer_saves != geSavesBefore);
        if (!rendered && !getenv("GETV_NO_AUDIO_MEMSET")) {
            memset(geOutBuf, 0, (size_t)want * 4);
        }
    }

    /* Measure the exact buffer that is about to be handed to the device. */
    if (gePortAudioDebugLevel() > 0) {
        geDbgFrame(geOutBuf, want, queued, geCmdLen);
    }
    geWavWrite(geOutBuf, want);

    if (geFrames < 4) { printf("[getv] af%llu: -> SDL_QueueAudio\n", geFrames); fflush(stdout); }
    if (!getenv("GETV_NO_AUDIO_QUEUE")) SDL_QueueAudio(geAudioDev, geOutBuf, (Uint32)want * 4);
    geSubmitted += (unsigned long long)want;
    if (geFrames < 4) { printf("[getv] af%llu: queued ok\n", geFrames); fflush(stdout); }

    if (geFrames++ == 0) {
        printf("[getv] audio: first frame rendered, %d samples, %d commands\n",
               want, geCmdLen);
    }
}

/* Reports whether a real backend came up, so the harness can log it and so the
 * musicSeqPlayerInit() call site has something to test. */
int gePortAudioIsLive(void)
{
    return geAudioReady;
}
