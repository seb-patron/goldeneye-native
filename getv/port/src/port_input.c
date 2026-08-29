/* GoldenEye native port - SDL GameController, replacing the N64's SI bus.
 *
 * See port_input.h for why the SDL half lives in its own translation unit.
 *
 * Two facts about tvOS input, both established on device during the Perfect Dark port
 * (commit 3b4b554, "stop the Siri Remote from claiming player 1") rather than guessed
 * at here. Both are silent failures - the app runs perfectly and no button does
 * anything - so they are worth stating plainly:
 *
 *   1. The Siri Remote enumerates as a joystick, and it tends to appear BEFORE the
 * real gamepad. Whatever takes index 0 becomes player 1, so the remote wins and
 * the actual pad is assigned to player 2. SDL_HINT_TV_REMOTE_AS_JOYSTICK must be
 * cleared BEFORE SDL_INIT_GAMECONTROLLER. The remote is useless for an FPS anyway.
 *
 * The hint is necessary but not sufficient, which is why there is a second layer
 * here. The hint is a request; whether the remote enumerates anyway depends on the
 * SDL build, and any future controller-like accessory (a remote in a game case, a
 * phone acting as a controller) hits the same "index 0 wins" trap. So selection
 * ranks candidates instead of taking the first one, and will upgrade from a
 * stickless device to a real pad whenever one shows up.
 *
 *   2. tvOS enumerates gamepads asynchronously. SDL_NumJoysticks() is 0 while the game
 * is still running its own osContInit(), and pads only arrive some frames later.
 * Anything that decides "is there a controller?" once, at init, decides "no"* forever. Hence the rescan on every poll.
 *
 * ---------------------------------------------------------------------------
 * All four N64 ports are supported. With a single `SDL_GameController *gePad` here and
 * a hard-coded `*bitpattern = 1` in port_os.c, players 2-4 were input-dead: joy.c's
 * accessors return early for any port whose bit is clear in g_ConnectedControllers, so
 * split-screen rendered and nobody but player 1 could move.
 *
 * The single pad is now `gePads[GE_PORT_MAX_PADS]`, and every rule the one-pad path had
 * earned is preserved:
 *
 *   - Ranking, not index 0. Real gamepads are taken first; a stickless device is only
 * ever accepted when nothing else is present, and only into port 0.
 *   - Upgrade. A stickless device holding port 0 is evicted the moment a real pad
 * enumerates.
 *   - Classification by capability (HasAxis/HasButton), never by product name.
 *
 * With four ports there is one further rule: a stickless device is never assigned to
 * ports 1-3. Accepting a Siri Remote into port 2 would make joyGetControllerCount()
 * report 3 and offer a 3-player match with one seat that cannot move. Under one port
 * that mistake was merely useless; under four it is a broken game mode. The remote is a
 * menu-only fallback for player 1 and nothing else.
 *
 * Ports must stay contiguous - see the comment on gePortInputPadCount() in the header.
 * Detach compacts the array rather than leaving a hole.
 */
#include <stdio.h>
#include <stdlib.h>

#include <SDL.h>

#include "port_input.h"
#include "ge_mouse_accum.h"/* Ports 0..3. gePads[i] != NULL implies gePads[j] != NULL for all j < i (contiguity). */
static SDL_GameController *gePads[GE_PORT_MAX_PADS];
static int gePadReal[GE_PORT_MAX_PADS];     /* the pad on this port has sticks + shoulders */
static int gePadCount   = 0;                /* number of OPEN devices, contiguous from 0 */
static int geSubsysReady = 0;

/* Devices examined and rejected as stickless while a real pad was available. Kept so
 * the scan does not open/close a Siri Remote every single frame forever. Consulted only
 * while at least one real device is open - if everything detaches, the remote becomes
 * the menu-only fallback again and must be reconsidered. */
#define GE_REJECT_MAX 16
static SDL_JoystickID geRejected[GE_REJECT_MAX];
static int geRejectCount = 0;

/* SDL's triggers are analogue; the N64's Z is a switch. */
#define GE_TRIGGER_ON 8000

/* Advances once per pass over the four ports, whether or not any port is synthetic. */
static int geSynthFrame = 0;

/* ===========================================================================
 * GETV_SCRIPT -- scripted input injection.
 *
 * Why an input source and not another state jump: `GETV_MENU=<n>` (initmenus.c) writes
 * `menu_update` directly, reaching a screen by skipping every constructor and
 * transition in between, and a front-end bug is at least as likely to live in that
 * initialisation as in the screen itself. Everything from MENU_FILE_SELECT onward is
 * input-only anyway (front.c:2451 wants the cursor inside a folder bbox plus A/Z/START,
 * and the only timer there sends you backwards).
 *
 * Injection happens into `struct GePadState`, i.e. at the device side, upstream of
 * gePortDecodePad() in port_os.c. Everything downstream is the production path: the N64
 * bit mapping, the stick rescale, the C-button Schmitt triggers, osContGetReadData(),
 * joy.c's 20-deep sample ring and its `buttonspressed |= cur & ~prev` edge detector. A
 * script entry is indistinguishable from a human holding the pad.
 *
 * A button must be held for at least 2 frames. joyConsumeSamples() (joy.c:371) derives
 * `buttonspressed` from consecutive samples, so a 1-frame blip is only a press if the
 * ring happened to catch both edges. Default hold is 4 frames. It is also why every
 * entry has an explicit release: a button left down forever produces exactly one press
 * and then blocks the idle timers that other screens use.
 *
 * Syntax GETV_SCRIPT="<frame>:<keys>[:<hold>][,...]"* frame the poll tick to fire on (geSynthFrame; one tick per osContGetReadData,
 * i.e. per game frame -- the same clock GETV_EXIT_FRAME counts)
 * keys   '+'-joined, case-insensitive:
 * A B X Y START BACK Z L R DU DD DL DR CU CD CL CR
 * LT RT   the analogue triggers. Z is the same as RT, because that is
 * where FIRE is bound by default; L and R are the shoulder buttons.
 * SX=<n> SY=<n> N64 stick counts, -80..80 (SY+ = up, as the game reads it)
 * hold frames to hold, default 4
 *
 * GETV_SCRIPT_PORT=<n> which N64 port the script drives (default 0)
 * GETV_SCRIPT_TRACE=0 silence the per-entry log (on by default -- it is the only
 * proof the entry fired, and it is a handful of lines a run)
 *
 * A live script forces port 0 present, so it works with no hardware and no GETV_PADS.
 * Overlapping entries or together, so a stick can be held across several taps.
 *
 * Diagnostic only: with GETV_SCRIPT unset nothing in this file changes behaviour.
 * =========================================================================== */

#define GE_SCRIPT_MAX 64

struct GeScriptEntry {
 int frame;
 int hold;
 unsigned keys;      /* GE_SK_* */
 int sx, sy;        /* N64 counts, -80..80 */
 int have_stick;
 int fired;
};

enum {
 GE_SK_A = 1u << 0, GE_SK_B = 1u << 1, GE_SK_X = 1u << 2, GE_SK_Y = 1u << 3,
 GE_SK_START = 1u << 4, GE_SK_BACK = 1u << 5, GE_SK_Z = 1u << 6,
 GE_SK_L = 1u << 7, GE_SK_R = 1u << 8,
 GE_SK_DU = 1u << 9, GE_SK_DD = 1u << 10, GE_SK_DL = 1u << 11, GE_SK_DR = 1u << 12,
 GE_SK_CU = 1u << 13, GE_SK_CD = 1u << 14, GE_SK_CL = 1u << 15, GE_SK_CR = 1u << 16,
 /* The analogue triggers, addressable in their own right. GE_SK_Z drives the right one
  * because that is where FIRE lives (port_os.c binds GE_ACT_FIRE to GE_SRC_RT by default),
  * but a script that wants a trigger without caring what is bound to it needs to be able to
  * say so. */
 GE_SK_LT = 1u << 17, GE_SK_RT = 1u << 18
};

static struct GeScriptEntry geScript[GE_SCRIPT_MAX];
static int geScriptCount  = -1;     /* -1 = not parsed yet, 0 = no script */
static int geScriptPort   = 0;
static int geScriptTrace  = 1;

/* N64 stick counts -> the SDL axis value gePortDecodePad()'s geStick() will turn back
 * into (approximately) that count. Inverse of geStick's rescale, deadzone included --
 * without the deadzone term a requested 70 would arrive as 62 and every "is the stick * past the rail" test in front.c would be answered about a different number than the
 * one written in the script. */
