/* Replay a recorded attract-mode demo through the player API.
 *
 * The ROM ships fourteen recorded input streams in assets/ramrom. They are not video and not a
 * bot: they are what a person pressed, kept frame by frame, on levels this port is still learning
 * to finish. Feeding one back through gePlayerPost gives two things at once -- a reference route
 * walked by someone who could see the screen, and a determinism check with fourteen recorded cases
 * to run it against.
 *
 *   GETV_RAMROM=<path>     a demo .bin to replay. Off unless set.
 *   GETV_RAMROM_SLOT=<n>   slot to drive, default 0.
 *   GETV_RAMROM_TRACE=1    log each block boundary and the seed comparison.
 *
 * FORMAT, derived rather than assumed. tools/audit_ramrom_header.py is the regression check and
 * carries the full derivation; the short version is that ramromreplay.c:453 advances the cursor by
 * sizeof(struct ramromfilestructure), so the header length IS that struct's size:
 *
 *   header   232 bytes. s32 filesize at 128, enum LEVELID stagenum at 16, u32 size_cmds at 24.
 *   stream   repeating: a 4-byte {speedframes, count, randseed, check}, then size_cmds*4*count
 *            bytes of {s8 stick_x, s8 stick_y, u8 button_low, u8 button_high}. Ends on a block
 *            with count and speedframes both zero.
 *
 * The file is BIG-ENDIAN and this host is not, so every multi-byte read goes through be32 rather
 * than being cast. The pad word is (button_high << 8) | button_low -- low byte FIRST in memory,
 * which is not what a big-endian target suggests, and which was settled by the fact that bits
 * 0x0040 and 0x0080 are assigned to no button on a real N64 pad: they are clear in all 32,469
 * records under this order and set in 173 under the other.
 *
 * WHAT THIS DOES NOT DO, stated because the difference decides how to read its output: it feeds
 * recorded INPUT. It does not reproduce the N64's frame pacing. The seed block carries a
 * speedframes field that the original passes to updateFrameCounters, and nothing here consumes it.
 * So a divergence reported below is not by itself a port bug -- this is the instrument that finds
 * where the two timelines part, not proof that the port is wrong at that point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_player_api.h"

/* N64 pad bits (PR/os.h). Named here rather than included: this file builds against the port,
 * which does not pull in the ultra64 headers. */
#define GE_PAD_A        0x8000u
#define GE_PAD_B        0x4000u
#define GE_PAD_Z        0x2000u
#define GE_PAD_START    0x1000u
#define GE_PAD_DU       0x0800u
#define GE_PAD_DD       0x0400u
#define GE_PAD_DL       0x0200u
#define GE_PAD_DR       0x0100u
#define GE_PAD_L        0x0020u
#define GE_PAD_R        0x0010u
#define GE_PAD_CU       0x0008u
#define GE_PAD_CD       0x0004u
#define GE_PAD_CL       0x0002u
#define GE_PAD_CR       0x0001u

#define GE_RAMROM_HDR   232u
#define GE_OFF_STAGE     16u
#define GE_OFF_CMDS      24u
#define GE_OFF_FILESIZE 128u

static unsigned char *ge_rr_buf;
static unsigned long  ge_rr_len;
static unsigned long  ge_rr_pos;        /* cursor: start of the current block's seed record */
static unsigned long  ge_rr_rec;        /* input records left in the current block */
static unsigned long  ge_rr_recoff;     /* cursor into the current block's input records */
static unsigned int   ge_rr_cmds;
static int            ge_rr_slot;
static int            ge_rr_trace;
static int            ge_rr_ready;
static int            ge_rr_done;
static unsigned long  ge_rr_blocks;
static unsigned long  ge_rr_frames;
static unsigned long  ge_rr_agree;      /* blocks whose recorded seed matched ours */
static unsigned long  ge_rr_checked;
static long           ge_rr_first_bad = -1;

static unsigned int be32(const unsigned char *p)
{
    return ((unsigned int) p[0] << 24) | ((unsigned int) p[1] << 16) |
           ((unsigned int) p[2] << 8)  |  (unsigned int) p[3];
}

/* N64 pad word to GE_IN_*. This INVERTS the forward mapping in ge_player_api.c rather than
 * inventing one, so the two cannot drift apart.
 *
 * L_TRIG and R_TRIG both carry AIM in the default single-controller styles: bondview2.c:5546-5558
 * gives KISSY and GOODNIGHT shoot=A, aim=Z, inv=L|R, and every other style shoot=Z, aim=L|R,
 * inv=A. The corpus matches the second -- Z most-held at 11.6%, A at 0.1%, L_TRIG at 2.8% -- so
 * these demos were recorded on a default style. ge_player_api.c already maps GE_IN_AIM back onto
 * L_TRIG|R_TRIG for those styles, so the round trip is faithful and neither side needs a special
 * case.
 *
 * Not static: netplay's local-input capture (ge_net_udp.c) needs this exact conversion too, to
 * turn what joyGetButtons() reads on this machine into the GE_IN_* a GePlayerInput carries over
 * the wire. Declared in ge_player_api.h, next to ge_pad_from_input, the direction it inverts. */
