/* The mouse-to-stick arithmetic.
 *
 * This is the surface that has cost the most. Of the first three attempts at the input path,
 * two made the mouse worse and one stopped it moving at all, and each round cost a build, a
 * launch and a hand on the mouse to judge the result. None of what went wrong was arithmetic.
 * The arithmetic is integers in, integers out, and it can be settled here in a millisecond,
 * which leaves the hand free for the part that genuinely needs one.
 *
 * Two jobs:
 *
 *   1. Equivalence. geMouseAccumulate was lifted out of geMousePoll so it could be reached
 *      without SDL. The reference below is the code it replaced, carried across character for
 *      character before the edit was made, and both are run over the same swept domain. A
 *      refactor of live input has to prove it changed nothing, and "it looked fine when I
 *      played it" is not that proof.
 *
 *   2. Properties. What the carry and the deadzone remap are actually for, so the next person
 *      to tidy this can see which lines are doing work.
 */
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include "ge_mouse_accum.h"

static int fails;

static void check(int cond, const char *what)
{
    if (cond) { printf("  ok    %s\n", what); return; }
    printf("  FAIL  %s\n", what);
    fails++;
}

/* ------------------------------------------------------------------ the reference
 *
 * port_input.c lines 1040-1090 as they stood before the extraction, with only the two file
 * statics turned into parameters so independent sequences do not bleed into each other. Nothing
 * else is touched: the literals stay literal, including the ones the header now names, because
 * a reference that has been tidied is no longer a reference.
 *
 * That means the reference carries the overflow the extraction fixed. `(long) dx * 32767 * sens`
 * is fine where long is 64 bits and wrong where it is 32, and Windows is LLP64. So the sweep
 * below bounds its magnitudes by what the REFERENCE can represent on this host, not by what the
 * real code can. Comparing against a reference that has overflowed proves nothing.
 */
static void reference(long dx, long dy, long sens,
                      long *ge_mouse_pend_x, long *ge_mouse_pend_y,
                      long *out_rx, long *out_ry)
{
    long rx, ry;

    rx = ((long) dx * 32767L * sens) / (GE_MOUSE_COUNTS_FULL * 100L);
    ry = ((long) dy * 32767L * sens) / (GE_MOUSE_COUNTS_FULL * 100L);

    *ge_mouse_pend_x += rx;
    *ge_mouse_pend_y += ry;

    if (*ge_mouse_pend_x >  4L * 32767L) *ge_mouse_pend_x =  4L * 32767L;
    if (*ge_mouse_pend_x < -4L * 32767L) *ge_mouse_pend_x = -4L * 32767L;
    if (*ge_mouse_pend_y >  4L * 32767L) *ge_mouse_pend_y =  4L * 32767L;
    if (*ge_mouse_pend_y < -4L * 32767L) *ge_mouse_pend_y = -4L * 32767L;

    rx = *ge_mouse_pend_x;
    ry = *ge_mouse_pend_y;
    if (rx >  32767L) rx =  32767L;
    if (rx < -32767L) rx = -32767L;
    if (ry >  32767L) ry =  32767L;
    if (ry < -32767L) ry = -32767L;

    *ge_mouse_pend_x -= rx;
    *ge_mouse_pend_y -= ry;

    {
        const long dz = 6553L, mx = 32767L;
        if (rx != 0) {
            long m = (rx < 0) ? -rx : rx;
            m = dz + (m * (mx - dz)) / mx;
            rx = (rx < 0) ? -m : m;
        }
        if (ry != 0) {
            long m = (ry < 0) ? -ry : ry;
            m = dz + (m * (mx - dz)) / mx;
            ry = (ry < 0) ? -m : m;
        }
    }

    *out_rx = rx;
    *out_ry = ry;
}

/* A cheap deterministic sequence. Nothing here may depend on the host's rand(), because a test
 * that sweeps a different domain on every run cannot be compared with the run that passed. */
static unsigned long seed = 0x2545F491UL;
static long next_delta(long span)
{
    seed = seed * 6364136223846793005UL + 1442695040888963407UL;
    return (long) ((seed >> 33) % (unsigned long) (2 * span + 1)) - span;
}

/* ------------------------------------------------------------------ 1. equivalence */