#define GE_SCRIPT_DEADZONE 3200
#define GE_SCRIPT_AXISMAX  32767
#define GE_SCRIPT_N64MAX   80

static int geScriptAxis(int n64)
{
 int sign = (n64 < 0) ? -1 : 1;
 int mag  = (n64 < 0) ? -n64 : n64;

 if (mag == 0) { return 0; }
 if (mag > GE_SCRIPT_N64MAX) { mag = GE_SCRIPT_N64MAX; }

 return sign * (GE_SCRIPT_DEADZONE
                   + (mag * (GE_SCRIPT_AXISMAX - GE_SCRIPT_DEADZONE)) / GE_SCRIPT_N64MAX);
}

static int geScriptKeyword(const char *tok, size_t len, struct GeScriptEntry *e)
{
 char buf[24];
 size_t i;

 if (len == 0 || len >= sizeof(buf)) { return 0; }
 for (i = 0; i < len; i++) {
 buf[i] = (char)SDL_toupper((unsigned char)tok[i]);
    }
 buf[len] = '\0';

 if (buf[0] == 'S' && (buf[1] == 'X' || buf[1] == 'Y') && buf[2] == '=') {
 int v = atoi(buf + 3);
 if (buf[1] == 'X') { e->sx = v; } else { e->sy = v; }
 e->have_stick = 1;
 return 1;
    }

#define GE_SK_MATCH(name, bit) if (SDL_strcmp(buf, name) == 0) { e->keys |= (bit); return 1; }
 GE_SK_MATCH("A", GE_SK_A) GE_SK_MATCH("B", GE_SK_B)
 GE_SK_MATCH("X", GE_SK_X) GE_SK_MATCH("Y", GE_SK_Y)
 GE_SK_MATCH("START", GE_SK_START) GE_SK_MATCH("BACK", GE_SK_BACK)
 GE_SK_MATCH("Z", GE_SK_Z)
 GE_SK_MATCH("LT", GE_SK_LT) GE_SK_MATCH("RT", GE_SK_RT)
 GE_SK_MATCH("L", GE_SK_L) GE_SK_MATCH("R", GE_SK_R)
 GE_SK_MATCH("DU", GE_SK_DU) GE_SK_MATCH("DD", GE_SK_DD)
 GE_SK_MATCH("DL", GE_SK_DL) GE_SK_MATCH("DR", GE_SK_DR)
 GE_SK_MATCH("CU", GE_SK_CU) GE_SK_MATCH("CD", GE_SK_CD)
 GE_SK_MATCH("CL", GE_SK_CL) GE_SK_MATCH("CR", GE_SK_CR)
#undef GE_SK_MATCH

 printf("[getv][script] unknown key '%s' -- ignored\n", buf);
 return 0;
}

static void geScriptParse(void)
{
 const char *s = getenv("GETV_SCRIPT");
 const char *p;
 const char *t;

 geScriptCount = 0;
 if (s == NULL || *s == '\0') { return; }

 t = getenv("GETV_SCRIPT_PORT");
 if (t != NULL && *t != '\0') {
 geScriptPort = atoi(t);
 if (geScriptPort < 0 || geScriptPort >= GE_PORT_MAX_PADS) { geScriptPort = 0; }
    }
 t = getenv("GETV_SCRIPT_TRACE");
 if (t != NULL && *t != '\0') { geScriptTrace = (atoi(t) != 0); }

 p = s;
 while (*p != '\0' && geScriptCount < GE_SCRIPT_MAX) {
 const char *end = SDL_strchr(p, ',');
 const char *stop = (end != NULL) ? end : (p + SDL_strlen(p));
 const char *c1 = NULL, *c2 = NULL, *q;
 struct GeScriptEntry e;

 SDL_memset(&e, 0, sizeof(e));
 e.hold = 4;

 for (q = p; q < stop; q++) {
 if (*q == ':') {
 if (c1 == NULL) { c1 = q; } else if (c2 == NULL) { c2 = q; }
            }
        }

 if (c1 == NULL) {
 printf("[getv][script] entry without a ':' -- ignored\n");
        } else {
 const char *kstart = c1 + 1;
 const char *kstop  = (c2 != NULL) ? c2 : stop;
 const char *k;

 e.frame = atoi(p);
 if (c2 != NULL) {
 e.hold = atoi(c2 + 1);
 if (e.hold < 1) { e.hold = 1; }
            }

 k = kstart;
 while (k < kstop) {
 const char *kend = k;
 while (kend < kstop && *kend != '+') { kend++; }
 geScriptKeyword(k, (size_t)(kend - k), &e);
 k = (kend < kstop) ? (kend + 1) : kstop;
            }

 geScript[geScriptCount++] = e;
        }

 if (end == NULL) { break; }
 p = end + 1;
    }

 printf("[getv][script] %d entr%s parsed, port %d: %s\n",
 geScriptCount, (geScriptCount == 1) ? "y" : "ies", geScriptPort, s);
 fflush(stdout);
}

static int geScriptActive(void)
{
 if (geScriptCount < 0) { geScriptParse(); }
 return geScriptCount > 0;
}

/* Overlay every entry live on `frame` onto `out`. or semantics, so a held stick and a
 * tapped button can overlap. */
static void geScriptApply(int port, int frame, struct GePadState *out)
{
 unsigned keys = 0;
 int sx = 0, sy = 0, have_stick = 0;
 int i;

 if (!geScriptActive() || port != geScriptPort) { return; }

 for (i = 0; i < geScriptCount; i++) {
 struct GeScriptEntry *e = &geScript[i];

 if (frame < e->frame || frame >= e->frame + e->hold) { continue; }

 if (geScriptTrace && !e->fired) {
 e->fired = 1;
 printf("[getv][script] f=%d FIRE keys=0x%x stick=(%d,%d) hold=%d\n",
 frame, e->keys, e->sx, e->sy, e->hold);
 fflush(stdout);
        }

 keys |= e->keys;
 if (e->have_stick) { sx = e->sx; sy = e->sy; have_stick = 1; }
    }

 out->present   = 1;
 out->synthetic = 1;

 if (keys & GE_SK_A)     { out->a = 1; }
 if (keys & GE_SK_B)     { out->b = 1; }
 if (keys & GE_SK_X)     { out->x = 1; }
 if (keys & GE_SK_Y)     { out->y = 1; }
 if (keys & GE_SK_START) { out->start = 1; }
 if (keys & GE_SK_BACK)  { out->back = 1; }
 /* GE_SK_Z drives the RIGHT trigger, not the left.
  *
  * The key names in this parser are the N64's, and on the N64 Z is the fire button --
  * bondview2.c picks `shootButtons = Z_TRIG` for every control style except KISSY and
  * GOODNIGHT. But this harness emits a *gamepad* state, which port_os.c then maps to N64
  * buttons, and it binds GE_ACT_FIRE to GE_SRC_RT (port_os.c:467) while AIM takes the left
  * trigger. Wiring "Z" to ltrigger therefore aimed instead of firing.
  *
  * No key in this parser reached fire at all, so a scripted run could walk, open menus and
  * aim but never shoot. The symptom is `trigger_down` stuck at 0 with a loaded weapon in
  * hand. */
 if (keys & GE_SK_Z)     { out->rtrigger = 1; out->rt_raw = 32767; }
 if (keys & GE_SK_RT)    { out->rtrigger = 1; out->rt_raw = 32767; }
 if (keys & GE_SK_LT)    { out->ltrigger = 1; out->lt_raw = 32767; }
 if (keys & GE_SK_L)     { out->lshoulder = 1; }
 if (keys & GE_SK_R)     { out->rshoulder = 1; }
 if (keys & GE_SK_DU)    { out->dup = 1; }
 if (keys & GE_SK_DD)    { out->ddown = 1; }
 if (keys & GE_SK_DL)    { out->dleft = 1; }
 if (keys & GE_SK_DR)    { out->dright = 1; }

    /* C buttons ride the right stick, which port_os.c puts through a Schmitt trigger
     * (on above 12000, off below 8000). Park well past the on threshold. */
 if (keys & GE_SK_CU) { out->ry = -30000; }
 if (keys & GE_SK_CD) { out->ry =  30000; }
 if (keys & GE_SK_CL) { out->rx = -30000; }
 if (keys & GE_SK_CR) { out->rx =  30000; }

 if (have_stick) {
 out->lx = geScriptAxis(sx);
 out->ly = -geScriptAxis(sy);    /* SDL +Y is DOWN; the script speaks N64 */
    }
}