unsigned int gePlayerButtonsFromPad(unsigned int pad)
{
    unsigned int b = 0;
    if (pad & GE_PAD_Z)                 { b |= GE_IN_FIRE; }
    if (pad & (GE_PAD_L | GE_PAD_R))    { b |= GE_IN_AIM; }
    if (pad & GE_PAD_A)                 { b |= GE_IN_WEAPON_NEXT; }  /* inventory, default styles */
    if (pad & GE_PAD_B)                 { b |= GE_IN_USE; }
    if (pad & GE_PAD_START)             { b |= GE_IN_START; }
    if (pad & GE_PAD_CU)                { b |= GE_IN_LOOK_UP; }
    if (pad & GE_PAD_CD)                { b |= GE_IN_LOOK_DOWN; }
    if (pad & GE_PAD_CL)                { b |= GE_IN_STEP_LEFT; }
    if (pad & GE_PAD_CR)                { b |= GE_IN_STEP_RIGHT; }
    if (pad & GE_PAD_DU)                { b |= GE_IN_DPAD_UP; }
    if (pad & GE_PAD_DD)                { b |= GE_IN_DPAD_DOWN; }
    if (pad & GE_PAD_DL)                { b |= GE_IN_DPAD_LEFT; }
    if (pad & GE_PAD_DR)                { b |= GE_IN_DPAD_RIGHT; }
    return b;
}

static void ge_rr_init(void)
{
    const char *path = getenv("GETV_RAMROM");
    const char *slot = getenv("GETV_RAMROM_SLOT");
    FILE *fh;
    unsigned int stored, stage;

    ge_rr_ready = 1;
    if (!path || !*path) { return; }

    ge_rr_slot  = slot ? atoi(slot) : 0;
    ge_rr_trace = getenv("GETV_RAMROM_TRACE") != NULL;

    fh = fopen(path, "rb");
    if (!fh) {
        printf("[getv][ramrom] cannot open %s\n", path);
        fflush(stdout);
        return;
    }
    fseek(fh, 0, SEEK_END);
    ge_rr_len = (unsigned long) ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (ge_rr_len <= GE_RAMROM_HDR) {
        printf("[getv][ramrom] %s is %lu bytes, shorter than the %u-byte header\n",
               path, ge_rr_len, GE_RAMROM_HDR);
        fflush(stdout);
        fclose(fh);
        return;
    }
    ge_rr_buf = (unsigned char *) malloc(ge_rr_len);
    if (!ge_rr_buf || fread(ge_rr_buf, 1, ge_rr_len, fh) != ge_rr_len) {
        printf("[getv][ramrom] short read on %s\n", path);
        fflush(stdout);
        fclose(fh);
        free(ge_rr_buf);
        ge_rr_buf = NULL;
        return;
    }
    fclose(fh);

    /* VALIDATE BEFORE DRIVING ANYTHING. A wrong file, or a right file read with a wrong header
     * length, produces plausible sticks and garbage buttons -- it looks like it is working.
     *
     * The stored filesize is the demo's unpadded length and the file on disk is padded up to a
     * 16-byte ROM boundary, so the test is align16(stored) rather than equality: only 3 of the 14
     * demos happen to land exactly on a boundary, and testing for equality would reject the other
     * eleven as corrupt. */
    stored = be32(ge_rr_buf + GE_OFF_FILESIZE);
    stage  = be32(ge_rr_buf + GE_OFF_STAGE);
    if (((stored + 15u) & ~15u) != (unsigned int) ge_rr_len) {
        printf("[getv][ramrom] %s: stored filesize %u does not pad to the actual %lu. "
               "Refusing rather than decoding garbage.\n", path, stored, ge_rr_len);
        fflush(stdout);
        free(ge_rr_buf);
        ge_rr_buf = NULL;
        return;
    }

    ge_rr_cmds = be32(ge_rr_buf + GE_OFF_CMDS);
    if (ge_rr_cmds == 0u || ge_rr_cmds > 4u) {
        printf("[getv][ramrom] %s: size_cmds is %u, outside the 1..4 seen in every demo\n",
               path, ge_rr_cmds);
        fflush(stdout);
        free(ge_rr_buf);
        ge_rr_buf = NULL;
        return;
    }

    ge_rr_pos    = GE_RAMROM_HDR;
    ge_rr_rec    = 0;
    ge_rr_recoff = 0;

    gePlayerApiInit();
    gePlayerClaim(ge_rr_slot, GE_SLOT_INJECTED);
    printf("[getv][ramrom] %s: stage %u, size_cmds %u, %lu bytes, driving slot %d\n",
           path, stage, ge_rr_cmds, ge_rr_len, ge_rr_slot);
    fflush(stdout);
}

