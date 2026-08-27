/* The player API. See ge_player_api.h for the design and docs/PLAYER_API.md for the evidence.
 *
 * This file attaches to GoldenEye's own demo-playback hook and owns the four pad structs the
 * game reads each frame. It deliberately contains no policy, no networking and no observation
 * encoding -- those are consumers, and the point of the seam is that they do not have to agree
 * with each other about anything except tick numbers.
 */

/* PR/os.h before any libc string header, and no <string.h> in this translation unit at all.
 * PR/os.h redeclares bcopy/bcmp/bzero with the N64's int-length signatures and declares struct
 * fields literally named `errno`; the two collide in either order. port_os.c carries the same
 * constraint and the same note. */
#include <PR/ultratypes.h>
#include <PR/os.h>

#include <stdio.h>
#include <stdlib.h>

#include "ge_player_api.h"

/* Mirrors src/joy.h:12-14 exactly -- `OSContPad pads[MAXCONTROLLERS]`, MAXCONTROLLERS == 4.
 *
 * Declared here rather than by including the decomp's joy.h because the port layer does not have
 * the decomp's src/ on its include path, and putting it there would drag in headers that fight
 * PR/os.h. The struct is two fields wide and has been stable since 1997; if it ever changes, the
 * static assert below fails at compile time rather than corrupting input at runtime. */
struct contsample {
    OSContPad pads[4];
};

_Static_assert(sizeof(struct contsample) == sizeof(OSContPad) * 4,
               "struct contsample must stay a bare array of four OSContPad -- see src/joy.h:12");

typedef s32 (*ge_contplaybackfunc)(struct contsample *, s32);

extern void joySetPlaybackFunc(ge_contplaybackfunc func, s32 controllercount);
extern void joySetContDataIndex(s32 index);
extern s8   joyGetStickX(s8 contpadnum);
extern s8   joyGetStickY(s8 contpadnum);
extern u16  joyGetButtons(s8 contpadnum, u16 mask);
extern s32  getPlayerCount(void);
extern u32  get_player_control_style(s32 playernum);
extern int  gePortPlayerPos(int idx, f32 *out);

/* Matches CONTROLLER_CONFIG_* in bondconstants.h.
 *
 * This list used to stop at GOODNIGHT, reasoning that the two-controller styles are forced back to
 * HONEY at three or more players (front.c:4800-4803). The rule is real; the conclusion was wrong
 * in the case that matters. The guard is `numplayers >= 3`, so at one or two players a 2.x style
 * survives untouched -- and THIS PORT DEFAULTS TO 2.2 GALORE, so in practice every slot is on one.
 *
 * Naming all nine is not enough on its own: see gePlayerSlotIsDrivable, where treating "two pad"
 * as "cannot be driven" disabled every bot on every level. Two-pad is a routing fact, not a
 * disqualification. */
#define GE_STYLE_HONEY      0   /* 1.1 */
#define GE_STYLE_SOLITARE   1   /* 1.2 */
#define GE_STYLE_KISSY      2   /* 1.3 */
#define GE_STYLE_GOODNIGHT  3   /* 1.4 */
#define GE_STYLE_PLENTY     4   /* 2.1 */
#define GE_STYLE_GALORE     5   /* 2.2 -- this port's default */
#define GE_STYLE_DOMINO     6   /* 2.3 */
#define GE_STYLE_GOODHEAD   7   /* 2.4 */
#define GE_STYLE_CINEMA     8

#define GE_STYLE_IS_TWO_PAD(s) ((s) >= GE_STYLE_PLENTY && (s) <= GE_STYLE_GOODHEAD)

/* ---------------------------------------------------------------- state */

#define GE_QUEUE_LEN 64

struct GeQueued {
    unsigned long  tick;        /* the tick this applies to */
    int            hold;        /* remaining ticks, counted down as it is applied */
    GePlayerInput  in;
    int            used;
};

static struct GeQueued ge_queue[GE_MAX_SLOTS][GE_QUEUE_LEN];
static GeSlotSource    ge_src[GE_MAX_SLOTS];
static GePlayerInput   ge_held[GE_MAX_SLOTS];   /* what is currently applied, for holds */
static int             ge_held_left[GE_MAX_SLOTS];
static unsigned long   ge_tick;
static int             ge_installed;
static int             ge_pinned_delta;
static unsigned int    ge_seed_fp;