/* ---- GETV_FRONTTRACE: watch the front-end from outside the front-end ------
 *
 * These are front.c globals read from the port layer. Adding printfs to
 * front.c costs a full `build_sim.sh lib` per iteration; reading the same variables
 * from here costs only `build_sim.sh port`. Nothing is written.
 *
 * `MENU` is a plain C enum with values 0..25, so it is int-sized on this ABI. It is
 * declared `int` here to keep front.h (and the PR/ headers it drags in) out of a
 * translation unit that includes SDL. If MENU ever gains an explicit underlying type,
 * this breaks loudly at link time rather than silently.
 */
extern int current_menu;
extern int menu_update;
extern int selected_folder_num;
extern int briefingpage;
extern int current_menu_briefing_page;
extern float cursor_h_pos;
extern float cursor_v_pos;

static int geFrontTrace(void)
{
 static int on = -1;
 if (on < 0) {
 const char *s = getenv("GETV_FRONTTRACE");
 on = (s != NULL && *s != '\0') ? atoi(s) : 0;
    }
 return on;
}

static void geFrontTraceTick(int frame)
{
 static int last_menu = -12345, last_update = -12345, last_folder = -12345;
 static int last_page = -12345, last_brief = -12345;
 int level = geFrontTrace();
 int changed;

 if (level <= 0) { return; }

 changed = (current_menu != last_menu) || (menu_update != last_update)
           || (selected_folder_num != last_folder) || (briefingpage != last_page)
           || (current_menu_briefing_page != last_brief);

 if (!changed && (level < 2) && (frame % 60) != 0) { return; }

 last_menu = current_menu; last_update = menu_update;
 last_folder = selected_folder_num; last_page = briefingpage;
 last_brief = current_menu_briefing_page;

 printf("[getv][front] f=%d menu=%d update=%d folder=%d briefpage=%d bpage=%d ""cursor=(%.1f,%.1f)%s\n",
 frame, current_menu, menu_update, selected_folder_num, briefingpage,
 current_menu_briefing_page, cursor_h_pos, cursor_v_pos,
 changed ? "<-- CHANGED" : "");
 fflush(stdout);
}

int gePortInputDebugLevel(void)
{
 static int level = -1;

 if (level < 0) {
 const char *s = getenv("GETV_INPUT_DEBUG");
 level = (s != NULL) ? atoi(s) : 0;
 if (level < 0) { level = 0; }
    }
 return level;
}

/* GETV_PADS=N - force N ports to report connected regardless of what enumerated.
 *
 * This exists because the four-controller path cannot otherwise be exercised: no
 * simulator produces a second pad and there are rarely four physical pads to plug in.
 * It is the same shape as gunfire.c's GETV_GUN_AUTOFIRE - drive the subsystem from
 * inside the process so the thing under test is the code path, not the peripheral.
 *
 * A forced port with no device behind it reads as present-and-idle, or, with
 * GETV_PAD_SYNTH=1, as a deterministic moving pad (see geSynthState). Clamped to
 * GE_PORT_MAX_PADS; 0 or unset means "report only what is really there". */
static int gePortForcedPads(void)
{
 static int n = -1;

 if (n < 0) {
 const char *s = getenv("GETV_PADS");
 n = (s != NULL && *s != '\0') ? atoi(s) : 0;
 if (n < 0) { n = 0; }
 if (n > GE_PORT_MAX_PADS) { n = GE_PORT_MAX_PADS; }
    }
 return n;
}

static int gePortSynthEnabled(void)
{
 static int on = -1;

 if (on < 0) {
 const char *s = getenv("GETV_PAD_SYNTH");
 on = (s != NULL && *s != '\0') ? (atoi(s) != 0) : 0;
    }
 return on;
}

const char *gePortInputPadName(int port)
{
 const char *n;

 if (port < 0 || port >= GE_PORT_MAX_PADS) {
 return "none";
    }
 if (gePads[port] == NULL) {
 return (port < gePortForcedPads()) ? "synthetic" : "none";
    }
 n = SDL_GameControllerName(gePads[port]);
 return (n != NULL) ? n : "unnamed";
}

/* "Real gamepad" = something an FPS can actually be played on: two analogue sticks
 * and shoulder buttons. The Siri Remote has neither.
 *
 * Ask SDL what the device has; do not pattern-match the product name. Name matching
 * ("Remote", "Siri") breaks the moment Apple renames the accessory or a localised name
 * comes back, and it fails in the dangerous direction: an unrecognised remote gets
 * treated as a gamepad and takes player 1 again. HasAxis/HasButton is answered from the
 * controller's own SDL mapping, so it is true of the hardware and not of a string. */
static int geControllerIsReal(SDL_GameController *gc)
{
 if (gc == NULL) {
 return 0;
    }
 return SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)
        && SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)
        && SDL_GameControllerHasAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX)
        && SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_A)
        && SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_B)
        && SDL_GameControllerHasButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
}

static SDL_JoystickID geInstanceOf(SDL_GameController *gc)
{
 SDL_Joystick *js = (gc != NULL) ? SDL_GameControllerGetJoystick(gc) : NULL;

 return (js != NULL) ? SDL_JoystickInstanceID(js) : -1;
}

static int geIsOpenAlready(SDL_JoystickID id)
{
 int i;

 if (id < 0) {
 return 0;
    }
 for (i = 0; i < GE_PORT_MAX_PADS; i++) {
 if (gePads[i] != NULL && geInstanceOf(gePads[i]) == id) {
 return 1;
        }
    }
 return 0;
}

static int geIsRejected(SDL_JoystickID id)
{
 int i;

 for (i = 0; i < geRejectCount; i++) {
 if (geRejected[i] == id) {
 return 1;
        }
    }
 return 0;
}

static void geRejectRemember(SDL_JoystickID id)
{
 if (id < 0 || geIsRejected(id) || geRejectCount >= GE_REJECT_MAX) {
 return;
    }
 geRejected[geRejectCount++] = id;
}

/* Close a port and slide the ports above it down, so the occupied ports stay
 * contiguous from 0. Slot 0 losing its pad must not strand player 2 in port 1 -
 * joyGetControllerCount() would then answer 0. */
static void geClosePort(int port)
{
 int i;

 if (port < 0 || port >= GE_PORT_MAX_PADS || gePads[port] == NULL) {
 return;
    }
 SDL_GameControllerClose(gePads[port]);
 gePads[port]   = NULL;
 gePadReal[port] = 0;

 for (i = port; i < GE_PORT_MAX_PADS - 1; i++) {
 gePads[i]    = gePads[i + 1];
 gePadReal[i] = gePadReal[i + 1];
    }
 gePads[GE_PORT_MAX_PADS - 1]    = NULL;
 gePadReal[GE_PORT_MAX_PADS - 1] = 0;

 gePadCount = 0;
 for (i = 0; i < GE_PORT_MAX_PADS; i++) {
 if (gePads[i] != NULL) { gePadCount++; }
    }
}

/* ---- gamepad PROFILE ------------------------------------------------------------
 *
 * See the long note in port_input.h: this resolves LABELS, not bindings. SDL's face
 * constants are positional, so nothing about gameplay depends on getting this right --
 * only what a prompt would print.
 *
 * Auto-detection uses SDL_GameControllerGetType(), which SDL derives from its own
 * controller database (USB VID/PID plus the mapping string) and therefore already
 * knows for anything it recognises as a GameController rather than a raw joystick.
 * GETV_GAMEPAD overrides it for a pad SDL mis-identifies -- most often a third-party
 * or adapter-fronted device that reports as a generic X-input pad.
 *
 * Resolved per port, not globally: nothing stops a player pairing an Xbox pad and a
 * DualSense, and a single global profile would mislabel one of them. The environment
 * override, being a single value, applies to every port -- the documented cost of
 * having one key.
 */
static int geProfileOverride(void)
{
 static int ov = -2;                 /* -2 = unread, -1 = auto */
 if (ov == -2) {
 const char *e = getenv("GETV_GAMEPAD");
 ov = -1;
 if (e != NULL && *e != '\0') {
 if      (SDL_strcasecmp(e, "xbox")        == 0) { ov = GE_PAD_XBOX; }
 else if (SDL_strcasecmp(e, "playstation") == 0) { ov = GE_PAD_PLAYSTATION; }
 else if (SDL_strcasecmp(e, "ps")          == 0) { ov = GE_PAD_PLAYSTATION; }
 else if (SDL_strcasecmp(e, "switch")      == 0) { ov = GE_PAD_SWITCH; }
 else if (SDL_strcasecmp(e, "nintendo")    == 0) { ov = GE_PAD_SWITCH; }
 else if (SDL_strcasecmp(e, "generic")     == 0) { ov = GE_PAD_GENERIC; }
 else if (SDL_strcasecmp(e, "auto")        == 0) { ov = -1; }
 else {
 printf("[getv] input: GETV_GAMEPAD=\"%s\" not recognised -- expected ""xbox, playstation, switch, generic or auto; using auto\n", e);
            }
        }
    }
 return ov;
}