static void equivalence(void)
{
    static const long sens_values[] = { 1, 25, 50, 100, 200, 400, 1000 };
    int  si;
    long total = 0, mismatch = 0, skipped = 0;

    /* Every sensitivity the config allows, against deltas from a single count up to well past
     * anything a hand produces, run as SEQUENCES rather than one-shots: the carry is the part
     * most likely to be got wrong, and it only shows up across successive calls. */
    for (si = 0; si < (int) (sizeof sens_values / sizeof sens_values[0]); si++) {
        long sens = sens_values[si];
        int  span_i;
        static const long spans[] = { 1, 3, 21, 100, 2000, 100000 };

        for (span_i = 0; span_i < (int) (sizeof spans / sizeof spans[0]); span_i++) {
            long ax = 0, ay = 0, bx = 0, by = 0;   /* two independent carries */
            int  step;

            /* Skip a span the reference cannot compute on this host. On LP64 nothing is
             * skipped and the whole domain is swept; on LLP64 the largest spans drop out and
             * the printed call count says so rather than quietly shrinking. */
            if (spans[span_i] > (long) (LONG_MAX / (32767L * sens))) {
                skipped++;
                continue;
            }

            seed = 0x2545F491UL + (unsigned long) (si * 977 + span_i);

            for (step = 0; step < 400; step++) {
                long dx = next_delta(spans[span_i]);
                long dy = next_delta(spans[span_i]);
                long r_rx = 0, r_ry = 0, g_rx = 0, g_ry = 0;

                reference(dx, dy, sens, &ax, &ay, &r_rx, &r_ry);
                geMouseAccumulate(dx, dy, sens, &bx, &by, &g_rx, &g_ry);

                total++;
                if (r_rx != g_rx || r_ry != g_ry || ax != bx || ay != by) {
                    if (mismatch < 3) {
                        printf("  FAIL  sens=%ld dx=%ld dy=%ld step=%d: "
                               "ref=(%ld,%ld) carry(%ld,%ld) vs got=(%ld,%ld) carry(%ld,%ld)\n",
                               sens, dx, dy, step, r_rx, r_ry, ax, ay, g_rx, g_ry, bx, by);
                    }
                    mismatch++;
                }
            }
        }
    }

    printf("  swept %ld calls across 7 sensitivities and 6 magnitude ranges", total);
    if (skipped > 0) {
        printf(" (%ld range(s) skipped: a 32-bit long cannot hold the reference's product)",
               skipped);
    }
    printf("\n");
    check(mismatch == 0, "extraction is bit-identical to the code it replaced");

    /* The extremes on their own, since a random sweep can miss a boundary by luck. */
    {
        static const long edge[] = { 0, 1, -1, 20, 21, 22, -21, 32767, -32767, 65535, -65535,
                                     1000000, -1000000 };
        int  i, j;
        long bad = 0, edge_skipped = 0;
        /* Same limit the sweep above applies, for the same reason, and it has to be applied
         * here too: reference() forms dx * 32767 * sens, and this block feeds it 1000000 at
         * sens 100 -- about 3.3e12, which a 32-bit long cannot hold. On LP64 nothing is
         * skipped and every pair runs. On LLP64 the reference overflowed instead, so it was
         * the REFERENCE that was wrong and geMouseAccumulate was reported as the mismatch:
         * one failure on Windows, while the 11600-call sweep beside it passed bit-identical
         * because it does guard the same product. */
        const long edge_max = LONG_MAX / (32767L * 100L);

        for (i = 0; i < (int) (sizeof edge / sizeof edge[0]); i++) {
            for (j = 0; j < (int) (sizeof edge / sizeof edge[0]); j++) {
                long ax = 0, ay = 0, bx = 0, by = 0;
                long r_rx = 0, r_ry = 0, g_rx = 0, g_ry = 0;
                long ei = edge[i] < 0 ? -edge[i] : edge[i];
                long ej = edge[j] < 0 ? -edge[j] : edge[j];
                int  rep;

                if (ei > edge_max || ej > edge_max) { edge_skipped++; continue; }

                /* Three calls, so a difference in what the carry retains is visible rather
                 * than only a difference in what is emitted. */
                for (rep = 0; rep < 3; rep++) {
                    reference(edge[i], edge[j], 100, &ax, &ay, &r_rx, &r_ry);
                    geMouseAccumulate(edge[i], edge[j], 100, &bx, &by, &g_rx, &g_ry);
                    if (r_rx != g_rx || r_ry != g_ry || ax != bx || ay != by) { bad++; }
                }
            }
        }
        /* Said out loud rather than quietly shrinking the domain, exactly as the sweep does. */
        if (edge_skipped > 0) {
            printf("  %ld boundary pair(s) skipped: a 32-bit long cannot hold the reference's"
                   " product\n", edge_skipped);
        }
        check(bad == 0, "identical on every boundary pair, carried over three calls");
    }
}

