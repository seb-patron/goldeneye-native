/* Free camera: the state and the input, none of the rendering.
 *
 * The game already has the camera. debug_camera.c carries a complete six-degree fly camera --
 * debugFreeCamera() -- and lv.c:1740 already calls it every frame with real stick input, gated
 * on getDebugMode() == DEB_MOVE_VIEW inside a get_debug_freeze_processing() switch. What it
 * does NOT have is a consumer: nothing outside debug_camera.c reads debugCameraPosition, the
 * angles or the basis vectors, so the camera moves its own state every frame and no render path
 * ever sees it.
 *
 * This file is not a second camera. It is the part Rare's one is missing on a PC: a state the
 * port can drive from a keyboard, a gate that is not the build-time debug menu (GETV_DEBUGMENU
 * changes codegen and its level select is gutted no-ops), and an answer to "where is the camera"
 * that the two decompilation-side hooks can ask cheaply every frame.
 *
 * Angles are degrees, positions are world units, and both match the decompilation's conventions
 * so the hook side needs no unit juggling.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "ge_freecam.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Movement is per frame rather than per second, deliberately. Everything the camera exists to
 * do -- lining up a shot, stepping through a scene -- happens under GETV_EXIT_FRAME or on a
 * frozen world, where wall-clock time is not the thing being advanced. Tying it to a delta
 * would make two runs of the same script land in different places. */
#define FREECAM_MOVE_PER_FRAME   12.0f
#define FREECAM_TURN_PER_FRAME    2.0f
#define FREECAM_FAST_MULTIPLIER   4.0f
#define FREECAM_SLOW_MULTIPLIER   0.25f

/* Stops short of straight up and straight down. At exactly +/-90 the forward vector goes
 * parallel to the up vector and the view basis collapses; the decompilation's own camera keeps
 * a vertical angle for the same reason. */
#define FREECAM_PITCH_LIMIT      89.0f

static int   s_enabled = -1;
static int   s_active;
static int   s_seeded;
static float s_pos[3];
static float s_yaw;
static float s_pitch;

/* Edge-detected, so holding the key does not toggle every frame. The decompilation's own camera
 * does the same thing with `joyBtns & ~previousButtons`. */
static int   s_toggle_was_down;

/* GETV_FREECAM=1 enables it and waits for F8. GETV_FREECAM=<frame> above 1 also switches it on
 * by itself at that frame, which is the only way to exercise it on a measurement run: those set
 * GETV_EXIT_FRAME, and geKeyboardIdle() then reports "nothing held" for the whole run, so a key
 * nobody is there to press can never arrive. Same shape as GETV_AIM_SELFTEST, for the same
 * reason. */
static long s_auto_frame;
static unsigned long s_frames;

int gePortFreecamEnabled(void)
{
    if (s_enabled < 0) {
        const char *e = getenv("GETV_FREECAM");
        s_enabled = (e != NULL && *e != '\0' && *e != '0');
        s_auto_frame = s_enabled ? atol(e) : 0;
        if (s_auto_frame <= 1) { s_auto_frame = 0; }
        if (s_enabled) {
            printf("[getv][freecam] enabled. F8 toggles, WASD moves, arrows look, "
                   "R/F down-up, shift faster, ctrl slower\n");
            if (s_auto_frame > 0) {
                printf("[getv][freecam] will switch itself on at frame %ld\n", s_auto_frame);
            }
            fflush(stdout);
        }
    }
    return s_enabled;
}

int gePortFreecamActive(void)
{
    return gePortFreecamEnabled() && s_active && s_seeded;
}

int gePortFreecamSeeded(void)
{
    return s_seeded;
}

int gePortFreecamNeedsSeed(void)
{
    return gePortFreecamEnabled() && s_active && !s_seeded;
}

void gePortFreecamSeed(float x, float y, float z, float yaw, float pitch)
{
    const char *mv;

    s_pos[0] = x;
    s_pos[1] = y;
    s_pos[2] = z;
    s_yaw    = yaw;
    s_pitch  = pitch;
    s_seeded = 1;

    /* GETV_FREECAM_MOVE=x,y,z displaces the camera from Bond the moment it seeds.
     *
     * It exists because a scripted run has nobody to hold W: without it the camera switches on
     * exactly where the player is standing and the frame is indistinguishable from one with no
     * camera at all, which proves nothing. It is also the useful half of the feature for
     * screenshots -- a fixed offset is repeatable, where flying by hand is not. */
    mv = getenv("GETV_FREECAM_MOVE");
    if (mv != NULL && *mv != '\0') {
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        if (sscanf(mv, "%f,%f,%f", &dx, &dy, &dz) == 3) {
            s_pos[0] += dx;
            s_pos[1] += dy;
            s_pos[2] += dz;
            printf("[getv][freecam] seeded at %.1f,%.1f,%.1f (Bond %.1f,%.1f,%.1f + %.1f,%.1f,%.1f)\n",
                   s_pos[0], s_pos[1], s_pos[2], x, y, z, dx, dy, dz);
            fflush(stdout);
        }
    }
}