int gePortPadProfile(int port)
{
 int ov = geProfileOverride();

 if (ov >= 0) { return ov; }

 if (port >= 0 && port < GE_PORT_MAX_PADS && gePads[port] != NULL) {
 switch (SDL_GameControllerGetType(gePads[port])) {
 case SDL_CONTROLLER_TYPE_XBOX360:
 case SDL_CONTROLLER_TYPE_XBOXONE:
 return GE_PAD_XBOX;
 case SDL_CONTROLLER_TYPE_PS3:
 case SDL_CONTROLLER_TYPE_PS4:
 case SDL_CONTROLLER_TYPE_PS5:
 return GE_PAD_PLAYSTATION;
 case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
 return GE_PAD_SWITCH;
 default:
 break;
        }
    }
 return GE_PAD_GENERIC;
}

/* Indexed [profile][glyph]. GENERIC uses positional names because inventing labels for
 * an unknown pad is how a prompt ends up lying. */
static const char *const GE_GLYPHS[4][GE_GLYPH_MAX] = {
    /* GE_PAD_GENERIC     */
    { "Down", "Right", "Left", "Up", "L1", "R1", "L2", "R2",
 "Start", "Select", "L3", "R3" },
    /* GE_PAD_XBOX        */
    { "A", "B", "X", "Y", "LB", "RB", "LT", "RT",
 "Menu", "View", "LS", "RS" },
    /* GE_PAD_PLAYSTATION */
    { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2",
 "Options", "Create", "L3", "R3" },
    /* GE_PAD_SWITCH -- note SOUTH is "B" and EAST is "A". Nintendo's labels are
     * mirrored relative to Xbox, and SDL reports the position, so this row is where
     * that difference is resolved. Getting it backwards is a common button-prompt bug
     * on PC ports. */
    { "B", "A", "Y", "X", "L", "R", "ZL", "ZR",
 "+", "-", "L3", "R3" },
};

const char *gePortPadGlyph(int port, int glyph)
{
 int p = gePortPadProfile(port);

 if (glyph < 0 || glyph >= GE_GLYPH_MAX) { return "?"; }
 if (p < 0 || p > GE_PAD_SWITCH)         { p = GE_PAD_GENERIC; }
 return GE_GLYPHS[p][glyph];
}

static const char *geProfileName(int p)
{
 switch (p) {
 case GE_PAD_XBOX: return "xbox";
 case GE_PAD_PLAYSTATION: return "playstation";
 case GE_PAD_SWITCH: return "switch";
 default: return "generic";
    }
}

static void geAssignPort(SDL_GameController *gc, int real)
{
 if (gc == NULL || gePadCount >= GE_PORT_MAX_PADS) {
 return;
    }
 gePads[gePadCount]    = gc;
 gePadReal[gePadCount] = real;
 printf("[getv] input: controller -> N64 port %d -- %s (real_gamepad=%d)\n",
 gePadCount, gePortInputPadName(gePadCount), real);
 printf("[getv] input: port %d profile=%s%s -- face buttons %s/%s/%s/%s ""(S/E/W/N), triggers %s/%s\n",
 gePadCount, geProfileName(gePortPadProfile(gePadCount)),
           (geProfileOverride() >= 0) ? " (GETV_GAMEPAD)" : " (auto)",
 gePortPadGlyph(gePadCount, GE_GLYPH_SOUTH),
 gePortPadGlyph(gePadCount, GE_GLYPH_EAST),
 gePortPadGlyph(gePadCount, GE_GLYPH_WEST),
 gePortPadGlyph(gePadCount, GE_GLYPH_NORTH),
 gePortPadGlyph(gePadCount, GE_GLYPH_LT),
 gePortPadGlyph(gePadCount, GE_GLYPH_RT));
 if (!real) {
 printf("[getv] input: no sticks/shoulders on this device (Siri Remote?) -- ""menus only; will switch to a gamepad when one appears\n");
    }
 gePadCount++;
}

static void gePortInputEnsure(void)
{
 int i;
 int n;
 int anyReal = 0;

 if (!geSubsysReady) {
        /* Before SDL_INIT_GAMECONTROLLER, not after - see (1) above. */
 SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK, "0");
 if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
 printf("[getv] input: SDL_INIT_GAMECONTROLLER failed: %s\n", SDL_GetError());
        }
 geSubsysReady = 1;
    }

    /* 1. Drop anything that unplugged, compacting as we go. Walk downwards so the
     * shift performed by geClosePort() cannot skip an entry. */
 for (i = GE_PORT_MAX_PADS - 1; i >= 0; i--) {
 if (gePads[i] != NULL && !SDL_GameControllerGetAttached(gePads[i])) {
 printf("[getv] input: controller detached from port %d (%s)\n",
 i, gePortInputPadName(i));
 geClosePort(i);
        }
    }

 for (i = 0; i < gePadCount; i++) {
 if (gePadReal[i]) { anyReal = 1; }
    }

 n = SDL_NumJoysticks();

    /* Report on a timer, not once. A pad that enumerates late is the expected case on
     * tvOS, so a single line at startup cannot distinguish "never arrived" from
     * "arrived and we stopped looking". Every 2s, 8 times. */
    {
 static Uint32 last = 0;
 static int said = 0;
 Uint32 now = SDL_GetTicks();

 if (said < 8 && (last == 0 || now - last > 2000)) {
 int j, ngc = 0;
 for (j = 0; j < n; j++) {
 if (SDL_IsGameController(j)) { ngc++; }
            }
 last = now;
 said++;
 printf("[getv] input: poll live @%ums -- joysticks=%d gamecontrollers=%d ""ports=%d [%s|%s|%s|%s] forced=%d synth=%d\n",
                   (unsigned)now, n, ngc, gePadCount,
 gePortInputPadName(0), gePortInputPadName(1),
 gePortInputPadName(2), gePortInputPadName(3),
 gePortForcedPads(), gePortSynthEnabled());
        }
    }

    /* 2. Nothing new can be there if every enumerated device is already open. This is
     * the common steady state and it costs one SDL call. */
 if (gePadCount >= GE_PORT_MAX_PADS || n <= gePadCount) {
 return;
    }

 for (i = 0; i < n && gePadCount < GE_PORT_MAX_PADS; i++) {
 SDL_GameController *cand;
 SDL_JoystickID id;
 int real;

 if (!SDL_IsGameController(i)) {
 continue;
        }
 id = SDL_JoystickGetDeviceInstanceID(i);
 if (geIsOpenAlready(id)) {
 continue;
        }
        /* Skip a known stickless device UNLESS we have nothing playable at all, in
         * which case it is the menus-only fallback for player 1. */
 if (anyReal && geIsRejected(id)) {
 continue;
        }

        /* Opening is the only way to ask about axes/buttons, so open, classify, and
         * close again unless this one is taken. Opening a controller twice is legal in
         * SDL2 (it refcounts), and the loser is closed immediately. */
 cand = SDL_GameControllerOpen(i);
 if (cand == NULL) {
 continue;
        }
 real = geControllerIsReal(cand);

 if (real) {
            /* The upgrade, preserved from the one-pad path: a stickless device
             * holding port 0 is evicted so the real pad becomes player 1. */
 if (gePadCount > 0 && !gePadReal[0]) {
 printf("[getv] input: upgrading port 0 from '%s' to a real gamepad\n",
 gePortInputPadName(0));
 geRejectRemember(geInstanceOf(gePads[0]));
 geClosePort(0);
            }
 geAssignPort(cand, 1);
 anyReal = 1;
 continue;
        }

        /* Stickless. Only ever acceptable as port 0, and only with nothing else. */
 if (gePadCount == 0 && !anyReal) {
 geAssignPort(cand, 0);
 continue;
        }
 geRejectRemember(id);
 SDL_GameControllerClose(cand);
    }
}

int gePortInputPadCount(void)
{
 int forced;

    /* Pump SDL's event queue here rather than relying on Fast3D's handle_events. On
     * tvOS an MFi pad is discovered through GameController framework notifications
     * delivered on the main run loop, and SDL only turns those into
     * SDL_CONTROLLERDEVICEADDED (and updates SDL_NumJoysticks) while it is pumping.
     * The harness never returns to UIKit's run loop -- bossMainloop() loops forever --
     * so the only pumping that happens is whatever this port does explicitly. */
 SDL_PumpEvents();
 gePortInputEnsure();

 forced = gePortForcedPads();
 return (forced > gePadCount) ? forced : gePadCount;
}

