/* The player API: one seam, four consumers.
 *
 * A bot, a network peer, a reinforcement-learning agent and an LLM over MCP all want the same
 * two things -- put controller input into a player slot on a numbered tick, and read out what
 * happened. This is that seam. Everything else wraps it; nothing wraps anything else.
 * See docs/PLAYER_API.md for the design and the evidence behind it.
 *
 * ---------------------------------------------------------------- how it attaches
 *
 * Through GoldenEye's own demo-playback hook, joySetPlaybackFunc() (src/joy.c:360), not through
 * the port's device layer. That choice matters:
 *
 *   - it runs ON THE GAME THREAD, exactly once per frame, from joyConsumeSamplesWrapper()
 *     (joy.c:412 <- boss.c:594). The port's own GETV_SCRIPT path injects into GePadState, which
 *     is filled by osContGetReadData on the RETRACE thread at field rate -- the wrong clock for
 *     one action per simulation step.
 *   - it fills ALL FOUR pads in a single call, which is the "one tick authority" multiplayer
 *     needs: every slot is decided together or not at all.
 *   - it bypasses the connected-controller check entirely. Every joy.c accessor guards on
 *     `(playbackcontcount < 0) && !(g_ConnectedControllers >> n & 1)`; with playback active the
 *     hardware check is skipped, so injected pads work with NO controllers attached.
 *   - joyGetControllerCount() then returns the injected count (joy.c:277-280), which is what
 *     front.c:4842 uses to default the player count, so the rest of the game agrees.
 *   - rumble is a no-op while playback is installed (joy.c:819-833), so injection cannot trip
 *     hardware side effects.
 *   - menus, pause, character select and gameplay all read through g_ContDataPtr, so there is
 *     no path that sees a different pad.
 *
 * And it needs NO change to the decompilation: joySetPlaybackFunc is a public function and this
 * file simply calls it.
 *
 * ---------------------------------------------------------------- mixing
 *
 * joyPoll writes real hardware to a hardcoded buffer -- `osContGetReadData(g_ContData[0]...)`
 * (joy.c:476) -- whether or not playback is installed, and the wrapper consumes both buffers
 * every frame. So one handler can fill slot 0 from the real pad while slots 1..3 come from
 * policies or from a network peer. ramromreplay.c:323,331 already uses that flip to let a human
 * abort a demo.
 *
 * A bot, a network peer and an RL agent are therefore the same thing, differing only in where
 * the four pad structs come from.
 *
 * ---------------------------------------------------------------- the unit is (input, delta)
 *
 * ramromreplay.c is the reference implementation and it does not record input alone. Each block
 * carries the pad samples, `speedframes` -- the frame delta (:256) -- and `randseed`, an RNG
 * fingerprint (:257), and playback ABORTS on seed mismatch (:312-315). That is a shipped desync
 * detector, and it is telling us the unit.
 *
 * The reason the delta must travel with the input: the frame delta is wall-clock derived
 * (frametiming.c:84) and multiplied into ~310 simulation sites through g_ClockTimer ->
 * g_GlobalTimerDelta (lv.c:1112,1117). Two machines that agree on input but not on delta do not
 * agree on anything.
 */
#ifndef GE_PLAYER_API_H
#define GE_PLAYER_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GE_MAX_SLOTS 4

/* ---------------------------------------------------------------- input */

/* Buttons, named by INTENT rather than by N64 bit, because the bit that fires depends on the
 * player's control style: 1.1/1.2 fire on Z with aim on L|R, while 1.3/1.4 fire on A with aim on
 * Z (bondview2.c:5546-5558). A caller that spoke in N64 bits would be silently wrong for half
 * the styles, which is a bug that looks like the bot cannot shoot.
 *
 * These are resolved to bits at injection time against the slot's own configured style. */