/* Advance to the next block. Returns 0 at the terminator or on a malformed length. */
static int ge_rr_next_block(void)
{
    unsigned int  count, seedbyte, ours;
    unsigned char speedframes;
    unsigned long bytes;

    if (ge_rr_pos + 4u > ge_rr_len) { return 0; }

    speedframes = ge_rr_buf[ge_rr_pos + 0];
    count       = ge_rr_buf[ge_rr_pos + 1];
    seedbyte    = ge_rr_buf[ge_rr_pos + 2];

    if (count == 0u && speedframes == 0u) { return 0; }   /* terminator */

    bytes = (unsigned long) ge_rr_cmds * 4ul * (unsigned long) count;
    if (ge_rr_pos + 4ul + bytes > ge_rr_len) { return 0; }

    /* THE SEED COMPARISON, and the reason it is counted rather than announced.
     *
     * The recorded seed is ONE BYTE per block, so two unrelated runs agree by chance about once
     * every 256 blocks. A single matching block therefore proves nothing, and reporting "seed
     * matched" off one would be the same error as calling one sample a rate. What carries
     * information is a RUN of agreement and where it ends. So both totals travel together, and the
     * first disagreement is remembered rather than printed once and lost in the scrollback. */
    ours = gePlayerSeedFingerprint() & 0xFFu;
    ge_rr_checked++;
    if (ours == seedbyte) {
        ge_rr_agree++;
    } else if (ge_rr_first_bad < 0) {
        ge_rr_first_bad = (long) ge_rr_blocks;
        printf("[getv][ramrom] first seed disagreement at block %lu (frame %lu): "
               "recorded %u, ours %u\n", ge_rr_blocks, ge_rr_frames, seedbyte, ours);
        fflush(stdout);
    }

    if (ge_rr_trace) {
        printf("[getv][ramrom] block %lu: speedframes %u, %u records, seed %u vs %u\n",
               ge_rr_blocks, (unsigned int) speedframes, count, seedbyte, ours);
        fflush(stdout);
    }

    ge_rr_blocks++;
    ge_rr_rec    = (unsigned long) ge_rr_cmds * (unsigned long) count;
    ge_rr_recoff = ge_rr_pos + 4ul;
    ge_rr_pos    = ge_rr_pos + 4ul + bytes;
    return 1;
}

/* Called once per rendered frame. Posts for the NEXT tick, for the same reason gePortBotFrame
 * does: the playback handler already ran for this frame, so posting "now" is posting into the
 * past and gePlayerPost would correctly refuse it. Inert unless GETV_RAMROM names a file. */
void gePortRamromFrame(int frame)
{
    GePlayerInput in;
    unsigned int  pad;
    signed char   sx, sy;

    (void) frame;
    if (!ge_rr_ready) { ge_rr_init(); }
    if (!ge_rr_buf || ge_rr_done) { return; }

    if (ge_rr_rec == 0ul && !ge_rr_next_block()) {
        ge_rr_done = 1;
        printf("[getv][ramrom] end of demo: %lu blocks, %lu frames, seed agreed on %lu of %lu "
               "blocks", ge_rr_blocks, ge_rr_frames, ge_rr_agree, ge_rr_checked);
        if (ge_rr_first_bad >= 0) {
            printf(", first disagreement at block %ld", ge_rr_first_bad);
        }
        printf("\n");
        /* One byte agrees by chance about 1 block in 256, so a handful of agreements is not
         * evidence of anything. Said here so the figure cannot be quoted without it. */
        if (ge_rr_checked < 16ul) {
            printf("[getv][ramrom] %lu blocks is too few to mean anything: a one-byte seed "
                   "agrees by chance about 1 block in 256.\n", ge_rr_checked);
        }
        fflush(stdout);
        return;
    }

    sx  = (signed char) ge_rr_buf[ge_rr_recoff + 0];
    sy  = (signed char) ge_rr_buf[ge_rr_recoff + 1];
    pad = ((unsigned int) ge_rr_buf[ge_rr_recoff + 3] << 8) |
           (unsigned int) ge_rr_buf[ge_rr_recoff + 2];

    memset(&in, 0, sizeof in);
    in.stick_x = sx;
    in.stick_y = sy;
    in.buttons = gePlayerButtonsFromPad(pad);

    gePlayerPost(ge_rr_slot, gePlayerTick() + 1, &in, 1);

    ge_rr_recoff += 4ul;
    ge_rr_rec--;
    ge_rr_frames++;
}