/* Deterministic synthetic pad, so ports 1-3 can be exercised with no hardware.
 *
 * Each port gets a different phase, so the decoded N64 stick_x differs per port on
 * every frame. A log showing four identical ports would prove the plumbing runs but not
 * that the ports are independently addressable, which is the property being tested. The
 * button is distinct per port for the same reason. */
static void geSynthState(int port, struct GePadState *out)
{
 const int period = 180;
 int phase, tri;

    /* The frame tick must not live here behind `if (port == 0)`. Port 0 is the one
     * port a real device normally occupies, so geSynthState(0, ...) would never be
     * called, the counter would never advance, and every synthetic port would emit a
     * constant stick for the whole run -- a frozen value that looks like working
     * plumbing. The tick belongs with the per-frame poll, not with the synthesis. */
 phase = (geSynthFrame + port * (period / GE_PORT_MAX_PADS)) % period;
 if (phase < period / 2) {
 tri = -20000 + (phase * 40000) / (period / 2);
    } else {
 tri =  20000 - ((phase - period / 2) * 40000) / (period / 2);
    }

 out->present      = 1;
 out->real_gamepad = 1;
 out->synthetic    = 1;
 out->lx = tri;
 out->ly = -16000;           /* SDL +Y is down, so this is "forward" */

 switch (port) {
 case 0: out->a = 1; break;
 case 1: out->b = 1; break;
 case 2: out->lshoulder = 1; break;
 default: out->rshoulder = 1; break;
    }
}


#ifdef GE_PLATFORM_DESKTOP
/* ===========================================================================
 * Keyboard as port 0 -- macOS only (build_mac.sh is the only thing that defines
 * GE_PLATFORM_DESKTOP; the tvOS device and simulator builds never see any of this).
 *
 * The Apple TV has no keyboard and the port otherwise assumes a gamepad, so
 * `gePads[0] == NULL` means "port 0 is dead" unless GETV_PADS or GETV_SCRIPT forces it.
 * On a Mac that would make a freshly built binary unplayable for anyone without an
 * MFi/DualSense/Xbox pad in reach.
 *
 * Injection happens into `struct GePadState`, exactly like GETV_SCRIPT: at the device
 * side, upstream of gePortDecodePad() in port_os.c. Everything downstream stays the
 * production path -- the N64 bit mapping, the stick rescale, the C-button Schmitt
 * triggers, osContGetReadData(), joy.c's 20-deep sample ring and its
 * `buttonspressed |= cur & ~prev` edge detector. A keypress is indistinguishable from a
 * thumb on a stick, so nothing observed through the keyboard exercises a different code
 * path than a gamepad would.
 *
 * or semantics, never clearing. A gamepad and the keyboard can both be live; whichever
 * is held wins. That also means a real pad on port 0 is never degraded by this.
 *
 * move W A S D            -> left stick
 * look arrow keys         -> right stick (port_os.c turns it into C-buttons)
 * fire SPACE or LEFT-CTRL -> Z trigger
 * A / use E or RETURN        -> A
 * B / aim Q                  -> B
 * crouch/L R Z / X              -> L and R shoulders
 * Start return(kp) or tab  -> start
 * d-pad I J K L
 *
 * GETV_KEYBOARD=0 disables it entirely.
 * =========================================================================== */
/* ---------------------------------------------------------------- mouse look
 *
 * The most-requested feature this port did not have. Off by default, because it is a
 * Phase 2 "modernise" change and is actively wrong for a faithful run -- the GoldenEye+
 * profile is where it belongs.
 *
 * How it works, and the honest limitation. The game reads looking from the right stick,
 * which is a *rate* control: a held stick turns continuously. A mouse is a *displacement*
 * control: moving it two inches should turn a fixed amount regardless of how long that
 * took. Mapping mouse delta onto the stick axes, which is what happens below, gives a very
 * usable result and is what most ports start with, but it is not the same thing. A stick
 * value derived from this frame's delta still turns for the whole frame, so fast flicks
 * overshoot slightly and very slow movement can quantise.
 *
 * Doing it properly means injecting into yaw and pitch directly, which needs the player,
 * camera and weapon orientations separated -- the same split third person and free camera
 * need. That is the right next step and it is not attempted here: this gets
 * mouse look working and playable without touching the game's movement code at all.
 *
 * Perfect Dark's port solved the same problem and is MIT with attribution (see
 * docs/REUSE_AUDIT.md). Nothing is copied from it here -- this is small enough not to need
 * to be -- but its separation of orientation is the model for the proper fix.
 */
static int geKeyboardIdle(void);   /* defined below; the mouse must idle for the same runs */

static int geMouseEnabled(void)
{
 static int on = -1;
 if (on < 0) {
 const char *s = getenv("GETV_MOUSE");
        /* Default on. Nearly everyone has a keyboard and mouse; a gamepad is the minority
         * case, so the majority should not have to find a setting before the game is
         * playable. GETV_MOUSE=0 turns it off, and a connected pad keeps working either way
         * -- the two are ORed, not exclusive. */
 on = (s != NULL && *s != '\0') ? (atoi(s) != 0) : 1;
 if (on) {
 SDL_SetRelativeMouseMode(SDL_TRUE);
 printf("[getv] input: mouse look ON (left button fires, right aims, ESC releases "
                   "the cursor; GETV_MOUSE_SENS to tune, GETV_MOUSE_INVERT=1 to invert Y, "
                   "GETV_MOUSE=0 to disable)\n");
 fflush(stdout);
        }
    }
 return on;
}

static int geMouseSens(void)
{
 static int v = -1;
 if (v < 0) {
 const char *s = getenv("GETV_MOUSE_SENS");
 v = (s != NULL && *s != '\0') ? atoi(s) : 100;   /* percent */
 if (v < 1)    v = 1;
 if (v > 1000) v = 1000;
    }
 return v;
}

static int geMouseInvert(void)
{
 static int v = -1;
 if (v < 0) {
 const char *s = getenv("GETV_MOUSE_INVERT");
 v = (s != NULL && *s != '\0') ? (atoi(s) != 0) : 0;
    }
 return v;
}

/* GE_MOUSE_COUNTS_FULL, the deadzone and the backlog cap all live in ge_mouse_accum.h now,
 * alongside the arithmetic that reads them. 21 counts per full-scale deflection was picked off
 * the measured sweep in docs/MOUSE.md rather than by feel; the old 220 was set against nothing
 * and needed roughly a metre of desk for a 180 degree turn. */

/* Motion the stick could not express yet. Held here rather than inside geMouseAccumulate so
 * that tests can run independent sequences without one bleeding into the next. */
static long ge_mouse_pend_x = 0;
static long ge_mouse_pend_y = 0;