#define GE_IN_FIRE        (1u << 0)
#define GE_IN_AIM         (1u << 1)
#define GE_IN_USE         (1u << 2)
#define GE_IN_WEAPON_NEXT (1u << 3)
#define GE_IN_WEAPON_PREV (1u << 4)
#define GE_IN_PAUSE       (1u << 5)
#define GE_IN_CROUCH_UP   (1u << 6)
#define GE_IN_CROUCH_DOWN (1u << 7)
#define GE_IN_LOOK_UP     (1u << 8)     /* C-buttons: the look/step cluster */
#define GE_IN_LOOK_DOWN   (1u << 9)
#define GE_IN_STEP_LEFT   (1u << 10)
#define GE_IN_STEP_RIGHT  (1u << 11)
#define GE_IN_DPAD_UP     (1u << 12)
#define GE_IN_DPAD_DOWN   (1u << 13)
#define GE_IN_DPAD_LEFT   (1u << 14)
#define GE_IN_DPAD_RIGHT  (1u << 15)
#define GE_IN_START       (1u << 16)

typedef struct GePlayerInput {
    unsigned int buttons;   /* GE_IN_* */
    /* N64 stick counts, -80..80. NOT SDL units: full SDL deflection is about +-127 against the
     * N64's practical +-84, and the game's deadzones are SUBTRACTED rather than clamped (walk
     * and turn +-5, aim mode +-60), so a caller writing raw SDL magnitudes is outside the range
     * every tuned constant assumes. Values are clamped here. */
    int stick_x;
    int stick_y;            /* positive = up, as the game reads it */
} GePlayerInput;

/* Where a slot's input comes from this frame. */
typedef enum GeSlotSource {
    GE_SLOT_HARDWARE = 0,   /* copy the real pad through; the default for every slot */
    GE_SLOT_INJECTED        /* use whatever was last posted for this slot */
} GeSlotSource;

/* Take control of a slot, or hand it back. Slots default to GE_SLOT_HARDWARE, so installing the
 * API changes nothing until a slot is claimed -- which is what lets a human keep slot 0 while
 * bots take 1..3. */
void gePlayerClaim(int slot, GeSlotSource src);
GeSlotSource gePlayerSource(int slot);

/* The current input tick: one per pass of the playback handler, i.e. one per
 * joyConsumeSamplesWrapper(), i.e. one per main-loop iteration. Starts at 0 and is monotonic.
 * This is the number netplay orders on and the number an RL step corresponds to. */
unsigned long gePlayerTick(void);

/* Post input for a slot, to be applied on `tick`.
 *
 * Returns 1 if accepted, 0 if refused. A post for a tick that has already been consumed is
 * REFUSED rather than silently dropped: in netplay that is precisely the condition that causes
 * a desync, and a caller that cannot tell the difference between "applied" and "too late" has
 * no way to detect it. Post ahead of time, not on the frame you want it.
 *
 * `tick == 0` means "the next tick", which is the common case for a local bot or an RL agent
 * stepping synchronously.
 *
 * `hold_ticks` is how long the input stays applied. See the note on the 2-frame rule below;
 * 0 means "this tick only", which is safe here and is not safe on the GePadState path. */
int gePlayerPost(int slot, unsigned long tick, const GePlayerInput *in, int hold_ticks);

/* Drop anything queued for a slot without releasing it. Its input becomes neutral. */
void gePlayerClearQueue(int slot);

/* ---------------------------------------------------------------- determinism
 *
 * The (input, delta) pair from ramromreplay, plus its seed fingerprint. */

/* Pin the frame delta instead of taking it from the wall clock. 0 restores clock-derived
 * behaviour. 1 is what the port already produces by accident in its default configuration --
 * osGetCount() returns `count += 1000`, so frametiming.c's quotient is exactly 1 every frame.
 * Under GETV_REALCLOCK=1 it becomes load-dependent, and nothing downstream is reproducible. */
void gePlayerPinDelta(int fields);
int  gePlayerPinnedDelta(void);

