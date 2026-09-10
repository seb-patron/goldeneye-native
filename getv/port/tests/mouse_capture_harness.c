/* Compiled by tools/tests/test_mouse_capture.py with unchanged production source sections.
 * Real SDL headers verify the interface; these device stubs cannot capture the user's cursor. */
#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "port_input.h"
#include "ge_mouse_accum.h"
/* Same reason as ge_mouse_accum.h above: the extracted production code below uses these,
 * and both headers include nothing themselves, so pulling them in costs no SDL and no
 * game data. */
#include "ge_actions.h"
#include "ge_bindings.h"
#include "ge_wheel.h"
#ifdef GE_TEST_HAS_MODERN_MOUSE
#include "ge_mouse_look.h"
#endif
#include "ge_console_input.c"

static int window_storage, other_storage;
static SDL_Window *wnd = (SDL_Window *)&window_storage;
static SDL_Window *other = (SDL_Window *)&other_storage;
static SDL_Window *keyboard_focus, *mouse_focus;
static Uint8 keys[SDL_NUM_SCANCODES];
static Uint32 buttons;
static SDL_bool relative;
static int motion_x, motion_y, attempts, fail_mode, idle;
static SDL_Event queue[8];
static int queue_count, queue_index;

const Uint8 *SDL_GetKeyboardState(int *n) { return keys; }
SDL_Window *SDL_GetKeyboardFocus(void) { return keyboard_focus; }
SDL_Window *SDL_GetMouseFocus(void) { return mouse_focus; }
Uint32 SDL_GetWindowID(SDL_Window *window) { return window == wnd ? 7 : 8; }
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h) { *w = 640; *h = 480; }
Uint32 SDL_GetMouseState(int *x, int *y) { return buttons; }
SDL_bool SDL_GetRelativeMouseMode(void) { return relative; }
const char *SDL_GetError(void) { return "simulated capture failure"; }
int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    attempts++;
    if (fail_mode == 1) return -1;
    if (fail_mode == 0) relative = enabled;
    return 0;
}
Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
    if (x) *x = motion_x;
    if (y) *y = motion_y;
    motion_x = motion_y = 0;
    return buttons;
}
int SDL_PollEvent(SDL_Event *event)
{
    if (queue_index == queue_count) return 0;
    *event = queue[queue_index++];
    if (event->type == SDL_MOUSEBUTTONDOWN) buttons |= SDL_BUTTON(event->button.button);
    if (event->type == SDL_MOUSEBUTTONUP) buttons &= ~SDL_BUTTON(event->button.button);
    if (event->type == SDL_WINDOWEVENT && event->window.windowID == 7) {
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            keyboard_focus = mouse_focus = NULL;
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            keyboard_focus = mouse_focus = wnd;
    }
    return 1;
}

static int geKeyboardIdle(void) { return idle; }
int gePortInputDebugLevel(void) { return 0; }
#include "mouse.inc"

/* Dependencies of gfx_sdl_handle_events unrelated to mouse handoff. Console ownership itself
 * uses production ge_console_input.c above; its ImGui event adapter is represented here. */
static struct { int exiting_fullscreen, x, y, w, h, settings_changed; } configWindow;
#define IS_FULLSCREEN() 0
static void (*kb_all_keys_up)(void);
static int gePortImguiConsoleOpen(void) { return geConsoleInputOpen(); }
static int gePortImguiEvent(void *event) { return geConsoleInputCaptureActive(); }
static void gfx_sdl_onkeydown(int code) {}
static void gfx_sdl_onkeyup(int code) {}
static void game_exit(void) {}
static void gfx_sdl_set_fullscreen(void) {}
static void gfx_sdl_reset_dimension_and_pos(void) {}
#include "events.inc"

/* Exercise the real controller/no-controller branch, not just geMousePoll in isolation.
 * SDL controller state, discovery and scripts are simulated at their boundaries. */
static int controller_mode, controller_storage[2];
static SDL_GameController *gePads[GE_PORT_MAX_PADS];
static int gePadReal[GE_PORT_MAX_PADS];
static int geSynthFrame;
static int geScriptPort, script_active;
static Sint16 controller_axes[SDL_CONTROLLER_AXIS_MAX];
static Uint8 controller_buttons[SDL_CONTROLLER_BUTTON_MAX];
#define GE_TRIGGER_ON 8000
void *SDL_memset(void *dst, int value, size_t size) { return memset(dst, value, size); }
int SDL_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }

