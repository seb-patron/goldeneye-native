/* Replay GoldenEye's own attract-mode demos through the port.
 *
 * assets/ramrom/ holds fourteen recorded input streams -- real human play, kept by the game for
 * its title screen. They are ground truth for navigation and, because each block carries an RNG
 * check, a determinism test Rare already wrote.
 *
 * FILE FORMAT, from ramromreplay.c rather than guessed:
 *
 *   header   0xF0 bytes (romCopyAligned(..., 0xf0) at :146)
 *            size_cmds at +24 is the CONTROLLER COUNT, not a byte size (:444 sets it from
 *            joyGetControllerCount) -- Train records TWO, because it was played on a 2.x
 *            twin-stick style, which is the same two-pad layout this port defaults to
 *   body     repeating: [ramrom_seed 4 bytes][seed.count * size_cmds * 4 bytes of input]
 *            (:344 reads the seed, :352 reads that many input bytes, :363 advances)
 *
 *   ramrom_seed     { u8 speedframes, count, randseed, check }
 *   ramrom_blockbuf { s8 stick_x, s8 stick_y, u8 button_low, u8 button_high }
 *
 * 🔑 Pads are INTERLEAVED per frame -- [f0p0][f0p1][f1p0][f1p1] -- not stored one pad after the
 * other. Determined by measuring smoothness rather than by reading: human input changes by about
 * 1 unit per frame, and the wrong interleave shuffles two streams together and reports 15.
 *
 *   GETV_DEMO=<path>    replay this file
 *   GETV_DEMO_TRACE=1   report progress and the seed check
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_player_api.h"

#define GE_DEMO_HDR 0xF0

static unsigned char *ge_dm_blob;
static long ge_dm_size;
static long ge_dm_off;          /* byte offset of the next block */
static int  ge_dm_pads;         /* size_cmds: controllers recorded */
static int  ge_dm_left;         /* frames remaining in the current block */
static int  ge_dm_frame;
static int  ge_dm_trace;
static int  ge_dm_ready;
static int  ge_dm_done;
static int  ge_dm_mismatch;

/* The frame's raw pads, published for ge_playback.
 *
 * A recorded 2.x demo carries BOTH controllers and they are not interchangeable: pad 0 is the
 * look/turn pad and pad 1 is the move pad. Posting only pad 0 through gePlayerPost replays the
 * aiming and none of the walking, which looks like a demo that does not work and is really a
 * reader throwing half the data away.
 *
 * gePlayerPost cannot express this -- it takes one input per SLOT, and here one slot has two
 * pads' worth of recorded state -- so the pads are handed over verbatim instead. */
static signed char ge_dm_pad[4][2];
static unsigned int ge_dm_btn[4];
static int ge_dm_live;

int gePortDemoPads(int pad, signed char *sx, signed char *sy, unsigned int *btn)
{
    if (!ge_dm_live || pad < 0 || pad >= ge_dm_pads) { return 0; }
    if (sx != NULL)  { *sx = ge_dm_pad[pad][0]; }
    if (sy != NULL)  { *sy = ge_dm_pad[pad][1]; }
    if (btn != NULL) { *btn = ge_dm_btn[pad]; }
    return 1;
}

static unsigned int rd_be32(const unsigned char *p)
{
    return ((unsigned int) p[0] << 24) | ((unsigned int) p[1] << 16)
         | ((unsigned int) p[2] << 8)  | (unsigned int) p[3];
}