static void freecam_forward(float *fx, float *fz)
{
    const float r = (float)(s_yaw * M_PI / 180.0);
    *fx = sinf(r);
    *fz = -cosf(r);
}

void gePortFreecamTick(void)
{
    const Uint8 *k;
    float fx, fz, step, turn;
    int toggle_down;

    if (!gePortFreecamEnabled()) {
        return;
    }

    s_frames++;
    if (s_auto_frame > 0 && !s_active && (long) s_frames >= s_auto_frame) {
        s_active = 1;
        printf("[getv][freecam] ON (frame %lu, automatic)\n", s_frames);
        fflush(stdout);
    }

    k = SDL_GetKeyboardState(NULL);
    if (k == NULL) {
        /* No keyboard is not a reason to stop flying. An automatic run has no hand on one and
         * still needs the camera to hold whatever position it was moved to. */
        return;
    }

    /* The toggle is read whether or not the camera is active, and whether or not the keyboard
     * is otherwise idle -- geKeyboardIdle() deliberately reports "nothing held" on a
     * measurement run, and this is the one key that has to work there, or the camera can never
     * be switched on during exactly the runs it is most useful for. */
    toggle_down = k[SDL_SCANCODE_F8] ? 1 : 0;
    if (toggle_down && !s_toggle_was_down) {
        s_active = !s_active;
        /* Not seeded here. The hook side seeds from the player when it first sees the camera
         * go active, because only it can read bondviewGetCurrentPlayersPosition(). Until that
         * happens gePortFreecamActive() stays false and nothing overrides the view. */
        printf("[getv][freecam] %s\n", s_active ? "ON" : "OFF");
        fflush(stdout);
    }
    s_toggle_was_down = toggle_down;

    if (!s_active || !s_seeded) {
        return;
    }

    step = FREECAM_MOVE_PER_FRAME;
    turn = FREECAM_TURN_PER_FRAME;
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) {
        step *= FREECAM_FAST_MULTIPLIER;
        turn *= FREECAM_FAST_MULTIPLIER;
    }
    if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]) {
        step *= FREECAM_SLOW_MULTIPLIER;
        turn *= FREECAM_SLOW_MULTIPLIER;
    }

    if (k[SDL_SCANCODE_LEFT])  { s_yaw   -= turn; }
    if (k[SDL_SCANCODE_RIGHT]) { s_yaw   += turn; }
    if (k[SDL_SCANCODE_UP])    { s_pitch += turn; }
    if (k[SDL_SCANCODE_DOWN])  { s_pitch -= turn; }

    if (s_pitch >  FREECAM_PITCH_LIMIT) { s_pitch =  FREECAM_PITCH_LIMIT; }
    if (s_pitch < -FREECAM_PITCH_LIMIT) { s_pitch = -FREECAM_PITCH_LIMIT; }

    /* Yaw is wrapped rather than left to grow. It is fed to sinf/cosf every frame and also
     * handed to the hook side as a heading; letting it run to six figures over a long session
     * costs precision in both. */
    if (s_yaw >= 360.0f) { s_yaw -= 360.0f; }
    if (s_yaw <    0.0f) { s_yaw += 360.0f; }

    freecam_forward(&fx, &fz);

    if (k[SDL_SCANCODE_W]) { s_pos[0] += fx * step;  s_pos[2] += fz * step; }
    if (k[SDL_SCANCODE_S]) { s_pos[0] -= fx * step;  s_pos[2] -= fz * step; }
    /* Strafe is forward rotated a quarter turn, which is (-fz, fx). */
    if (k[SDL_SCANCODE_A]) { s_pos[0] += fz * step;  s_pos[2] -= fx * step; }
    if (k[SDL_SCANCODE_D]) { s_pos[0] -= fz * step;  s_pos[2] += fx * step; }
    if (k[SDL_SCANCODE_R]) { s_pos[1] += step; }
    if (k[SDL_SCANCODE_F]) { s_pos[1] -= step; }
}

void gePortFreecamGetPos(float *out_xyz)
{
    out_xyz[0] = s_pos[0];
    out_xyz[1] = s_pos[1];
    out_xyz[2] = s_pos[2];
}

void gePortFreecamGetAngles(float *out_yaw, float *out_pitch)
{
    *out_yaw   = s_yaw;
    *out_pitch = s_pitch;
}