/* The RNG fingerprint for the frame just consumed -- the low bits of g_randomSeed, which is what
 * ramromreplay stores per block and aborts on (:257, :312-315). Two peers that agree on input
 * and delta but not on this have already diverged, and this is the cheapest possible detector. */
unsigned int gePlayerSeedFingerprint(void);

/* ---------------------------------------------------------------- state readout
 *
 * Deliberately a flags word rather than a struct full of zeroes. Health, armour, angle, weapon
 * and ammo need accessors that can only be written where `struct player` is visible -- inside
 * the decompilation -- and that is a coordinated change with the other machine. Until it lands,
 * a caller must be able to tell "this field is not available in this build" from "this field is
 * genuinely zero". Returning 0 for an unavailable health would be a silent lie and would train
 * an agent on it. */
#define GE_ST_POSITION  (1u << 0)
#define GE_ST_ROOM      (1u << 1)
#define GE_ST_ANGLE     (1u << 2)
#define GE_ST_HEALTH    (1u << 3)
#define GE_ST_WEAPON    (1u << 4)
#define GE_ST_SCORE     (1u << 5)

typedef struct GePlayerState {
    unsigned int fields;    /* which of the below are actually populated */

    int   present;          /* slot occupied at all */
    float x, y, z;
    float angle;            /* heading, degrees */
    int   room;

    float health;
    float armour;
    int   dead;

    int   weapon;
    int   ammo_clip;
    int   ammo_reserve;

    int   kills;
    int   deaths;
    int   shots;
} GePlayerState;

/* Read a slot. Returns 1 if the slot exists, 0 otherwise. Populates only the fields named in
 * `fields`.
 *
 * Reads g_playerPointers[slot], never g_CurrentPlayer. g_CurrentPlayer is a cursor that is
 * re-pointed once per player per render pass, so sampling it from outside a set_cur_player scope
 * names whichever player was drawn last. That mistake already made co-op "look motionless when
 * the truth was that consecutive samples described different players"
 * (objective_status.c:738-741). */
int gePlayerStateGet(int slot, GePlayerState *out);

/* How many slots are occupied. Derived, not stored: the game has no g_NumPlayers, it counts
 * non-NULL entries in g_playerPointers every call (player.c:105-115). */
int gePlayerSlotCount(void);

/* ---------------------------------------------------------------- control style
 *
 * A slot's control style decides which physical bit each GE_IN_* intent becomes, and on the
 * two-controller styles it decides whether the intent is expressible at all. Callers need to see
 * it, because the failure mode when they cannot is silent: input is accepted, the bot moves, and
 * the only outward sign is that it moves sideways instead of turning. */

/* GE_STYLE_* value for a slot, or -1 if the slot is out of range. Values match CONTROLLER_CONFIG_*
 * in bondconstants.h: 0-3 are the one-controller styles (1.1 Honey .. 1.4 Goodnight), 4-7 the
 * two-controller ones (2.1 Plenty .. 2.4 Goodhead). */
int gePlayerControlType(int slot);

/* Non-zero if this slot can express the whole intent set and steers unambiguously -- true for the
 * 1.x styles only.
 *
 * On 2.x, fire and aim are the same bit on two different controllers and this API drives one, so
 * one of them is silently undeliverable. Worse for anything that steers: bondview2.c:5384 sets
 * canTurnTank unconditionally on that path, which routes the stick's X axis into strafe alongside
 * turn. A steering bot should check this at startup and say something rather than walk sideways
 * into a wall for eleven hundred frames. */
int gePlayerSlotIsDrivable(int slot);

/* ---------------------------------------------------------------- lifecycle */

/* Install the playback hook. Idempotent. Called once during port init; does nothing observable
 * until a slot is claimed. */
void gePlayerApiInit(void);

/* Remove the hook and return every slot to hardware. */
void gePlayerApiShutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* GE_PLAYER_API_H */