static void ge_dm_init(void)
{
    const char *path = getenv("GETV_DEMO");
    const char *tr = getenv("GETV_DEMO_TRACE");
    FILE *fp;

    ge_dm_ready = 1;
    ge_dm_trace = (tr != NULL && *tr != '\0' && *tr != '0');
    if (path == NULL || *path == '\0') { return; }

    fp = fopen(path, "rb");
    if (fp == NULL) { printf("[getv][demo] cannot open %s\n", path); return; }
    fseek(fp, 0, SEEK_END);
    ge_dm_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    ge_dm_blob = (unsigned char *) malloc((size_t) ge_dm_size);
    if (ge_dm_blob == NULL || fread(ge_dm_blob, 1, (size_t) ge_dm_size, fp) != (size_t) ge_dm_size) {
        printf("[getv][demo] short read on %s\n", path);
        free(ge_dm_blob);
        ge_dm_blob = NULL;
    }
    fclose(fp);
    if (ge_dm_blob == NULL) { return; }

    ge_dm_pads = (int) rd_be32(ge_dm_blob + 24);
    ge_dm_off = GE_DEMO_HDR;
    /* Refuse rather than guess: a pad count outside this range means the header is not where we
     * think it is, and replaying from a wrong offset produces plausible sticks and nonsense
     * buttons -- which looks like a bad demo rather than a bad reader. */
    if (ge_dm_pads < 1 || ge_dm_pads > 4) {
        printf("[getv][demo] size_cmds=%d is not a controller count -- header layout wrong\n",
               ge_dm_pads);
        free(ge_dm_blob);
        ge_dm_blob = NULL;
        return;
    }

    printf("[getv][demo] %s: stage %u, difficulty %u, %d pad(s), %ld bytes\n",
           path, rd_be32(ge_dm_blob + 16), rd_be32(ge_dm_blob + 20), ge_dm_pads, ge_dm_size);
    gePlayerClaim(0, GE_SLOT_INJECTED);
}

void gePortDemoFrame(int frame)
{
    GePlayerInput in;
    const unsigned char *rec;

    (void) frame;
    if (!ge_dm_ready) { ge_dm_init(); }
    if (ge_dm_blob == NULL || ge_dm_done) { return; }

    if (ge_dm_left <= 0) {
        unsigned char count, randseed;

        if (ge_dm_off + 4 > ge_dm_size) { ge_dm_done = 1; goto finished; }
        count = ge_dm_blob[ge_dm_off + 1];
        randseed = ge_dm_blob[ge_dm_off + 2];
        ge_dm_off += 4;
        if (count == 0 || ge_dm_off + (long) count * ge_dm_pads * 4 > ge_dm_size) {
            ge_dm_done = 1;
            goto finished;
        }

        /* The recorded RNG byte against ours. ramromreplay aborts playback on a mismatch; we
         * COUNT instead and keep going, because the interesting output of a replay is how far it
         * stays in step, not the first frame it does not. */
        if ((unsigned char) (gePlayerSeedFingerprint() & 0xFF) != randseed) { ge_dm_mismatch++; }
        ge_dm_left = count;
    }

    /* Pads interleaved per frame; pad 0 is the one the game reads turn from. */
    rec = ge_dm_blob + ge_dm_off;
    {
        int p;
        for (p = 0; p < ge_dm_pads && p < 4; p++) {
            ge_dm_pad[p][0] = (signed char) rec[p * 4 + 0];
            ge_dm_pad[p][1] = (signed char) rec[p * 4 + 1];
            /* button_low then button_high, as recorded. */
            ge_dm_btn[p] = (unsigned int) rec[p * 4 + 2] | ((unsigned int) rec[p * 4 + 3] << 8);
        }
        ge_dm_live = 1;
    }
    memset(&in, 0, sizeof in);
    in.stick_x = ge_dm_pad[0][0];
    in.stick_y = ge_dm_pad[0][1];

    ge_dm_off += (long) ge_dm_pads * 4;
    ge_dm_left--;
    ge_dm_frame++;

    gePlayerPost(0, gePlayerTick() + 1, &in, 1);

    if (ge_dm_trace && (ge_dm_frame % 300) == 0) {
        printf("[getv][demo] frame %d, %d seed mismatch(es)\n", ge_dm_frame, ge_dm_mismatch);
        fflush(stdout);
    }
    return;

finished:
    printf("[getv][demo] finished: %d frames replayed, %d seed mismatch(es)\n",
           ge_dm_frame, ge_dm_mismatch);
    fflush(stdout);
}