static void geMousePoll(int port, struct GePadState *out)
{
 int dx = 0, dy = 0;
 Uint32 mb = 0;
 int sens;
 int selftest = 0;
 int selftest_y = 0;

    /* Idle on a measurement run, for a stronger reason than the keyboard has: relative mode
     * hides and locks the cursor system-wide, so an automated run would steal the pointer
     * from whatever the user was actually doing. geKeyboardIdle() is on by default whenever
     * GETV_EXIT_FRAME is set, which is exactly the "this is a script, not a play session"
     * signal wanted here. Checked before geMouseEnabled() so the capture never happens. */
    /* GETV_MOUSE_SELFTEST=<counts>: pretend the mouse moves this many counts right every
     * frame, and take that straight to the scaling below.
     *
     * ahead of both the idle check and the capture, for two reasons. It has to
     * run under GETV_EXIT_FRAME, which is what makes two runs comparable, and that is exactly
     * when the idle gate is on. And it must never call SDL_SetRelativeMouseMode, because a
     * measurement run has no business hiding and locking the pointer of whoever is using the
     * machine. This is the only way to put a number on "how far must I sweep to turn 180
     * degrees" without a hand on the mouse. */
    {
 static int st = -2;
 if (st == -2) { const char *e = getenv("GETV_MOUSE_SELFTEST"); st = (e && *e) ? atoi(e) : 0; }
 if (st != 0 && port == 0) {
 selftest = st;
        }
        /* Vertical twin, so pitch can be measured on its own rather than assumed to behave
         * like yaw. It does not: pitch is clamped by the game and yaw is not. */
        {
 static int sty = -2;
 if (sty == -2) { const char *e = getenv("GETV_MOUSE_SELFTEST_Y"); sty = (e && *e) ? atoi(e) : 0; }
 if (sty != 0 && port == 0) { selftest_y = sty; selftest = (selftest != 0) ? selftest : -1; }
        }
    }

 if (selftest == 0 && (port != 0 || geKeyboardIdle() || !geMouseEnabled())) {
 return;
    }

    /* Relative mode keeps delivering motion with the cursor hidden and locked, so the
     * pointer cannot wander onto another monitor mid-firefight. SDL_PumpEvents() has
     * already run this frame, the same reason the keyboard read relies on. */
    /* ESC releases the cursor, and pressing it again recaptures.
     *
     * This matters more with the mouse on by default: relative mode hides and locks the
     * pointer, and a player who cannot reach their other windows will read that as the game
     * having hung. Edge-triggered, so holding ESC does not flap the mode every frame. ESC is
     * otherwise unbound in this port -- the game's pause is START, which the keyboard maps
     * to TAB. */
    if (!selftest) {
 static int prev_esc = 0;
 const Uint8 *ks = SDL_GetKeyboardState(NULL);
 int esc = (ks != NULL && ks[SDL_SCANCODE_ESCAPE]) ? 1 : 0;
 if (esc && !prev_esc) {
 SDL_bool now = SDL_GetRelativeMouseMode();
 SDL_SetRelativeMouseMode(now ? SDL_FALSE : SDL_TRUE);
 printf("[getv] input: mouse %s\n", now ? "released (ESC to recapture)" : "captured");
 fflush(stdout);
        }
 prev_esc = esc;
 if (!SDL_GetRelativeMouseMode()) { return; }   /* released: no look, no clicks */
    }

 if (selftest) { dx = (selftest == -1) ? 0 : selftest; dy = selftest_y; }
 else          { mb = SDL_GetRelativeMouseState(&dx, &dy); }
 sens = geMouseSens();

    /* GETV_MOUSE_SELFTEST=<counts>: pretend the mouse moves this many counts right every
     * frame. The only way to put a number on "how far must I sweep to turn 180 degrees"
     * without a hand on the mouse, and the only way to tune the gain against a measurement
     * rather than against somebody's impression of it. */

 if (dx != 0 || dy != 0) {
 long rx, ry;

 if (geMouseInvert()) { dy = -dy; }

        /* Scale, carry and deadzone, in ge_mouse_accum.h.
         *
         * Lifted out of here so it can be tested without SDL, a window, or a pointer taken
         * from whoever is using the machine. The move is proved bit-identical against the code
         * it replaced over 16,800 swept calls in tests/test_mouse.c. That care is warranted:
         * of the first three attempts at this input path, two made the mouse worse and one
         * stopped it moving at all. The reasoning for each step lives in that header. */
 geMouseAccumulate((long) dx, (long) dy, (long) sens,
                          &ge_mouse_pend_x, &ge_mouse_pend_y, &rx, &ry);

        /* GETV_INPUT_DEBUG=2: what the mouse is actually handing the game.
         *
         * Added because GETV_MOUSE_SELFTEST could not be shown to do anything. Two runs of
         * DAM, one with the selftest off and one at 30 counts a frame, produced logs that
         * differed only in the heap base and the millisecond timings -- and there was no way
         * to tell whether the selftest was dead or whether the run simply could not show it,
         * since the scripted player walks into geometry at f=270 and stays pinned there for
         * the remaining 630 frames. A knob whose effect cannot be observed is not a
         * measurement tool, so this prints the numbers directly. */
 if (gePortInputDebugLevel() >= 2) {
 static unsigned long mf = 0;
 mf++;
 if ((mf % 60) == 1) {
 printf("[getv][mouse] n=%lu dx=%d dy=%d sens=%d -> rx=%ld ry=%ld carry=(%ld,%ld)\n",
 mf, dx, dy, sens, rx, ry, ge_mouse_pend_x, ge_mouse_pend_y);
 fflush(stdout);
            }
        }

 out->rx = (int) rx;
 out->ry = (int) ry;
 out->present      = 1;
 out->real_gamepad = 1;
    }

 if (mb & SDL_BUTTON(SDL_BUTTON_LEFT))  { out->rtrigger = 1; out->rt_raw = 32767;
 out->present = 1; out->real_gamepad = 1; }
 if (mb & SDL_BUTTON(SDL_BUTTON_RIGHT)) { out->ltrigger = 1; out->lt_raw = 32767;
 out->present = 1; out->real_gamepad = 1; }
}

static int geKeyboardEnabled(void)
{
 static int on = -1;
 if (on < 0) {
 const char *s = getenv("GETV_KEYBOARD");
 on = (s != NULL && *s != '\0') ? (atoi(s) != 0) : 1;   /* default ON */
 if (on) {
 printf("[getv] input: keyboard bound to N64 port 0 ""(WASD move, arrows look, SPACE/LCTRL fire, E or F use, Q aim, "
 "R weapon, TAB start; ""GETV_KEYBOARD=0 to disable)\n");
 fflush(stdout);
        }
    }
 return on;
}

/* Full deflection for a digital key. Well past every threshold port_os.c applies:
 * the C-button Schmitt trigger turns on above 12000, and the stick rescale saturates
 * long before 32767. */
#define GE_KB_FULL 32000

/* GETV_KEYBOARD_UNFOCUSED=1 reads the key state whether or not our window has keyboard
 * focus. Kept as an A/B switch, not because it is ever the right setting. */
static int geKeyboardUnfocusedAllowed(void)
{
 static int on = -1;
 if (on < 0) {
 const char *s = getenv("GETV_KEYBOARD_UNFOCUSED");
 on = (s != NULL && *s != '\0') ? (atoi(s) != 0) : 0;
    }
 return on;
}

/* Idle keyboard for automated runs.
 *
 * The Mac window takes keyboard focus when it opens (`SDL_GetKeyboardFocus() != NULL`
 * on every sampled frame of a plain `GETV_STAGE=33` run), so anything typed while an
 * unattended run is up is delivered to the game as input. On DAM with
 * GETV_INPUT_DEBUG=2, two of six identical launches picked up phantom presses from the
 * keyboard pad with no gamepad attached and no script running: `btn=2000 Z T(32767,0)`
 * for one frame (SPACE/LCTRL) and `btn=1000 St` for four (TAB).
 *
 * That matters because bondview2.c's intro-cutscene skip is
 * `buttons & ~oldbuttons & (A|B|Z|START|R|L)`, so one stray edge aborts the level's
 * opening cinema: those two runs left CAMERAMODE_INTRO at t=12 and t=111 instead of the
 * scripted t=480, i.e. an unattended run can silently capture a different camera than
 * the one it asked for.
 *
 * The keyboard is made idle, not disabled. Turning it off would drop
 * joyGetControllerCount() to 0, and front.c:1493 sends the front-end to
 * MENU_NO_CONTROLLERS at the 4-second mark -- a terminal state whose init, update and
 * interface are all empty `return;` with no frontChangeMenu anywhere, which would wedge
 * the run rather than stabilise it. So port 0 stays present and reports "nothing held".
 *
 * On by default whenever GETV_EXIT_FRAME is set, which marks an automated run rather
 * than a play session. A plain `./build_mac.sh run` is unaffected and stays playable.
 * GETV_KEYBOARD_IDLE=1 force idle GETV_KEYBOARD_IDLE=0 force live
 */
static int geKeyboardIdle(void)
{
 static int on = -1;
 if (on < 0) {
 const char *s = getenv("GETV_KEYBOARD_IDLE");
 if (s != NULL && *s != '\0') {
 on = (atoi(s) != 0);
        } else {
 const char *ef = getenv("GETV_EXIT_FRAME");
 on = (ef != NULL && *ef != '\0' && atoi(ef) > 0);
        }
 if (on) {
 printf("[getv] input: keyboard pad is PRESENT but IDLE for this run ""(measurement run; GETV_KEYBOARD_IDLE=0 to type into it)\n");
 fflush(stdout);
        }
    }
 return on;
}

/* Global so `nm -g` can prove a rebuild landed: a static would be dead-stripped at -O1
 * and `strings` would then report its absence on a binary where it is present. */
unsigned long ge_kbd_unfocused_frames = 0;
unsigned long ge_kbd_unfocused_keys   = 0;
unsigned long ge_kbd_idle_frames      = 0;

