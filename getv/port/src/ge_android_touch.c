/* On-screen touch controls for Android. See ge_android_touch.h for why this drives a virtual
 * pad rather than reaching into the input path.
 *
 * Everything here is inside #ifdef __ANDROID__. On every other platform the three entry
 * points are empty, so callers need no guard of their own.
 */
#include "ge_android_touch.h"

#ifndef __ANDROID__

void gePortAndroidTouchInit(void)     { }
void gePortAndroidTouchUpdate(void)   { }
void gePortAndroidTouchShutdown(void) { }

#else

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL.h>

/* Zones are in SDL's normalised touch coordinates (0..1 across the window, y downward), so
 * they hold on any screen size and any density without asking the window how big it is.
 * The game is landscape-locked, so x is the long axis.
 *
 * Left edge is movement, right edge is look, and the buttons sit over the look zone rather
 * than beside it -- a finger inside a button zone is claimed by the button and never also
 * turns the view. */
#define TOUCH_MOVE_ZONE_X     0.40f   /* x below this is the movement stick */

/* The movement stick FLOATS: it centres wherever the thumb lands rather than at a fixed
 * spot. A fixed stick has to be found by feel and lands differently on a phone than on a
 * tablet; a floating one is under the thumb by construction. */
#define TOUCH_MOVE_RADIUS     0.11f   /* full deflection this far from where the thumb landed */
#define TOUCH_MOVE_DEADZONE   0.012f  /* ignores the wobble of a thumb trying to hold still */

/* Look is rate-based: the axis reflects how far the finger moved THIS frame, so holding the
 * finger still stops the view even though the finger is still down. That is the behaviour a
 * mouse-look player expects and it is what mobile shooters do.
 *
 * It does mean turn speed is per frame rather than per second, so a slower frame turns less
 * far for the same swipe. Matching the freecam's reasoning, that is the right trade here:
 * the alternative scales input by a delta the renderer does not hold steady. */
#define TOUCH_LOOK_GAIN      14.0f

/* Button zones, all x/y in the same normalised space. Fire and aim are the two the game is
 * played with, so they get the corner and the edge above it -- the two places a right thumb
 * reaches without leaving the look area. */
#define TOUCH_BTN_X          0.86f    /* everything right of this is a button column */
#define TOUCH_FIRE_Y         0.62f    /* below -> fire */
#define TOUCH_AIM_Y_TOP      0.28f    /* between AIM_Y_TOP and FIRE_Y -> aim */
#define TOUCH_USE_X_LO       0.71f    /* a second, inner column for use/reload */
#define TOUCH_USE_Y          0.74f
#define TOUCH_START_X_LO     0.45f    /* a small tab at the top centre for start/pause */
#define TOUCH_START_X_HI     0.55f
#define TOUCH_START_Y        0.09f

#define TOUCH_AXIS_MAX       32767

static int           s_enabled = -1;
static SDL_Joystick *s_js;
static int           s_attached_index = -1;

/* Which finger owns the movement stick and where it first touched down. SDL_FingerID is
 * stable for the life of a touch, which is what makes it usable as an owner token: a second
 * finger arriving in the same zone does not steal the stick. */
static SDL_FingerID  s_move_id;
static int           s_move_active;
static float         s_move_ox, s_move_oy;

static SDL_FingerID  s_look_id;
static int           s_look_active;
static float         s_look_lx, s_look_ly;

static int ge_touch_enabled(void)
{
    if (s_enabled < 0) {
        const char *e = getenv("GETV_TOUCH");
        /* Default on. Off is for a device with a real pad paired to it, where a silent
         * second controller would otherwise be classed 'real' and take player 1. */
        s_enabled = (e == NULL || *e == '\0' || *e != '0');
    }
    return s_enabled;
}

void gePortAndroidTouchInit(void)
{
    SDL_VirtualJoystickDesc desc;
    int idx;

    if (!ge_touch_enabled() || s_js != NULL) {
        return;
    }

    if (SDL_WasInit(SDL_INIT_JOYSTICK) == 0 && SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
        printf("[getv][touch] joystick subsystem would not start: %s\n", SDL_GetError());
        fflush(stdout);
        return;
    }

    SDL_zero(desc);
    desc.version  = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type     = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    /* The full standard layout rather than only the controls used below. port_input.c's
     * geControllerIsReal() asks for LEFTX, LEFTY, RIGHTX, A, B and LEFTSHOULDER before it
     * will treat a pad as playable, and a pad that fails that test is accepted only as a
     * menus-only fallback. Declaring the whole layout passes it and costs nothing: an axis
     * that is never written simply reads 0. */
    desc.naxes    = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.nhats    = 0;
    desc.name     = "GoldenEye Touch Controls";

    idx = SDL_JoystickAttachVirtualEx(&desc);
    if (idx < 0) {
        printf("[getv][touch] could not attach the virtual pad: %s\n", SDL_GetError());
        fflush(stdout);
        return;
    }

    s_js = SDL_JoystickOpen(idx);
    if (s_js == NULL) {
        printf("[getv][touch] attached the virtual pad but could not open it: %s\n", SDL_GetError());
        fflush(stdout);
        SDL_JoystickDetachVirtual(idx);
        return;
    }
    s_attached_index = idx;

    printf("[getv][touch] on-screen controls ready (left thumb moves, right thumb looks, "
           "right edge fires and aims). GETV_TOUCH=0 disables.\n");
    fflush(stdout);
}