/* Scancode names, for the keyboard binding layer.
 *
 * Stubbed rather than linked, like every other SDL entry point in this harness -- the
 * whole point of this file is that it runs with SDL HEADERS and no SDL library. But
 * unlike the others these cannot be inert: geKeymapEnsure() resolves the presets through
 * SDL_GetScancodeFromName, so a stub that returned UNKNOWN would leave every keyboard
 * action unbound and the scenarios below would be testing nothing while still passing.
 *
 * This table must cover every name used by a preset in ge_bindings.c. It does not have
 * to cover all of SDL's ~240 names, and deliberately does not -- an unlisted name is
 * reported by geParseCodeList as "not a key name" on stdout, and the movement and fire
 * scenarios below fail outright, which is the loud failure wanted here.
 *
 * The spellings are SDL's own, because that is what a config file contains. */
static const struct { SDL_Scancode code; const char *name; } scancode_names[] = {
    { SDL_SCANCODE_A, "A" }, { SDL_SCANCODE_C, "C" }, { SDL_SCANCODE_D, "D" },
    { SDL_SCANCODE_E, "E" }, { SDL_SCANCODE_F, "F" }, { SDL_SCANCODE_Q, "Q" },
    { SDL_SCANCODE_R, "R" }, { SDL_SCANCODE_S, "S" }, { SDL_SCANCODE_V, "V" },
    { SDL_SCANCODE_W, "W" }, { SDL_SCANCODE_X, "X" }, { SDL_SCANCODE_Z, "Z" },
    { SDL_SCANCODE_I, "I" }, { SDL_SCANCODE_J, "J" }, { SDL_SCANCODE_K, "K" },
    { SDL_SCANCODE_L, "L" },
    { SDL_SCANCODE_SPACE, "Space" }, { SDL_SCANCODE_RETURN, "Return" },
    { SDL_SCANCODE_TAB, "Tab" }, { SDL_SCANCODE_ESCAPE, "Escape" },
    { SDL_SCANCODE_BACKSPACE, "Backspace" },
    { SDL_SCANCODE_KP_ENTER, "Keypad Enter" },
    { SDL_SCANCODE_LCTRL, "Left Ctrl" }, { SDL_SCANCODE_RCTRL, "Right Ctrl" },
    { SDL_SCANCODE_LSHIFT, "Left Shift" }, { SDL_SCANCODE_RSHIFT, "Right Shift" },
    { SDL_SCANCODE_LALT, "Left Alt" }, { SDL_SCANCODE_RALT, "Right Alt" },
    { SDL_SCANCODE_UP, "Up" }, { SDL_SCANCODE_DOWN, "Down" },
    { SDL_SCANCODE_LEFT, "Left" }, { SDL_SCANCODE_RIGHT, "Right" },
};

const char *SDL_GetScancodeName(SDL_Scancode code)
{
    size_t i;
    for (i = 0; i < sizeof scancode_names / sizeof scancode_names[0]; i++) {
        if (scancode_names[i].code == code) { return scancode_names[i].name; }
    }
    return "";
}