static void geKeyboardApply(int port, struct GePadState *out)
{
 const Uint8 *k;
 int lx = 0, ly = 0, rx = 0, ry = 0;

 if (port != 0 || !geKeyboardEnabled()) {
 return;
    }
    /* SDL_PumpEvents() already ran this frame in gePortInputPadCount(); the key state
     * array is only updated by a pump, so reading it without one returns last frame. */
 k = SDL_GetKeyboardState(NULL);
 if (k == NULL) {
 return;
    }

    /* Focus gate. Without this, keystrokes aimed at another window reach the game.
     *
     * SDL's key-state array is process-global and outlives focus changes: a KEYDOWN
     * delivered while our window was frontmost stays set if the matching KEYUP goes to
     * whatever the user switched to. On DAM with GETV_INPUT_DEBUG=2, two of six
     * identical launches saw phantom presses from the keyboard pad with no gamepad
     * attached and no script running: `btn=2000 Z T(32767,0)` for one frame
     * (space/LCTRL -> Z) and `btn=1000 St` for four (tab -> start).
     *
     * The consequence is not cosmetic. bondview2.c's intro-cutscene skip is
     * `buttons & ~oldbuttons & (A|B|Z|START|R|L)`, so one phantom edge aborts the
     * level's opening cinema: those two runs left CAMERAMODE_INTRO at t=12 and t=111
     * instead of the scripted t=480.
     *
     * SDL_GetKeyboardFocus() returns the window with keyboard focus or NULL. */
    {
 const int focused = (SDL_GetKeyboardFocus() != NULL);
 int i, anykey = 0;
 static unsigned long fr = 0;
 static int dbg = -1;
 if (dbg < 0) { const char *d = getenv("GETV_INPUT_DEBUG"); dbg = (d && *d) ? atoi(d) : 0; }
 fr++;
 if (dbg > 0 && (fr % 120) == 1) {
 printf("[getv][kbd] f=%lu focus=%s (unfocused frames so far=%lu, with a key down=%lu)\n",
 fr, focused ? "YES" : "NO",
 ge_kbd_unfocused_frames, ge_kbd_unfocused_keys);
 fflush(stdout);
        }
 for (i = 0; i < SDL_NUM_SCANCODES; i++) { if (k[i]) { anykey = 1; break; } }

        /* GETV_AIM_SELFTEST=1 holds AIM down, so anything gated behind aim mode can be measured
         * under GETV_EXIT_FRAME without a hand on the keyboard. Same reason as
         * GETV_MOUSE_SELFTEST and GETV_CROUCH_SELFTEST, and it sits ahead of the idle gate for
         * the same reason those do -- a measurement run has the keyboard present but idle, so
         * anything read after that gate is never seen.
         *
         * Written for the crosshair, which gunDrawSight() only draws while aiming: without this
         * there is no way to show GETV_CROSSHAIR_COLOR or GETV_CROSSHAIR_SCALE doing anything on
         * an automated run, and "no crosshair on screen" looks identical to "the setting does
         * nothing". Note that crouch is gated behind aim too (bondview2.c:5484 wants
         * insightaimmode), so GETV_CROUCH_SELFTEST on its own cannot lower Bond either. */
        {
 static long aimst = -2;   /* -2 unread; 0 off; otherwise the frame to start holding from */
 if (aimst == -2) {
 const char *e = getenv("GETV_AIM_SELFTEST");
 aimst = (e && *e && *e != '0') ? atol(e) : 0;
        }
            /* Held from a FRAME, not from the start, and the difference decides whether this
             * works at all. bondview2.c:5643 reads aim one of two ways: as a level
             * (`buttons & aimButtons`) when cur_player_get_aim_control() is 0, or as a TOGGLE
             * on the rising edge (`(buttons & ~oldbuttons) & aimButtons`) otherwise. A hold
             * that starts at frame 0 spends the whole boot and level intro down -- controls
             * are locked for most of it -- so by the time the player has control the button is
             * already pressed, no edge ever arrives, and the toggle never fires. Starting the
             * hold once gameplay is running gives a clean edge and satisfies both readings. */
 if (aimst > 0 && (long)fr >= aimst) { out->ltrigger = 1; out->lt_raw = 32767; }
        }

        /* Both gates mark the port present before bailing. An early `return` here
         * would leave out->present == 0, joyGetControllerCount() would read 0, and
         * front.c:1493 would drop the front-end into MENU_NO_CONTROLLERS, a terminal
         * state with no exit. Present-but-idle is the whole design. */
 if (geKeyboardIdle()) {
 ge_kbd_idle_frames++;
 if (anykey && dbg > 0) {
 printf("[getv][kbd] IGNORED scancode %d -- measurement run, keyboard idle\n", i);
 fflush(stdout);
            }
 out->present = 1;
 out->real_gamepad = 1;
 return;
        }
 if (!focused) {
 ge_kbd_unfocused_frames++;
 if (anykey) {
 ge_kbd_unfocused_keys++;
 printf("[getv][kbd] BLOCKED scancode %d -- window has no keyboard focus ""(GETV_KEYBOARD_UNFOCUSED=1 to allow)\n", i);
 fflush(stdout);
            }
 if (!geKeyboardUnfocusedAllowed()) {
 out->present = 1;
 out->real_gamepad = 1;
 return;
            }
        } else if (anykey && dbg > 0) {
 printf("[getv][kbd] key scancode %d accepted (focused)\n", i);
 fflush(stdout);
        }
    }

 if (k[SDL_SCANCODE_W]) { ly -= GE_KB_FULL; }
 if (k[SDL_SCANCODE_S]) { ly += GE_KB_FULL; }   /* SDL +Y is DOWN */
 if (k[SDL_SCANCODE_A]) { lx -= GE_KB_FULL; }
 if (k[SDL_SCANCODE_D]) { lx += GE_KB_FULL; }

 if (k[SDL_SCANCODE_UP])    { ry -= GE_KB_FULL; }
 if (k[SDL_SCANCODE_DOWN])  { ry += GE_KB_FULL; }
 if (k[SDL_SCANCODE_LEFT])  { rx -= GE_KB_FULL; }
 if (k[SDL_SCANCODE_RIGHT]) { rx += GE_KB_FULL; }

    /* or against whatever a real pad reported: a held stick must not be zeroed by an
     * unpressed key. Only overwrite an axis the keyboard is actually driving. */
 if (lx != 0) { out->lx = lx; }
 if (ly != 0) { out->ly = ly; }
 if (rx != 0) { out->rx = rx; }
 if (ry != 0) { out->ry = ry; }

    /* Space and lctrl are fire, which means the right trigger.
     *
     * This read `ltrigger` and the banner above has always said "SPACE fire", so intent and
     * wiring disagreed: port_os.c:467 binds GE_ACT_FIRE to GE_SRC_RT and gives AIM the left
     * trigger, so pressing space aimed. Keyboard players could walk, use, and aim, and could
     * not shoot. The scripted-input harness had the identical bug for the identical reason --
     * the port's "Z" concept was wired to the left trigger in both places, because on the N64
     * Z *is* fire and the name reads correct at a glance. */
 if (k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_LCTRL]) {
 out->rtrigger = 1;
 out->rt_raw   = 32767;
    }
    /* Q is AIM, as the banner says. */
 if (k[SDL_SCANCODE_Q]) {
 out->ltrigger = 1;
 out->lt_raw   = 32767;
    }
    /* E and F are USE, which is the N64's B button.
     *
     * Nothing on the keyboard set `b` at all, and GE_ACT_USE binds to GE_SRC_B by default
     * (port_os.c:469), so a keyboard player could not open a door, plant a bomb or trip a
     * switch -- every objective in the game runs through that button. E had the pad's A,
     * which is the inventory/weapon-next button, so the key a PC player reaches for to use
     * something cycled the weapon instead. */
    if (k[SDL_SCANCODE_E] || k[SDL_SCANCODE_F])       { out->b = 1; }

    /* RETURN stays on A because every front.c menu branch accepts it to confirm, and R
     * gives weapon-next a key of its own now that E no longer serves it. */
    if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_R])  { out->a = 1; }
 if (k[SDL_SCANCODE_Z])                            { out->lshoulder = 1; }
 if (k[SDL_SCANCODE_X])                            { out->rshoulder = 1; }
 if (k[SDL_SCANCODE_TAB] || k[SDL_SCANCODE_KP_ENTER]) { out->start = 1; }
 if (k[SDL_SCANCODE_BACKSPACE])                    { out->back = 1; }

 if (k[SDL_SCANCODE_I]) { out->dup = 1; }
 if (k[SDL_SCANCODE_K]) { out->ddown = 1; }
 if (k[SDL_SCANCODE_J]) { out->dleft = 1; }
 if (k[SDL_SCANCODE_L]) { out->dright = 1; }

    /* Anything held makes port 0 PRESENT. Without this the game's
     * g_ConnectedControllers bit stays clear and joy.c drops every sample -- the pad
     * would decode perfectly and the game would ignore it. */
 out->present      = 1;
 out->real_gamepad = 1;
}
#endif /* GE_PLATFORM_DESKTOP */

