/* Mouse wheel notches to weapon-cycle presses -- ge_wheel.h.
 *
 * The wheel is the default weapon-change binding, and the thing that makes it work is a
 * timing rule rather than any arithmetic: the engine cycles weapons on a RISING EDGE of
 * the inventory button, so a flick that delivers four notches inside one frame has to be
 * spread across frames or it advances one weapon instead of four.
 *
 * That is precisely the class of bug that is invisible when you play it -- the weapon
 * does change, just not as far as you asked -- and obvious in a test. Which is why the
 * state machine was lifted into a header, the same move ge_mouse_accum.h documents for
 * the same reason.
 */
#include <stdio.h>
#include <string.h>

#include "ge_wheel.h"

static int checks;
static int failures;

static void eq(int got, int want, const char *what)
{
    checks++;
    if (got == want) { printf("ok   %s (%d)\n", what, got); return; }
    failures++;
    printf("FAIL %s: expected %d, got %d\n", what, want, got);
}

/* Run `frames` frames and count how many times each direction fired. This is the shape
 * that matters: not what happens on one frame, but how many EDGES a gesture produces. */
static void drain(struct GeWheel *w, int frames, int *ups, int *dns)
{
    int i;
    *ups = *dns = 0;
    for (i = 0; i < frames; i++) {
        geWheelTickState(w, 1);
        if (w->up_now) (*ups)++;
        if (w->dn_now) (*dns)++;
    }
}

static void test_one_notch(void)
{
    struct GeWheel w;
    int ups, dns;

    printf("# one notch is one press\n");
    geWheelClear(&w);
    geWheelAdd(&w, 1);

    geWheelTickState(&w, 1);
    eq(w.up_now, 1, "fires on the first frame");
    eq(w.dn_now, 0, "and not downwards");

    /* The gap is the whole point: the button has to be seen to go up again, or the next
     * notch raises no edge. */
    geWheelTickState(&w, 1);
    eq(w.up_now, 0, "released on the next frame");

    drain(&w, 10, &ups, &dns);
    eq(ups, 0, "nothing left over");
    eq(dns, 0, "and nothing in the other direction");
}

static void test_flick_produces_one_edge_each(void)
{
    struct GeWheel w;
    int ups, dns;

    printf("# a fast flick advances once per notch, not once in total\n");
    /* Four notches inside a single frame, which is what a real flick delivers -- SDL
     * queues them all before the next poll. Asserting the button for all four would
     * produce ONE rising edge and move ONE weapon. */
    geWheelClear(&w);
    geWheelAdd(&w, 1);
    geWheelAdd(&w, 1);
    geWheelAdd(&w, 1);
    geWheelAdd(&w, 1);
    eq(w.pend_up, 4, "all four queued");

    drain(&w, 20, &ups, &dns);
    eq(ups, 4, "four separate presses");
    eq(dns, 0, "none downwards");

    /* SDL can also deliver a single event with y > 1. It must mean the same thing. */
    geWheelClear(&w);
    geWheelAdd(&w, 3);
    drain(&w, 20, &ups, &dns);
    eq(ups, 3, "a multi-notch event is three presses too");
}

static void test_never_two_frames_running(void)
{
    struct GeWheel w;
    int i, consecutive = 0, prev = 0;

    printf("# no two presses are ever adjacent\n");
    geWheelClear(&w);
    geWheelAdd(&w, GE_WHEEL_MAX_PENDING);

    for (i = 0; i < 40; i++) {
        geWheelTickState(&w, 1);
        if (w.up_now && prev) { consecutive++; }
        prev = w.up_now;
    }
    /* Two adjacent asserted frames are one long press, which is one edge. If this ever
     * fires, the gap has been removed and the cap-sized flick silently became a single
     * weapon change. */
    eq(consecutive, 0, "always a released frame between presses");
}

static void test_reversal(void)
{
    struct GeWheel w;
    int ups, dns;

    printf("# reversing direction abandons the queued notches\n");
    geWheelClear(&w);
    geWheelAdd(&w, 4);
    geWheelAdd(&w, -1);

    eq(w.pend_up, 0, "the up queue is dropped");
    eq(w.pend_dn, 1, "and one down notch is queued");

    drain(&w, 20, &ups, &dns);
    /* Scrolling up then straight back down means the player changed their mind.
     * Draining the up notches first would spend the next several frames going the way
     * they just abandoned. */
    eq(ups, 0, "nothing goes the abandoned way");
    eq(dns, 1, "only the new direction fires");
}

static void test_cap(void)
{
    struct GeWheel w;
    int ups, dns;

    printf("# the backlog is capped\n");
    geWheelClear(&w);
    /* A free-spinning wheel. Honouring all of these would leave the weapon cycling for
     * seconds after the player stopped touching it, which reads as a bug even though
     * every notch was real. */
    for (int i = 0; i < 500; i++) { geWheelAdd(&w, 1); }
    eq(w.pend_up, GE_WHEEL_MAX_PENDING, "queue is capped");

    geWheelClear(&w);
    geWheelAdd(&w, 500);
    eq(w.pend_up, GE_WHEEL_MAX_PENDING, "a single huge event is capped too");

    drain(&w, 200, &ups, &dns);
    eq(ups, GE_WHEEL_MAX_PENDING, "and drains completely");
}

static void test_disallowed_drops_the_backlog(void)
{
    struct GeWheel w;
    int ups, dns;

    printf("# a disallowed frame discards, it does not merely defer\n");
    geWheelClear(&w);
    geWheelAdd(&w, 5);

    geWheelTickState(&w, 0);
    eq(w.up_now, 0, "nothing fires");
    /* Scrolling the developer console must not cycle the player's weapon five times the
     * instant the console closes. */
    eq(w.pend_up, 0, "and the backlog is gone, not held");

    drain(&w, 20, &ups, &dns);
    eq(ups, 0, "nothing arrives late");

    /* Notches taken while disallowed are queued but cleared by the next disallowed
     * tick, so a stream of them cannot accumulate either. */
    geWheelAdd(&w, 3);
    geWheelTickState(&w, 0);
    eq(w.pend_up, 0, "collected-while-blocked is discarded too");
}

static void test_zero_and_clear(void)
{
    struct GeWheel w;

    printf("# edges\n");
    geWheelClear(&w);
    geWheelAdd(&w, 0);
    eq(w.pend_up, 0, "a zero delta queues nothing up");
    eq(w.pend_dn, 0, "and nothing down");

    geWheelAdd(&w, -2);
    geWheelTickState(&w, 1);
    eq(w.dn_now, 1, "negative is downwards");

    geWheelClear(&w);
    eq(w.pend_dn, 0, "clear empties the queue");
    eq(w.dn_now, 0, "and the frame flags");
    eq(w.gap, 0, "and the gap");
}

int main(void)
{
    test_one_notch();
    test_flick_produces_one_edge_each();
    test_never_two_frames_running();
    test_reversal();
    test_cap();
    test_disallowed_drops_the_backlog();
    test_zero_and_clear();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