static int ge_clampi(int v, int lo, int hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

/* ---------------------------------------------------------------- intent -> N64 bits
 *
 * Which physical bit fires depends on the slot's control style. bondview2.c:5546-5558:
 *
 *     KISSY / GOODNIGHT : shoot = A_BUTTON, aim = Z_TRIG,      inventory = L_TRIG|R_TRIG
 *     everything else   : shoot = Z_TRIG,   aim = L_TRIG|R_TRIG, inventory = A_BUTTON
 *
 * A caller that spoke in N64 bits would therefore be silently wrong for half the styles, and it
 * would present as "the bot cannot shoot" rather than as a mapping error. Hence GE_IN_* naming
 * intent and this function resolving it per slot. */
/* Warn once per slot per intent. A bot posting FIRE sixty times a second must not produce sixty
 * lines a second, but silence is what let the two-controller mismap live -- so it is one line, not
 * none. */
static unsigned int ge_unreach_warned[GE_MAX_SLOTS];

static void ge_warn_unreachable(int slot, unsigned int bit, const char *intent,
                                const char *style_name, const char *why)
{
    if (slot < 0 || slot >= GE_MAX_SLOTS) { return; }
    if (ge_unreach_warned[slot] & bit)    { return; }
    ge_unreach_warned[slot] |= bit;
    printf("[getv][playerapi] slot %d: %s is unreachable on %s -- %s\n",
           slot, intent, style_name, why);
}

static u16 ge_intent_to_buttons(int slot, unsigned int want)
{
    u16 b = 0;
    u32 style = (slot >= 0 && slot < GE_MAX_SLOTS) ? get_player_control_style(slot) : GE_STYLE_HONEY;
    int swapped = (style == GE_STYLE_KISSY || style == GE_STYLE_GOODNIGHT);

    u16 shoot = swapped ? A_BUTTON : Z_TRIG;
    u16 aim   = swapped ? Z_TRIG   : (u16)(L_TRIG | R_TRIG);
    u16 inv   = swapped ? (u16)(L_TRIG | R_TRIG) : A_BUTTON;

    /* THE TWO-CONTROLLER STYLES SPLIT FIRE AND AIM ACROSS TWO PADS, and this function fills one.
     *
     * This is the BUTTON half of the two-pad problem and is separate from the stick half. The
     * playback loop routes the walk axis to whichever pad the engine reads for movement; this
     * decides which BIT an intent becomes on the pad being filled. Both are needed and neither
     * substitutes for the other.
     *
     * On 2.x both intents are Z_TRIG and the difference is which pad carries which. The game reads
     * them into two separate locals (bondview2.c:5345-5359); sp104 becomes insightaimmode at 5365,
     * sp10C becomes triggerOn at 5535:
     *
     *     PLENTY 2.1 / GALORE 2.2  : pad1 Z_TRIG = SHOOT, pad2 Z_TRIG = aim
     *     DOMINO 2.3 / GOODHEAD 2.4: pad1 Z_TRIG = AIM,   pad2 Z_TRIG = shoot
     *
     * So on Domino and Goodhead the plain mapping sends FIRE to pad 1's Z_TRIG and the game reads
     * AIM: insightaimmode goes true and canNaturalTurn = !insightaimmode (5385) goes false, which
     * switches off the yaw computation at 6394. A bot that fires in bursts loses its steering for
     * the length of every burst, and it presents as an intermittent TURNING fault.
     *
     * The half that lives on pad 2 cannot be expressed by filling one pad, so it says so once
     * rather than sending a bit that means something else. */
    if (GE_STYLE_IS_TWO_PAD(style))
    {
        int pad1_is_aim = (style == GE_STYLE_DOMINO || style == GE_STYLE_GOODHEAD);
        const char *sn  = pad1_is_aim ? "a 2.3/2.4 style" : "a 2.1/2.2 style";

        shoot = pad1_is_aim ? 0            : (u16) Z_TRIG;
        aim   = pad1_is_aim ? (u16) Z_TRIG : 0;

        if (pad1_is_aim && (want & GE_IN_FIRE)) {
            ge_warn_unreachable(slot, GE_IN_FIRE, "FIRE", sn,
                                "shoot is on controller 2; sending Z_TRIG here would enter aim "
                                "mode and disable turning");
        }
        if (!pad1_is_aim && (want & GE_IN_AIM)) {
            ge_warn_unreachable(slot, GE_IN_AIM, "AIM", sn,
                                "aim is on controller 2; sending Z_TRIG here would fire");
        }

        /* Inventory stays on A_BUTTON, which is what this function always did. The 1.x
         * shoot/aim/inv locals at 5546-5558 have no counterpart in the 2.x branch, so the correct
         * 2.x inventory bit is genuinely not established. Unchanged and flagged beats invented. */
    }

    if (want & GE_IN_FIRE)        { b |= shoot; }
    if (want & GE_IN_AIM)         { b |= aim; }
    if (want & GE_IN_WEAPON_NEXT) { b |= inv; }
    /* Back-cycle is hold-inventory plus tap-fire; the game has no dedicated button. Synthesising
     * the pair is faithful (bondview2.c:5091-5111) but triggerOn is suppressed while inventory
     * is held (:5447-5450), so it cannot discharge the gun. */
    if (want & GE_IN_WEAPON_PREV) { b |= (u16)(inv | shoot); }

    if (want & GE_IN_USE)         { b |= B_BUTTON; }
    if (want & GE_IN_PAUSE)       { b |= START_BUTTON; }
    if (want & GE_IN_START)       { b |= START_BUTTON; }

    if (want & GE_IN_LOOK_UP)     { b |= U_CBUTTONS; }
    if (want & GE_IN_LOOK_DOWN)   { b |= D_CBUTTONS; }
    if (want & GE_IN_STEP_LEFT)   { b |= L_CBUTTONS; }
    if (want & GE_IN_STEP_RIGHT)  { b |= R_CBUTTONS; }

    if (want & GE_IN_DPAD_UP)     { b |= U_JPAD; }
    if (want & GE_IN_DPAD_DOWN)   { b |= D_JPAD; }
    if (want & GE_IN_DPAD_LEFT)   { b |= L_JPAD; }
    if (want & GE_IN_DPAD_RIGHT)  { b |= R_JPAD; }

    /* Crouch is deliberately absent and that is a property of the game, not an omission. In the
     * two-controller styles crouch is not a button at all: it is controller 2's stick Y crossing
     * +-30 while aiming (bondview2.c:5027-5085). Binding a button to it would mean synthesising
     * a stick deflection that fights the move stick. */
    (void) 0;
    return b;
}

/* ---------------------------------------------------------------- the hook */

/* Fill one pad from the real hardware buffer. Reads through joy.c's own accessors, which -- with
 * playback installed -- still index the REGULAR buffer for the underlying sample, because
 * joyPoll writes hardware to g_ContData[0] unconditionally (joy.c:476). That is what lets a
 * human keep a slot while bots take the others. */
static void ge_pad_from_hardware(int slot, OSContPad *out)
{
    out->button  = joyGetButtons((s8) slot, 0xffff);
    out->stick_x = joyGetStickX((s8) slot);
    out->stick_y = joyGetStickY((s8) slot);
    out->errno   = 0;   /* the field really is named errno; ge_win_compat.h undefines the macro */
}

static void ge_pad_from_input(int slot, const GePlayerInput *in, OSContPad *out)
{
    out->button  = ge_intent_to_buttons(slot, in->buttons);
    out->stick_x = (s8) ge_clampi(in->stick_x, -80, 80);
    out->stick_y = (s8) ge_clampi(in->stick_y, -80, 80);
    out->errno   = 0;   /* the field really is named errno; ge_win_compat.h undefines the macro */
}

/* Promote anything queued for this tick into the held slot. */
static void ge_advance_slot(int slot)
{
    int i;

    for (i = 0; i < GE_QUEUE_LEN; i++) {
        struct GeQueued *q = &ge_queue[slot][i];
        if (!q->used) { continue; }
        if (q->tick != ge_tick) { continue; }
        ge_held[slot]      = q->in;
        ge_held_left[slot] = (q->hold > 0) ? q->hold : 1;
        q->used = 0;
    }

    if (ge_held_left[slot] <= 0) {
        /* Nothing scheduled and nothing held: neutral. Not "the last thing forever" -- a button
         * left down indefinitely produces exactly ONE press and then blocks the idle timers that
         * several screens rely on. */
        ge_held[slot].buttons = 0;
        ge_held[slot].stick_x = 0;
        ge_held[slot].stick_y = 0;
    }
}

/* The playback callback. Called from joyConsumeSamplesWrapper (joy.c:412) on the game thread,
 * exactly once per main-loop iteration, immediately before joyConsumeSamples runs on the same
 * buffer.
 *
 * Writes exactly ONE sample and returns its index, so curlast == curstart + 1 and
 * `buttonspressed |= cur & ~prev` is the clean edge between two consecutive frames. That is what
 * makes a one-tick action reliable here, where on the device-side path it would be a coin flip:
 * joyConsumeSamples derives presses from consecutive ring samples, so a one-frame blip is only a
 * press if the ring happened to catch both edges. */
static s32 ge_playback(struct contsample *samples, s32 curlast)
{
    s32 index = (curlast + 1) % 20;
    int slot;

    for (slot = 0; slot < GE_MAX_SLOTS; slot++) {
        OSContPad *pad = &samples[index].pads[slot];

        if (ge_src[slot] == GE_SLOT_INJECTED) {
            ge_advance_slot(slot);
            ge_pad_from_input(slot, &ge_held[slot], pad);
            if (ge_held_left[slot] > 0) { ge_held_left[slot]--; }

        } else {
            ge_pad_from_hardware(slot, pad);
        }
    }

    /* A player slot is not always one pad, so movement is routed in a SECOND PASS.
     *
     * Under the four twin-stick styles -- 2.1 Plenty, 2.2 Galore, 2.3 Domino, 2.4 Goodhead --
     * the engine reads MOVEMENT from a second controller at playernum + getPlayerCount() and
     * leaves only the turn on the slot's own pad (bondview2.c:5371). This port defaults to 2.2
     * Galore precisely so a modern dual analog stick maps onto it, so the split is the normal
     * case here rather than an exotic one. A caller posting "walk forward" should not have to
     * know any of that.
     *
     * It has to be a second pass. Writing the companion pad inside the loop above works and is
     * then immediately undone: the loop visits slots in order, so slot 1's own iteration
     * overwrites whatever slot 0 wrote into pads[1], and it overwrites it from HARDWARE, which
     * is neutral. The symptom is an injected bot that turns correctly and never moves, with no
     * sign that anything was written at all.
     *
     * The axes are split rather than mirrored. Writing the whole input to both pads would also
     * land stick_x on the companion, where it means STRAFE, so the bot would sidestep every
     * time it turned -- a subtler bug than the one being fixed, and one that reads as drift. */
    for (slot = 0; slot < GE_MAX_SLOTS; slot++) {
        extern int gePortPlayerMovePad(int idx);
        int movepad;

        if (ge_src[slot] != GE_SLOT_INJECTED) { continue; }

        movepad = gePortPlayerMovePad(slot);
        if (movepad == slot || movepad < 0 || movepad >= GE_MAX_SLOTS) { continue; }
        if (ge_src[movepad] == GE_SLOT_INJECTED) { continue; }  /* a real player owns it */

        {
            OSContPad *own = &samples[index].pads[slot];
            OSContPad *mp  = &samples[index].pads[movepad];

            mp->button  = own->button;
            mp->stick_x = 0;              /* strafe: not expressed by this API yet */
            mp->stick_y = own->stick_y;   /* walk */
            mp->errno   = 0;
            own->stick_y = 0;             /* the slot's own pad no longer walks */
        }
    }

    /* After the pads are decided, not before: the tick a caller posts against is the tick that
     * is about to be consumed. */
    ge_tick++;

    {   /* The seed fingerprint for the frame just decided. ramromreplay stores exactly this per
         * block and aborts playback when it disagrees (:257, :312-315) -- the cheapest desync
         * detector there is, and it is already proven in this engine. */
        extern u64 g_randomSeed;
        ge_seed_fp = (unsigned int) (g_randomSeed & 0xffffffffu);

    }

    return index;
}

/* ---------------------------------------------------------------- public */

void gePlayerApiInit(void)
{
    int i;

    if (ge_installed) { return; }

    for (i = 0; i < GE_MAX_SLOTS; i++) {
        ge_src[i] = GE_SLOT_HARDWARE;
        ge_held_left[i] = 0;
        ge_unreach_warned[i] = 0;   /* a new session re-earns its warnings */
        ge_held[i].buttons = 0;
        ge_held[i].stick_x = 0;
        ge_held[i].stick_y = 0;
    }

    /* Not installed until a slot is claimed. Installing the hook changes joyGetControllerCount()
     * (joy.c:277-280), which front.c:4842 uses to default the player count, so installing
     * eagerly would change the front end for every run whether or not anything used the API. */
    ge_installed = 1;
    printf("[getv][playerapi] ready (no slots claimed)\n");
    fflush(stdout);
}

static void ge_install_hook_if_needed(void)
{
    static int hooked = 0;
    int i, claimed = 0;

    for (i = 0; i < GE_MAX_SLOTS; i++) {
        if (ge_src[i] == GE_SLOT_INJECTED) { claimed++; }
    }

    if (claimed > 0 && !hooked) {
        /* controllercount is what joyGetControllerCount() reports during playback. Report all
         * four so the front end offers every slot; the hardware check is bypassed anyway, which
         * is what lets injected pads work with nothing plugged in. */
        joySetPlaybackFunc(ge_playback, GE_MAX_SLOTS);
        joySetContDataIndex(1);
        hooked = 1;
        printf("[getv][playerapi] hook installed, %d slot(s) injected\n", claimed);
        fflush(stdout);
    } else if (claimed == 0 && hooked) {
        joySetPlaybackFunc(NULL, 0);
        joySetContDataIndex(0);
        hooked = 0;
        printf("[getv][playerapi] hook removed, all slots back on hardware\n");
        fflush(stdout);
    }
}

void gePlayerClaim(int slot, GeSlotSource src)
{
    if (slot < 0 || slot >= GE_MAX_SLOTS) { return; }
    if (!ge_installed) { gePlayerApiInit(); }
    ge_src[slot] = src;
    if (src == GE_SLOT_HARDWARE) { gePlayerClearQueue(slot); }
    ge_install_hook_if_needed();
}

GeSlotSource gePlayerSource(int slot)
{
    if (slot < 0 || slot >= GE_MAX_SLOTS) { return GE_SLOT_HARDWARE; }
    return ge_src[slot];
}

unsigned long gePlayerTick(void) { return ge_tick; }

int gePlayerPost(int slot, unsigned long tick, const GePlayerInput *in, int hold_ticks)
{
    int i;

    if (slot < 0 || slot >= GE_MAX_SLOTS || in == NULL) { return 0; }
    if (tick == 0) { tick = ge_tick; }

    /* Refused, not silently dropped. In netplay a late post IS the desync, and a caller that
     * cannot distinguish "applied" from "too late" has no way to notice. */
    if (tick < ge_tick) { return 0; }

    for (i = 0; i < GE_QUEUE_LEN; i++) {
        struct GeQueued *q = &ge_queue[slot][i];
        if (q->used) { continue; }
        q->tick = tick;
        q->hold = (hold_ticks > 0) ? hold_ticks : 1;
        q->in   = *in;
        q->used = 1;
        return 1;
    }
    return 0;   /* queue full: also a refusal, also worth the caller knowing */
}

void gePlayerClearQueue(int slot)
{
    int i;
    if (slot < 0 || slot >= GE_MAX_SLOTS) { return; }
    for (i = 0; i < GE_QUEUE_LEN; i++) { ge_queue[slot][i].used = 0; }
    ge_held_left[slot] = 0;
}

void gePlayerPinDelta(int fields) { ge_pinned_delta = (fields > 0) ? fields : 0; }
int  gePlayerPinnedDelta(void)    { return ge_pinned_delta; }

unsigned int gePlayerSeedFingerprint(void) { return ge_seed_fp; }

int gePlayerSlotCount(void) { return (int) getPlayerCount(); }


/* GE_STYLE_* for a slot, and whether a bot can drive it.
 *
 * Both read the SLOT. cur_player_get_control_type() reads g_CurrentPlayer, a per-viewport
 * cursor, so asking it about slot 2 while slot 0 is being drawn returns slot 0's answer.
 *
 * gePlayerSlotIsDrivable returns 1 for the two-controller styles, and that is a deliberate
 * disagreement with the earlier reading that a 2.x slot cannot be steered. It could not be, once:
 * those styles read MOVEMENT from a second controller at playernum + getPlayerCount(), so a
 * caller writing only the slot's own pad drove the turn and never the walk. ge_playback now
 * routes the walk axis to the pad the engine actually reads, so 2.x slots ARE drivable -- which
 * matters, because this port DEFAULTS to 2.2 Galore and every bot runs on one.
 *
 * It still refuses an out-of-range or empty slot, and a style the engine has not set yet. */
int gePlayerControlType(int slot)
{
    extern int gePortPlayerControlStyle(int idx);

    if (slot < 0 || slot >= GE_MAX_SLOTS) { return -1; }
    return gePortPlayerControlStyle(slot);
}

int gePlayerSlotIsDrivable(int slot)
{
    return gePlayerControlType(slot) >= 0;
}

/* Does the input currently applied to this slot ask for movement?
 *
 * Reads the HELD input rather than the queue, because held is what the pad is actually showing the
 * game this frame. A post scheduled for three ticks away is not a command the body has been given
 * yet, and counting it would report a bot as commanded-and-motionless during the delay it is
 * supposed to be standing still for.
 *
 * A hardware slot answers 0: the port did not command anything there, and reporting a human's own
 * walking as a bot command would make every player look stuck the moment they touched a wall. */
int gePlayerCommandedMove(int slot)
{
    if (slot < 0 || slot >= GE_MAX_SLOTS)   { return 0; }
    if (ge_src[slot] != GE_SLOT_INJECTED)   { return 0; }
    return (ge_held[slot].stick_x != 0 || ge_held[slot].stick_y != 0);
}

int gePlayerStateGet(int slot, GePlayerState *out)
{
    f32 pos[3];

    if (out == NULL || slot < 0 || slot >= GE_MAX_SLOTS) { return 0; }

    out->fields = 0;
    out->present = 0;
    out->x = out->y = out->z = 0.0f;
    out->angle = 0.0f;
    out->room = -1;
    out->health = out->armour = 0.0f;
    out->dead = 0;
    out->weapon = out->ammo_clip = out->ammo_reserve = 0;
    out->kills = out->deaths = out->shots = 0;

    /* gePortPlayerPos reads g_playerPointers[slot] -- the stable array -- and returns 0 for an
     * empty slot. NOT g_CurrentPlayer, which is a per-viewport cursor. */
    if (!gePortPlayerPos(slot, pos)) { return 0; }

    out->present = 1;
    out->x = pos[0];
    out->y = pos[1];
    out->z = pos[2];
    out->fields |= GE_ST_POSITION;

    /* Heading, from the same forward vector the walk code steers by. gePortPlayerAngle lives
     * in the decomp because struct player is a game type; it refuses a zero vector rather than
     * reporting a heading of zero, so an absent angle stays absent from `fields`. */
    {
        extern int gePortPlayerAngle(int idx, float *out_deg);
        float deg;
        if (gePortPlayerAngle(slot, &deg)) {
            out->angle = deg;
            out->fields |= GE_ST_ANGLE;
        }
    }

    /* The rest, from the accessors in objective_status.c. Each refuses rather than writing a
     * zero, so a field only appears in `fields` when the game actually had an answer -- a
     * caller reading a health of 0.0 that really meant "not implemented" would be acting on
     * a lie, and that distinction is the whole point of the flags word. */
    {
        extern int gePortPlayerRoom(int idx, int *out_room);
        extern int gePortPlayerHealth(int idx, float *hp, float *armour, int *dead);
        extern int gePortPlayerWeapon(int idx, int *weapon, int *clip, int *reserve);
        extern int gePortPlayerScore(int idx, int *kills, int *deaths, int *shots);

        int room, weapon, clip, reserve, kills, deaths, shots, dead;
        float hp, armour;

        if (gePortPlayerRoom(slot, &room)) {
            out->room = room;
            out->fields |= GE_ST_ROOM;
        }
        if (gePortPlayerHealth(slot, &hp, &armour, &dead)) {
            out->health = hp;
            out->armour = armour;
            out->dead   = dead;
            out->fields |= GE_ST_HEALTH;
        }
        if (gePortPlayerWeapon(slot, &weapon, &clip, &reserve)) {
            out->weapon       = weapon;
            out->ammo_clip    = clip;
            out->ammo_reserve = reserve;
            out->fields |= GE_ST_WEAPON;
        }
        if (gePortPlayerScore(slot, &kills, &deaths, &shots)) {
            out->kills  = kills;
            out->deaths = deaths;
            out->shots  = shots;
            out->fields |= GE_ST_SCORE;
        }
    }

    return 1;
}

void gePlayerApiShutdown(void)
{
    int i;
    for (i = 0; i < GE_MAX_SLOTS; i++) { gePlayerClaim(i, GE_SLOT_HARDWARE); }
    ge_installed = 0;
}