static float ge_touch_clamp1(float v)
{
    if (v >  1.0f) { return  1.0f; }
    if (v < -1.0f) { return -1.0f; }
    return v;
}

static void ge_touch_axis(SDL_GameControllerAxis ax, float unit)
{
    SDL_JoystickSetVirtualAxis(s_js, (int) ax, (Sint16)(ge_touch_clamp1(unit) * TOUCH_AXIS_MAX));
}

void gePortAndroidTouchUpdate(void)
{
    float mx = 0.0f, my = 0.0f;      /* movement stick deflection, -1..1 */
    float lx = 0.0f, ly = 0.0f;      /* look rate this frame, -1..1 */
    int fire = 0, aim = 0, use = 0, start = 0;
    int move_seen = 0, look_seen = 0;
    int nd, d;

    if (s_js == NULL) {
        return;
    }

    nd = SDL_GetNumTouchDevices();
    for (d = 0; d < nd; d++) {
        SDL_TouchID tid = SDL_GetTouchDevice(d);
        int nf = SDL_GetNumTouchFingers(tid);
        int f;

        for (f = 0; f < nf; f++) {
            SDL_Finger *fin = SDL_GetTouchFinger(tid, f);
            float x, y;

            if (fin == NULL) {
                continue;
            }
            x = fin->x;
            y = fin->y;

            /* Buttons are tested first so a finger resting on one is never also read as a
             * look drag underneath it. */
            if (x >= TOUCH_BTN_X && y >= TOUCH_FIRE_Y) {
                fire = 1;
                continue;
            }
            if (x >= TOUCH_BTN_X && y >= TOUCH_AIM_Y_TOP && y < TOUCH_FIRE_Y) {
                aim = 1;
                continue;
            }
            if (x >= TOUCH_USE_X_LO && x < TOUCH_BTN_X && y >= TOUCH_USE_Y) {
                use = 1;
                continue;
            }
            if (x >= TOUCH_START_X_LO && x <= TOUCH_START_X_HI && y <= TOUCH_START_Y) {
                start = 1;
                continue;
            }

            if (x < TOUCH_MOVE_ZONE_X) {
                if (!s_move_active) {
                    /* A stick that is not currently owned is claimed here, at the point the
                     * thumb lands, which is what makes it float. */
                    s_move_active = 1;
                    s_move_id     = fin->id;
                    s_move_ox     = x;
                    s_move_oy     = y;
                } else if (fin->id != s_move_id) {
                    continue;   /* another finger already owns the stick */
                }
                move_seen = 1;
                {
                    float dx  = x - s_move_ox;
                    float dy  = y - s_move_oy;
                    float mag = sqrtf(dx * dx + dy * dy);

                    if (mag < TOUCH_MOVE_DEADZONE) {
                        mx = 0.0f;
                        my = 0.0f;
                    } else {
                        mx = ge_touch_clamp1(dx / TOUCH_MOVE_RADIUS);
                        /* y grows downward on screen and LEFTY is negative for forward, so
                         * dragging the thumb up walks forward with no extra negation. */
                        my = ge_touch_clamp1(dy / TOUCH_MOVE_RADIUS);
                    }
                }
                continue;
            }

            /* Anything left over is the look area. */
            if (!s_look_active) {
                s_look_active = 1;
                s_look_id     = fin->id;
                s_look_lx     = x;
                s_look_ly     = y;
                look_seen     = 1;
                continue;       /* the first frame of a drag has no delta yet */
            }
            if (fin->id != s_look_id) {
                continue;
            }
            look_seen = 1;
            lx = ge_touch_clamp1((x - s_look_lx) * TOUCH_LOOK_GAIN);
            ly = ge_touch_clamp1((y - s_look_ly) * TOUCH_LOOK_GAIN);
            s_look_lx = x;
            s_look_ly = y;
        }
    }

    /* A finger that lifted stops owning its control. Without this the stick would stay
     * deflected at whatever it last read and the player would keep walking. */
    if (!move_seen) { s_move_active = 0; mx = 0.0f; my = 0.0f; }
    if (!look_seen) { s_look_active = 0; lx = 0.0f; ly = 0.0f; }

    ge_touch_axis(SDL_CONTROLLER_AXIS_LEFTX,  mx);
    ge_touch_axis(SDL_CONTROLLER_AXIS_LEFTY,  my);
    ge_touch_axis(SDL_CONTROLLER_AXIS_RIGHTX, lx);
    ge_touch_axis(SDL_CONTROLLER_AXIS_RIGHTY, ly);

    /* Fire and aim go to the triggers rather than to face buttons: port_input.c derives
     * ltrigger/rtrigger from the raw trigger axes, and the aim self-test already drives aim
     * through ltrigger, so this reaches the game by the same route that does. */
    ge_touch_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, fire ? 1.0f : 0.0f);
    ge_touch_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT,  aim  ? 1.0f : 0.0f);

    SDL_JoystickSetVirtualButton(s_js, SDL_CONTROLLER_BUTTON_A,     (Uint8) use);
    SDL_JoystickSetVirtualButton(s_js, SDL_CONTROLLER_BUTTON_START, (Uint8) start);
}

void gePortAndroidTouchShutdown(void)
{
    if (s_js != NULL) {
        SDL_JoystickClose(s_js);
        s_js = NULL;
    }
    if (s_attached_index >= 0) {
        SDL_JoystickDetachVirtual(s_attached_index);
        s_attached_index = -1;
    }
}

#endif /* __ANDROID__ */
