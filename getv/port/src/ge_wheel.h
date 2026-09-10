/* Mouse wheel notches -> weapon-cycle presses.
 *
 * Lifted out of port_input.c for the same reason ge_mouse_accum.h was: so it can be
 * tested without SDL, a window, or a hand on a mouse. It is integers in, integers out,
 * and the part that is easy to get wrong is a timing rule rather than arithmetic --
 * exactly the kind of thing that is invisible when you play it and obvious in a test.
 *
 * The rule, and why it exists:
 *
 * The engine cycles weapons on a RISING EDGE of the inventory button --
 * `(buttons & ~oldbuttons) & invButtons` in bondview2.c. A wheel flick delivers several
 * SDL_MOUSEWHEEL events inside one frame. Asserting the button for all of them produces
 * ONE edge and advances ONE weapon, so a three-notch flick would move one slot and feel
 * broken.
 *
 * So notches are queued and released one per frame, with a gap frame after each so the
 * button is seen to go up again. Three notches become three edges and three weapons.
 *
 * The queue is capped because the failure at the other end is just as bad: a
 * free-spinning wheel can deliver hundreds of notches, and draining them all would leave
 * the weapon cycling for several seconds after the player stopped touching it. Input
 * that outlives the gesture reads as a bug even though every notch was honoured.
 */
#ifndef GE_WHEEL_H
#define GE_WHEEL_H

/* About a second of cycling at 60fps with a gap frame between notches. */
#define GE_WHEEL_MAX_PENDING 8

struct GeWheel {
    int pend_up;
    int pend_dn;
    int up_now;    /* asserted for exactly the frame geWheelTick set it */
    int dn_now;
    int gap;       /* frames still owed before the next notch may fire */
};

static void geWheelClear(struct GeWheel *w)
{
    w->pend_up = 0;
    w->pend_dn = 0;
    w->up_now  = 0;
    w->dn_now  = 0;
    w->gap     = 0;
}

/* One SDL_MOUSEWHEEL, with `y` already un-flipped by the event owner so positive is
 * always "away from the user".
 *
 * A reversal clears the opposite queue rather than adding to the total. Scrolling up
 * then immediately down means the player changed their mind; draining the up notches
 * first would spend the next several frames going the way they just abandoned. */
static void geWheelAdd(struct GeWheel *w, int y)
{
    if (y > 0) {
        w->pend_dn = 0;
        w->pend_up += y;
        if (w->pend_up > GE_WHEEL_MAX_PENDING) { w->pend_up = GE_WHEEL_MAX_PENDING; }
    } else if (y < 0) {
        w->pend_up = 0;
        w->pend_dn += -y;
        if (w->pend_dn > GE_WHEEL_MAX_PENDING) { w->pend_dn = GE_WHEEL_MAX_PENDING; }
    }
}

/* Once per FRAME, before the pads are read. `allowed` is false whenever the game should
 * not be reading input -- the console owns the devices, the pointer is released, the run
 * is a measurement run.
 *
 * A disallowed frame drops the backlog rather than merely emitting nothing. Notches
 * collected while the console was open must not all fire the instant it closes. */
static void geWheelTickState(struct GeWheel *w, int allowed)
{
    w->up_now = 0;
    w->dn_now = 0;

    if (!allowed) { geWheelClear(w); return; }

    if (w->gap > 0) { w->gap--; return; }

    if (w->pend_up > 0)      { w->pend_up--; w->up_now = 1; w->gap = 1; }
    else if (w->pend_dn > 0) { w->pend_dn--; w->dn_now = 1; w->gap = 1; }
}

#endif /* GE_WHEEL_H */
