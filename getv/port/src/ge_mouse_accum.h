/* The mouse-to-stick arithmetic, on its own.
 *
 * Split out of port_input.c so it can be tested. That file is 1,497 lines and reaches for SDL
 * on the way in, so nothing inside it runs without a window, a device and a pointer taken from
 * whoever is using the machine. This part needs none of that: integers in, integers out, with
 * the carry held in two longs the caller owns. Header-only because a unit this small is not
 * worth a fourth build script edit.
 *
 * Scale, then carry, then deadzone. Each is commented where it happens.
 *
 * One thing to know before changing the carry, because the obvious reading is wrong: it drains
 * over later CALLS, not later frames. geMousePoll only calls in when the mouse has actually
 * moved, so a remainder waits for the next movement rather than draining while the hand is
 * still. That is what makes the view stop dead instead of gliding, and it is deliberate.
 */
#ifndef GE_MOUSE_ACCUM_H
#define GE_MOUSE_ACCUM_H

/* Counts of mouse travel per full-scale deflection at GETV_MOUSE_SENS=100, from the sweep in
 * docs/MOUSE.md. Full scale, and the deadzone port_os.c enforces at 20% of the axis. */
#define GE_MOUSE_COUNTS_FULL 21L
#define GE_MOUSE_FULL     32767L
#define GE_MOUSE_DEADZONE  6553L

/* How much travel the carry may hold, in frames of full deflection. A hitch that batches many
 * frames of motion, or the burst that arrives on alt-tab, must not leave the view gliding after
 * the hand has stopped. Four smooths a fast flick without the view feeling detached. */
#define GE_MOUSE_BACKLOG_FRAMES 4L

/* dx and dy are raw mouse counts since the last call, sens is percent in 1..1000. pend_x and
 * pend_y are the caller's carry, updated in place. out_rx and out_ry receive stick units.
 *
 * The carry is a parameter rather than a static in here so tests can run independent sequences.
 * Shared hidden state between cases is how a test starts passing for the wrong reason.
 */
static void geMouseAccumulate(long dx, long dy, long sens,
                              long *pend_x, long *pend_y,
                              long *out_rx, long *out_ry)
{
    /* Scaling and accumulation are done in long long, and that is a fix rather than a
     * flourish. This started as `(long) dx * 32767L * sens`, which is fine where long is 64
     * bits and overflows where it is 32: a 5,000-count flick at 1000% is 1.6e11 against a
     * LONG_MAX of 2.1e9. Windows is LLP64, and build_windows.ps1 compiles port/src, so that
     * is a live target and not a hypothetical one. The result is bounded by the cap below and
     * always fits back in a long, so only the intermediates needed widening.
     *
     * On a 64-bit long this is identical to what it replaced, which is what tests/test_mouse.c
     * checks against the original text over 16,800 swept calls. */
    const long long cap = (long long) GE_MOUSE_BACKLOG_FRAMES * GE_MOUSE_FULL;
    long long sx = ((long long) dx * GE_MOUSE_FULL * sens) / (GE_MOUSE_COUNTS_FULL * 100LL);
    long long sy = ((long long) dy * GE_MOUSE_FULL * sens) / (GE_MOUSE_COUNTS_FULL * 100LL);
    long long ax = (long long) *pend_x + sx;
    long long ay = (long long) *pend_y + sy;
    long rx, ry;

    if (ax >  cap) ax =  cap;
    if (ax < -cap) ax = -cap;
    if (ay >  cap) ay =  cap;
    if (ay < -cap) ay = -cap;

    /* Emit at most one frame of deflection and keep the rest. */
    rx = (long) ((ax >  GE_MOUSE_FULL) ?  GE_MOUSE_FULL :
                 (ax < -GE_MOUSE_FULL) ? -GE_MOUSE_FULL : ax);
    ry = (long) ((ay >  GE_MOUSE_FULL) ?  GE_MOUSE_FULL :
                 (ay < -GE_MOUSE_FULL) ? -GE_MOUSE_FULL : ay);

    *pend_x = (long) (ax - rx);
    *pend_y = (long) (ay - ry);

    if (rx != 0) {
        long m = (rx < 0) ? -rx : rx;
        m = GE_MOUSE_DEADZONE + (m * (GE_MOUSE_FULL - GE_MOUSE_DEADZONE)) / GE_MOUSE_FULL;
        rx = (rx < 0) ? -m : m;
    }
    if (ry != 0) {
        long m = (ry < 0) ? -ry : ry;
        m = GE_MOUSE_DEADZONE + (m * (GE_MOUSE_FULL - GE_MOUSE_DEADZONE)) / GE_MOUSE_FULL;
        ry = (ry < 0) ? -m : m;
    }

    *out_rx = rx;
    *out_ry = ry;
}

#endif /* GE_MOUSE_ACCUM_H */