SDL_Scancode SDL_GetScancodeFromName(const char *name)
{
    size_t i;
    if (name == NULL || *name == '\0') { return SDL_SCANCODE_UNKNOWN; }
    /* Case-insensitive, as SDL's own implementation is. */
    for (i = 0; i < sizeof scancode_names / sizeof scancode_names[0]; i++) {
        if (strcasecmp(name, scancode_names[i].name) == 0) { return scancode_names[i].code; }
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* The binding layer itself. SDL-free and game-data-free, so it is compiled straight in
 * -- the same arrangement as ge_console_input.c above. Without it the extracted keyboard
 * code has no geActionName/gePresetKeys/geInputPreset to call. */
#include "ge_bindings.c"
void SDL_GameControllerUpdate(void) {}
Sint16 SDL_GameControllerGetAxis(SDL_GameController *gc, SDL_GameControllerAxis axis)
{ return controller_axes[axis]; }
Uint8 SDL_GameControllerGetButton(SDL_GameController *gc, SDL_GameControllerButton button)
{ return controller_buttons[button]; }
static void gePortAndroidTouchInit(void) {}
static void gePortAndroidTouchUpdate(void) {}
int gePortInputPadCount(void) { return gePads[0] != NULL; }
static void geFrontTraceTick(int frame) {}
static int geScriptActive(void) { return script_active; }
static void geScriptApply(int port, int frame, struct GePadState *out)
{ if (script_active && port == geScriptPort) out->rx = 1234; }
int gePortForcedPads(void) { return 0; }
int gePortSynthEnabled(void) { return 0; }
static void geSynthState(int port, struct GePadState *out) {}
#include "keyboard.inc"
#include "polling.inc"

static int failures;
static void check(int condition, const char *name)
{
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    failures += !condition;
}
static struct GePadState poll(void)
{
    struct GePadState out = {0};
    if (controller_mode) gePortInputPollPortInner(0, &out);
    else geMousePoll(0, &out);
    return out;
}
static void release_buttons(void) { buttons = 0; (void)poll(); }
static void escape(int held) { keys[SDL_SCANCODE_ESCAPE] = held; (void)poll(); }
static void release_cursor(void) { escape(1); escape(0); }
static SDL_Event click_event(Uint32 type, int button, Uint32 id, int x, int y)
{
    SDL_Event event = {0};
    event.type = type;
    event.button.button = button;
    event.button.windowID = id;
    event.button.x = x;
    event.button.y = y;
    return event;
}
static void dispatch(SDL_Event event)
{
    queue[0] = event; queue_count = 1; queue_index = 0;
    gfx_sdl_handle_events();
}
static void click(void) { dispatch(click_event(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 7, 100, 100)); }
static void focus_event(int kind, Uint32 id)
{
    SDL_Event event = {0};
    event.type = SDL_WINDOWEVENT;
    event.window.event = kind;
    event.window.windowID = id;
    dispatch(event);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const char *scenario = argv[1];
    printf("scenario: %s\n", scenario);
    if (strncmp(scenario, "controller-", 11) == 0) {
        controller_mode = 1;
        gePads[0] = (SDL_GameController *)&controller_storage[0];
        gePadReal[0] = 1;
        scenario += 11;
    }
    unsetenv("GETV_MOUSE_SELFTEST"); unsetenv("GETV_MOUSE_SELFTEST_Y");
    unsetenv("GETV_NET_HOST"); unsetenv("GETV_NET_JOIN");
    unsetenv("GETV_RAMROM"); unsetenv("GETV_SCRIPT"); unsetenv("GETV_DEMO");
    setenv("GETV_MOUSE_MODE", strcmp(scenario, "modern") == 0 ? "modern" : "classic", 1);
    setenv("GETV_MOUSE", strcmp(scenario, "disabled") == 0 ? "0" : "1", 1);
    setenv("GETV_MOUSE_SENS", "100", 1); setenv("GETV_MOUSE_INVERT", "0", 1);
    setenv("GETV_KEYBOARD", strcmp(scenario, "no-keyboard") == 0 ? "0" : "1", 1);
    setenv("GETV_KEYBOARD_UNFOCUSED", "0", 1); setenv("GETV_AIM_SELFTEST", "0", 1);
    keyboard_focus = mouse_focus = wnd;
    geConsoleInputReset();
    if (strcmp(scenario, "idle") == 0) idle = 1;
    if (strcmp(scenario, "selftest-x") == 0) setenv("GETV_MOUSE_SELFTEST", "12", 1);
    if (strcmp(scenario, "selftest-y") == 0) setenv("GETV_MOUSE_SELFTEST_Y", "12", 1);
    if (strcmp(scenario, "unfocused-start") == 0) keyboard_focus = mouse_focus = NULL;
    struct GePadState out = poll();

    if (strcmp(scenario, "modern") == 0) {
#ifdef GE_TEST_HAS_MODERN_MOUSE
        float yaw, pitch;
        check(gePortInputTakeMouseLook(0, 1, &yaw, &pitch), "modern mode activates");
        controller_axes[SDL_CONTROLLER_AXIS_RIGHTX] = 16000;
        motion_x = 1000; motion_y = -40; out = poll();
        check(out.rx == (controller_mode ? 16000 : 0), "mouse leaves controller look axes intact");
        gePortInputTakeMouseLook(1, 1, &yaw, &pitch);
        check(yaw == 0 && pitch == 0, "player two cannot consume mouse travel");
        gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        check(fabsf(yaw - 100.0f) < 0.0001f && fabsf(pitch - 4.0f) < 0.0001f,
              "fast swipe is full displacement without a stick speed ceiling");
        poll(); gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        check(yaw == 0 && pitch == 0, "stopping has no residual motion");
        motion_x = -1; poll(); gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        check(fabsf(yaw + 0.1f) < 0.0001f, "tiny reversal immediately turns the other way");
        for (int i = 0; i < 10; ++i) { motion_x = 100; poll(); }
        gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        check(fabsf(yaw - 100.0f) < 0.0001f, "event batching and delayed ticks preserve total travel");
        motion_x = 1000; poll(); gePortInputTakeMouseLook(0, 0, &yaw, &pitch);
        check(yaw == 0, "pause and cutscene context discard pending movement");
        motion_x = 1000; poll(); gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        check(yaw == 0, "return to gameplay never replays menu movement");
        motion_x = 1000; poll(); focus_event(SDL_WINDOWEVENT_FOCUS_LOST, 7);
        gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        check(yaw == 0, "focus loss clears modern motion");
        keyboard_focus = mouse_focus = wnd; click(); release_buttons();
        gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        motion_x = 1000; poll(); gePortInputConsoleCapture(1);
        gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        check(yaw == 0, "console capture clears modern motion");
        gePortInputConsoleCapture(0); gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        motion_x = 1000; poll(); release_cursor();
        gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        check(yaw == 0, "Escape clears modern motion");
        click(); release_buttons();
        check(!gePortInputTakeMouseLook(0, 2, &yaw, &pitch), "vehicle context retains classic controls");
        motion_x = 50; out = poll();
        check(out.rx == 32767, "vehicle mouse still turns through the original stick path");
        gePortInputTakeMouseLook(0, 1, &yaw, &pitch);
        check(yaw == 0, "leaving vehicle discards old movement");
        setenv("GETV_NET_HOST", "27100", 1);
        check(!gePortInputTakeMouseLook(0, 1, &yaw, &pitch), "lockstep keeps N64 input contract");
        motion_x = 50; out = poll();
        check(out.rx == 32767, "network mouse retains classic stick input");
        unsetenv("GETV_NET_HOST"); setenv("GETV_MOUSE", "0", 1);
        check(!gePortInputTakeMouseLook(0, 1, &yaw, &pitch), "disabled mouse does not own pitch centering");
#else
        motion_x = 1000; poll(); poll();
        motion_x = -1; out = poll();
        check(out.rx < 0, "tiny reversal after a fast swipe immediately turns the other way");
#endif
        return failures ? 1 : 0;
    }

    if (strcmp(scenario, "disabled") == 0 || strcmp(scenario, "idle") == 0 ||
        strncmp(scenario, "selftest-", 9) == 0) {
        check(attempts == 0, "initial poll never captures automated or disabled mouse");
        click();
        check(attempts == 0 && !relative, "click cannot capture automated or disabled mouse");
        if (strcmp(scenario, "selftest-x") == 0) check(out.rx != 0, "horizontal selftest still drives look");
        if (strcmp(scenario, "selftest-y") == 0) check(out.ry != 0, "vertical selftest still drives look");
        if (strcmp(scenario, "disabled") != 0) {
            ge_mouse_pend_x = 12000;
            focus_event(SDL_WINDOWEVENT_FOCUS_LOST, 7);
            check(ge_mouse_pend_x == 12000 && attempts == 0,
                  "focus loss does not alter automated mouse carry or capture");
        }
        return failures != 0;
    }
    if (strcmp(scenario, "unfocused-start") == 0) {
        check(attempts == 0, "unfocused first poll does not request capture");
        focus_event(SDL_WINDOWEVENT_FOCUS_GAINED, 7);
        click(); out = poll();
        check(relative && !out.act[GE_ACT_FIRE], "first focused click captures without firing");
        return failures != 0;
    }
    check(relative, "initial mouse capture");

    if (strcmp(scenario, "no-keyboard") == 0) {
        motion_x = 12; buttons = SDL_BUTTON_RMASK; out = poll();
        check(out.rx > 0 && out.act[GE_ACT_AIM], "controller and mouse work with keyboard disabled");
        return failures != 0;
    }
    if (strcmp(scenario, "mixed") == 0) {
        controller_axes[SDL_CONTROLLER_AXIS_LEFTX] = 16000;
        controller_axes[SDL_CONTROLLER_AXIS_LEFTY] = -18000;
        controller_axes[SDL_CONTROLLER_AXIS_RIGHTX] = -12000;
        controller_axes[SDL_CONTROLLER_AXIS_RIGHTY] = 14000;
        controller_buttons[SDL_CONTROLLER_BUTTON_A] = 1;
        motion_x = 12; motion_y = -6;
        buttons = SDL_BUTTON_LMASK | SDL_BUTTON_RMASK;
        out = poll();
        check(out.lx == 16000 && out.ly == -18000 && out.a,
              "controller movement and buttons survive simultaneous mouse look");
        check(out.rx > 0 && out.ry < 0, "mouse movement overrides controller look axes");
        /* The ACTION, not the trigger fields.
         *
         * The mouse used to write `rtrigger`/`ltrigger` directly, which meant it was not
         * bound to fire and aim so much as bound to whatever those two happened to sit
         * on -- rebinding aim to a face button took it off the right mouse button too.
         * Mouse buttons are ordinary bindings now, so the trigger fields stay 0 and the
         * action is what carries the meaning. This is a stronger check than the one it
         * replaces: it holds however the player has remapped things. */
        check(out.act[GE_ACT_FIRE] && out.act[GE_ACT_AIM],
              "mouse fire and aim work with a connected controller");
        check(!out.rtrigger && !out.ltrigger,
              "the mouse no longer fakes a gamepad trigger to say so");
        keys[SDL_SCANCODE_RIGHT] = 1; keys[SDL_SCANCODE_W] = 1;
        motion_x = -12; out = poll();
        check(out.rx < 0 && out.ly == -GE_KB_FULL && out.lx == 16000,
              "mouse look wins over arrow key while keyboard and controller movement coexist");
        keys[SDL_SCANCODE_RIGHT] = keys[SDL_SCANCODE_W] = 0;
        buttons = 0; out = poll();
        check(out.rx == -12000 && out.ry == 14000 && !out.rtrigger && !out.ltrigger,
              "stationary released mouse preserves controller look without sticking buttons");
        controller_axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = 17000;
        controller_axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 19000;
        out = poll();
        check(out.ltrigger && out.rtrigger && out.lt_raw == 17000 && out.rt_raw == 19000,
              "released mouse preserves held controller triggers");
        controller_axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = 0;
        controller_axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 0;
        gePads[1] = (SDL_GameController *)&controller_storage[1]; gePadReal[1] = 1;
        motion_x = 12; buttons = SDL_BUTTON_RMASK;
        gePortInputPollPortInner(1, &out);
        check(out.rx == -12000 && !out.ltrigger && motion_x == 12,
              "player two never consumes player one's mouse input");
        script_active = 1; out = poll();
        check(out.rx == 1234, "script retains priority over physical mouse input");
        script_active = 0; buttons = 0;
        gePads[0] = NULL; motion_x = 12; out = poll();
        check(out.rx > 0, "mouse keeps working after controller detach");
        gePads[0] = (SDL_GameController *)&controller_storage[0];
        motion_x = 12; out = poll();
        check(out.rx > 0 && out.lx == 16000, "mouse and movement work after controller reconnect");
        return failures != 0;
    }

    if (strcmp(scenario, "focus") == 0) {
        focus_event(SDL_WINDOWEVENT_FOCUS_LOST, 8);
        check(relative, "another window losing focus does not release game mouse");
        motion_x = 24; ge_mouse_pend_x = 4000;
        focus_event(SDL_WINDOWEVENT_FOCUS_LOST, 7);
        check(!relative, "game focus loss releases logical capture");
        check(ge_mouse_pend_x == 0 && motion_x == 0, "focus loss clears stale motion and carry");
        focus_event(SDL_WINDOWEVENT_FOCUS_GAINED, 7);
        check(!relative, "focus alone does not recapture");
        click(); out = poll();
        check(relative && !out.act[GE_ACT_FIRE] && !out.rx, "activating click resumes without shot or jump");
        return failures != 0;
    }

    release_cursor();
    check(!relative, "Escape releases mouse");
    int before = attempts;
    if (strcmp(scenario, "ownership") == 0) {
        dispatch(click_event(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_RIGHT, 7, 100, 100));
        check(attempts == before, "right click does not recapture");
        release_buttons();
        dispatch(click_event(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 8, 100, 100));
        check(attempts == before, "another window click does not recapture");
        release_buttons();
        const int coords[][2] = {{-1, 100}, {640, 100}, {100, -1}, {100, 480}};
        for (int i = 0; i < 4; i++) {
            dispatch(click_event(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 7, coords[i][0], coords[i][1]));
            check(attempts == before, "out-of-client click does not recapture");
            release_buttons();
        }
        mouse_focus = other; click();
        check(attempts == before, "mouse outside game does not recapture");
        mouse_focus = wnd; release_buttons();
        keyboard_focus = NULL; click();
        check(attempts == before, "unfocused click does not request capture");
        keyboard_focus = wnd; release_buttons();
        buttons = SDL_BUTTON_LMASK; (void)poll();
        check(attempts == before, "held drag without click event does not recapture");
        release_buttons();
        geConsoleInputSetOpen(1); gePortInputConsoleCapture(1); before = attempts;
        click(); out = poll();
        check(attempts == before && !out.act[GE_ACT_FIRE], "open console owns the click");
        geConsoleInputSetOpen(0); gePortInputConsoleCapture(0); before = attempts;
        click(); out = poll();
        check(attempts == before && !out.act[GE_ACT_FIRE], "console close quarantine owns held click");
        release_buttons(); click();
        check(relative, "fresh click works after console quarantine ends");
        return failures != 0;
    }
    if (strcmp(scenario, "failure") == 0) {
        fail_mode = 1; click();
        check(!relative && attempts == before + 1, "SDL capture failure leaves mouse released");
        check(!ge_mouse_capture_wanted, "failed capture does not claim capture intent");
        release_buttons(); fail_mode = 2; before = attempts; click();
        check(!relative && attempts == before + 1, "successful return with wrong mode is not accepted");
        release_buttons(); fail_mode = 0; click();
        check(relative && ge_mouse_capture_wanted, "fresh click retries failed capture");
        release_buttons(); fail_mode = 1; escape(1);
        check(relative && ge_mouse_capture_wanted, "failed Escape release preserves actual state and intent");
        return failures != 0;
    }

    motion_x = 72; motion_y = 48; ge_mouse_pend_x = 12000; ge_mouse_pend_y = 6000;
    click();
    check(relative && attempts == before + 1, "click event recaptures after Escape");
    out = poll();
    check(!out.rx && !out.ry, "recapture discards pre-capture motion and carry");
    check(!out.act[GE_ACT_FIRE], "resume click does not fire");
    motion_x = 12; out = poll();
    check(out.rx != 0 && !out.act[GE_ACT_FIRE], "mouse look resumes while resume click is held");
    buttons |= SDL_BUTTON_RMASK; out = poll();
    check(!out.act[GE_ACT_FIRE] && !out.act[GE_ACT_AIM],
          "mouse actions stay blocked until all buttons release");
    release_buttons(); click(); out = poll();
    check(out.act[GE_ACT_FIRE], "fresh click after release fires normally");
    release_buttons(); release_cursor();
    queue[0] = click_event(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, 7, 100, 100);
    queue[1] = click_event(SDL_MOUSEBUTTONUP, SDL_BUTTON_LEFT, 7, 100, 100);
    queue_count = 2; queue_index = 0; gfx_sdl_handle_events(); out = poll();
    check(relative && !out.act[GE_ACT_FIRE], "click down and up between polls still recaptures without firing");
    for (int i = 0; i < 3; i++) {
        release_buttons(); release_cursor(); click(); out = poll();
        check(relative && !out.act[GE_ACT_FIRE], "repeated Escape-click cycle stays usable");
    }
    release_buttons(); escape(1); before = attempts; escape(1);
    check(!relative && attempts == before, "held Escape does not flap capture");
    escape(0); buttons = SDL_BUTTON_LMASK; escape(1); out = poll();
    check(relative && !out.act[GE_ACT_FIRE], "second Escape recaptures without leaking a held click");
    return failures != 0;
}