/* ---- a real crouch button -------------------------------------------------------
 *
 * Retail crouch is gated behind aim mode: `bondview2.c:5484` requires `insightaimmode` and
 * stick-down before it will lower Bond, so crouching means holding aim, pushing down, and
 * then releasing aim while staying low. That is faithful and it is genuinely awkward, and
 * it is the sort of thing a native port is for.
 *
 * These report a held key straight to the game, which ORs them alongside the retail
 * condition rather than replacing it -- the original gesture keeps working exactly as it
 * did, and the keys are simply another way in. Off unless GETV_CROUCH_KEY is 1, which it is
 * by default; set it to 0 for faithful-only behaviour.
 *
 * not routed through port_os.c's action table. That table is mid-rework for the
 * per-player bindings, and adding rows to it while that is in progress is how the last
 * merge conflict happened. When the binding work lands these should move onto it as
 * GE_ACT_CROUCH / GE_ACT_STAND.
 */
static int geCrouchKeysEnabled(void)
{
 static int on = -1;
 if (on < 0) {
 const char *e = getenv("GETV_CROUCH_KEY");
 on = (e && *e) ? (*e != '0') : 1;
    }
 return on;
}

int gePortCrouchHeld(void)
{
#ifdef GE_PLATFORM_DESKTOP
 const Uint8 *k;
    /* GETV_CROUCH_SELFTEST=1 holds the key down, so the effect can be measured under
     * GETV_EXIT_FRAME without a hand on the keyboard. Same reason as GETV_MOUSE_SELFTEST,
     * and it has to be ahead of the idle gate for the same reason too. */
    {
 static int st = -1;
 if (st < 0) { const char *e = getenv("GETV_CROUCH_SELFTEST"); st = (e && *e && *e != '0'); }
 if (st) return 1;
    }
 if (!geCrouchKeysEnabled() || geKeyboardIdle() || !geKeyboardEnabled()) return 0;
 k = SDL_GetKeyboardState(NULL);
 if (k == NULL) return 0;
 return (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_LSHIFT]) ? 1 : 0;
#else
 return 0;
#endif
}

int gePortStandHeld(void)
{
#ifdef GE_PLATFORM_DESKTOP
 const Uint8 *k;
 if (!geCrouchKeysEnabled() || geKeyboardIdle() || !geKeyboardEnabled()) return 0;
 k = SDL_GetKeyboardState(NULL);
 if (k == NULL) return 0;
 return k[SDL_SCANCODE_V] ? 1 : 0;
#else
 return 0;
#endif
}

/* GETV_MOVE_SELFTEST=<frame>: hold the left stick forward on every port from that frame.
 * GETV_MOVE_SELFTEST_Y=<counts> sets the axis; default is full forward, positive walks back.
 *
 * From a frame rather than from zero, like GETV_AIM_SELFTEST: controls are locked through the
 * boot and the intro, so a hold starting at zero is already down before the player has control.
 *
 * Wraps the whole poll rather than living in one applier, because with GETV_PADS=2 the two co-op
 * players take different paths -- port 0 is claimed by geKeyboardApply and returns early, port 1
 * reaches geSynthState. Driving only one of them looks exactly like one player being stuck.
 *
 * Forward is negative; the struct documents SDL's +Y down. */
static void geMoveSelftestApply(int port, struct GePadState *out)
{
    static long movest = -2;   /* -2 unread; 0 off; otherwise the frame to start holding from */

    (void) port;
    if (out == NULL) { return; }
    if (movest == -2) {
        const char *e = getenv("GETV_MOVE_SELFTEST");
        movest = (e != NULL && *e != '\0' && *e != '0') ? atol(e) : 0;
    }
    if (movest <= 0 || (long) geSynthFrame < movest) { return; }

    {
        static int ycount = 1;   /* 1 = unread */
        if (ycount == 1) {
            const char *e = getenv("GETV_MOVE_SELFTEST_Y");
            ycount = (e != NULL && *e != '\0') ? atoi(e) : -32000;
        }
        out->present = 1;   /* a port the game believes is absent is never read */
        out->ly      = ycount;
    }
}

static void gePortInputPollPortInner(int port, struct GePadState *out)
{
 SDL_GameController *gc;

 if (out == NULL) {
 return;
    }
 SDL_memset(out, 0, sizeof(*out));
 if (port < 0 || port >= GE_PORT_MAX_PADS) {
 return;
    }

    /* Discovery happens once per frame on port 0; the other three ports read the state
     * that pass established. Pumping four times a frame is harmless but wasteful, and
     * it would let the port set CHANGE half way through a single osContGetReadData(). */
 if (port == 0) {
        (void)gePortInputPadCount();
 geSynthFrame++;
 geFrontTraceTick(geSynthFrame);
    }

 gc = gePads[port];
 if (gc == NULL) {
#ifdef GE_PLATFORM_DESKTOP
        /* The keyboard is its own forced pad, for the same reason a script is: on a Mac
         * it is the default input device, not a fallback, and requiring GETV_PADS=1 to
         * use it would make a fresh build look input-dead.
         *
         * This must not return here. `geKeyboardApply(); if (out->present) return;`
         * would shadow the GETV_SCRIPT branch below completely, because the keyboard
         * marks port 0 present unconditionally while enabled. The script then parses,
         * announces itself, and never fires -- a silently disabled harness that still
         * prints its startup banner, which makes an input test look like a null result.
         * Script keeps priority; the keyboard only fills in underneath it. */
 geKeyboardApply(port, out);
        /* Mouse after the keyboard so a moved mouse wins the look axes over held arrow
         * keys, and before the script branch below so a script still overrides both --
         * same priority rule the keyboard follows, for the same reason. */
 geMousePoll(port, out);
#endif
        /* A live script is its own "forced pad": it must work with no hardware and
         * without also having to set GETV_PADS. */
 if (geScriptActive() && port == geScriptPort) {
 out->present = 1;
 out->real_gamepad = 1;
 out->synthetic = 1;
 geScriptApply(port, geSynthFrame, out);
 return;
        }
#ifdef GE_PLATFORM_DESKTOP
        /* Keyboard claimed the port and no script is live -- nothing further to do. */
 if (out->present) {
 return;
        }
#endif
 if (port < gePortForcedPads()) {
 if (gePortSynthEnabled()) {
 geSynthState(port, out);
            } else {
 out->present = 1;       /* forced-present but idle */
 out->synthetic = 1;
            }
        }
 return;
    }

    /* The harness pumps SDL events through Fast3D's handle_events, but that only runs
     * on frames that submit a display list. Updating here keeps input correct on any
     * frame the game polls. */
 if (port == 0) {
 SDL_GameControllerUpdate();
    }

    /* Every read below is safe on a device that lacks the control: SDL returns 0 for
     * an axis or button the open controller has no mapping for. That is what keeps a
     * Siri Remote from crashing anything - it simply reads as an idle pad. */
 out->present      = 1;
 out->real_gamepad = gePadReal[port];

 out->a         = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A);
 out->b         = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B);
 out->x         = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X);
 out->y         = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y);
 out->start     = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START);
 out->back      = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK);
 out->lshoulder = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
 out->rshoulder = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);

 out->dup       = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP);
 out->ddown     = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
 out->dleft     = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
 out->dright    = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

 out->lt_raw    = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
 out->rt_raw    = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
 out->ltrigger  = out->lt_raw > GE_TRIGGER_ON;
 out->rtrigger  = out->rt_raw > GE_TRIGGER_ON;

 out->lx        = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
 out->ly        = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
 out->rx        = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
 out->ry        = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);

    /* The script overlays a real pad too, so a run is reproducible whether or not a
     * controller happens to be attached to the host. OR-only: it never CLEARS a button
     * the human is holding, so a script can be steered out of by hand on the device. */
 geScriptApply(port, geSynthFrame, out);
#ifdef GE_PLATFORM_DESKTOP
 geKeyboardApply(port, out);
#endif
}

/* One entry point: the inner function has six returns and the self-test must survive all of them. */
void gePortInputPollPort(int port, struct GePadState *out)
{
    gePortInputPollPortInner(port, out);
    geMoveSelftestApply(port, out);
}