/* ------------------------------------------------------------------ 2. properties */

static void properties(void)
{
    /* A flick is not thrown away. This is the defect the carry exists for: the stick is a
     * velocity control and a mouse reports a displacement, so a single large movement used to
     * be clamped to one frame of deflection and the remainder discarded. Sweeping further did
     * nothing, which was the reported symptom.
     *
     * Read this one carefully, because it does NOT describe what a player feels. geMousePoll
     * only calls in when dx or dy is nonzero, so on a frame with the hand still, nothing here
     * runs and the carry is held rather than drained. The remainder is delivered on the next
     * frame that does move. That is why the view stops dead when the hand does instead of
     * gliding, and it is worth knowing before anyone "fixes" the guard to match the wording. */
    {
        long px = 0, py = 0, rx = 0, ry = 0;
        int  i, frames_emitting = 0;

        geMouseAccumulate(2000, 0, 100, &px, &py, &rx, &ry);   /* one big flick */
        if (rx > 0) { frames_emitting++; }
        for (i = 0; i < 16; i++) {
            geMouseAccumulate(0, 0, 100, &px, &py, &rx, &ry);  /* hand now still */
            if (rx > 0) { frames_emitting++; }
        }
        check(frames_emitting > 1, "a flick keeps emitting across further calls");
        check(px == 0, "and the carry drains to empty rather than holding motion forever");
    }

    /* But it drains. An unbounded carry is a view that glides on after the hand has stopped,
     * which is what the cap is for: four frames of full deflection, no more. */
    {
        long px = 0, py = 0, rx = 0, ry = 0;
        int  i, frames = 0;

        for (i = 0; i < 50; i++) {                              /* a long hard sweep */
            geMouseAccumulate(100000, 0, 1000, &px, &py, &rx, &ry);
        }
        check(px <= GE_MOUSE_BACKLOG_FRAMES * GE_MOUSE_FULL,
              "the carry never exceeds its cap however hard the sweep");

        for (i = 0; i < 64; i++) {                              /* hand stops dead */
            geMouseAccumulate(0, 0, 1000, &px, &py, &rx, &ry);
            if (rx != 0) { frames++; }
        }
        check(frames <= 4, "and the glide after a stop lasts at most four frames");
        check(px == 0, "carry empty once it has drained");
    }

    /* The deadzone remap. port_os.c drops anything under 6553, so before this a small real
     * movement produced a number that was quietly discarded further down: 12 counts landed at
     * 6241 against the 6553 threshold and turned the view 0.0002 degrees. Any nonzero movement
     * must now clear the threshold. */
    {
        long dx;
        int  below = 0;

        for (dx = 1; dx <= 500; dx++) {
            long px = 0, py = 0, rx = 0, ry = 0;
            geMouseAccumulate(dx, 0, 100, &px, &py, &rx, &ry);
            if (rx != 0 && rx < GE_MOUSE_DEADZONE) { below++; }
        }
        check(below == 0, "no movement lands under the deadzone port_os.c enforces");
    }

    /* Full scale has to survive the remap, or the fastest flick would turn slower than before
     * the deadzone was dealt with. */
    {
        long px = 0, py = 0, rx = 0, ry = 0;
        geMouseAccumulate(100000, 0, 1000, &px, &py, &rx, &ry);
        check(rx == GE_MOUSE_FULL, "full deflection stays full deflection");
    }

    /* Symmetry. Turning left and right at the same speed must cost the same, and a sign bug
     * here reads as a mouse that pulls to one side. */
    {
        long dx;
        int  asym = 0;

        for (dx = 1; dx <= 300; dx++) {
            long ap = 0, aq = 0, arx = 0, ary = 0;
            long bp = 0, bq = 0, brx = 0, bry = 0;
            geMouseAccumulate( dx, 0, 100, &ap, &aq, &arx, &ary);
            geMouseAccumulate(-dx, 0, 100, &bp, &bq, &brx, &bry);
            if (arx != -brx) { asym++; }
        }
        check(asym == 0, "left and right are mirror images");
    }

    /* Anchored to a live run, so this file is tied to the real binary and not only to itself.
     *
     * getv/build-mac/goldeneye on DAM, GETV_MOUSE_SELFTEST=30 GETV_INPUT_DEBUG=2, printed:
     *
     *   [getv][mouse] n=1   dx=30 dy=0 sens=100 -> rx=32767 ry=0 carry=(14043,0)
     *   [getv][mouse] n=61  dx=30 dy=0 sens=100 -> rx=32767 ry=0 carry=(98301,0)
     *   [getv][mouse] n=121 dx=30 dy=0 sens=100 -> rx=32767 ry=0 carry=(98301,0)
     *
     * 30 counts at 100% is 46810 stick units, which is past full scale, so every frame emits
     * the maximum and banks the remaining 14043. The carry climbs until it meets the cap and
     * then sits there. If these three numbers ever stop matching what the game prints, either
     * the arithmetic moved or the caller stopped feeding it what it used to. */
    {
        long px = 0, py = 0, rx = 0, ry = 0;
        int  i;

        geMouseAccumulate(30, 0, 100, &px, &py, &rx, &ry);
        check(rx == 32767 && px == 14043, "first frame at 30 counts matches the live log");

        for (i = 1; i < 61; i++) { geMouseAccumulate(30, 0, 100, &px, &py, &rx, &ry); }
        check(rx == 32767 && px == 98301, "and frame 61 matches it too");

        for (i = 61; i < 121; i++) { geMouseAccumulate(30, 0, 100, &px, &py, &rx, &ry); }
        check(rx == 32767 && px == 98301, "saturated carry stays put rather than creeping");
    }

    /* The overflow the extraction fixed, at the smallest input that actually shows it.
     *
     * `(long) dx * 32767 * sens` is evaluated in long, and Windows is LLP64, so on that host
     * it is a 32-bit multiply. 656 counts at the DEFAULT 100% is 656 * 32767 * 100 =
     * 2,149,515,200 against a LONG_MAX of 2,147,483,647. It exceeds it by 0.1%, wraps
     * negative, and the emitted value comes out -32767 where it should be +32767.
     *
     * That is a full-scale turn in the WRONG DIRECTION on a fast flick, at stock settings, on
     * a mouse that reports 656 counts in a frame, which a high-DPI one does easily. Not an
     * edge case worth a shrug.
     *
     * Picking the threshold mattered. The first version of this used 5,000 counts at 1000%,
     * which overflows far harder, and it was a useless test: the wrapped value was 298,210,
     * still past the 131,068 cap, so it saturated to the same answer and the check passed
     * either way. An overflow test has to be aimed at an input where the wrap changes the
     * result, not merely at one where it happens.
     *
     * 2,149,515,200 / 2,100 = 1,023,578, well past the cap, so the carry saturates, one full
     * frame is emitted and 98,301 is held. */
    {
        long px = 0, py = 0, rx = 0, ry = 0;
        geMouseAccumulate(656, 0, 100, &px, &py, &rx, &ry);
        check(rx == GE_MOUSE_FULL, "a 656-count flick at 100% turns the right way");
        check(px == 98301,         "and banks the right carry rather than a wrapped one");
    }

    /* Zero in, zero out. Sounds trivial, and it is exactly what breaks when a deadzone floor is
     * applied before the nonzero test rather than after: the view creeps with no hand on the
     * mouse at all. */
    {
        long px = 0, py = 0, rx = 1, ry = 1;
        geMouseAccumulate(0, 0, 100, &px, &py, &rx, &ry);
        check(rx == 0 && ry == 0, "a still mouse produces no deflection");
    }

    /* The axes do not talk to each other. They share a function and a call, and that is the
     * only reason a copy-paste slip between them would ever be possible. */
    {
        long px = 0, py = 0, rx = 0, ry = 0;
        geMouseAccumulate(500, 0, 100, &px, &py, &rx, &ry);
        check(ry == 0 && py == 0, "moving in x leaves y alone");
        px = py = 0;
        geMouseAccumulate(0, 500, 100, &px, &py, &rx, &ry);
        check(rx == 0 && px == 0, "and moving in y leaves x alone");
    }
}

int main(void)
{
    printf("mouse-to-stick arithmetic\n");
    equivalence();
    properties();
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "all checks passed", fails);
    return fails != 0;
}
