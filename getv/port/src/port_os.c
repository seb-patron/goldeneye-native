/* GoldenEye tvOS port - libultra's OS layer, replaced.
 *
 * Covers the last two N64 subsystems the boot path depends on:
 *
 *   TLB   - GoldenEye maps cartridge pages into virtual memory on demand. There is
 *           no cartridge and no TLB here. What actually matters is that boss.c
 *           derives its heap from this subsystem, so the port owns a real heap and
 *           reports its bounds.
 *   OS    - message queues, timers, cache and interrupt control, controllers.
 *
 * The message queues are IMPLEMENTED, not stubbed: the game uses them to hand data
 * between subsystems, and a stub would silently drop it. See the note on blocking.
 */
/* PR/os.h redeclares bcopy/bcmp/bzero, which conflicts with <string.h> if that is
 * seen first. Include the N64 headers before any libc string header. */
/* PR/os.h declares struct fields literally named `errno` (OSContStatus, OSContPad), which
 * joy.c reads as `g_ContStatus[i].errno & CONT_NO_RESPONSE_ERROR`. Any libc that defines
 * errno as a macro makes that struct fail to parse. The undef now lives in
 * getv/port/include/ge_win_compat.h, force-included by the Windows build: it was scoped to
 * this file first, on the wrong assumption that this was the only port translation unit
 * including PR/os.h -- port_vi.c does too. */
#include <PR/ultratypes.h>
#include <PR/os.h>

#include <stdio.h>
#include <stdlib.h>

/* Neither <string.h> nor <SDL.h> can be included here, and SDL.h only because it
 * pulls <string.h> itself (SDL_stdinc.h:60). PR/os.h redeclares bcopy/bcmp/bzero/
 * sprintf with the N64's `int`-length signatures while macOS's _FORTIFY_SOURCE headers
 * define the same names as macros, so the two collide in either order: string.h first
 * gives "expected parameter declarator" on bcopy, PR/os.h first gives "conflicting
 * types for bcmp". The host timebase GETV_REALCLOCK needs therefore lives in
 * port_support.c, which can include SDL, and is reached through this one
 * declaration. */
unsigned int gePortHostN64Count(void);

#include "port_input.h"

/* ---- the heap ----------------------------------------------------------- */

/* boss.c does:
 *     start = PHYS_TO_K0(osVirtualToPhysical(&_bssSegmentEnd));
 *     mempCheckMemflagTokens(start, tlbmanageGetTlbAllocatedBlock() - start);
 * i.e. the heap is [ &_bssSegmentEnd, tlbmanageGetTlbAllocatedBlock() ). On the N64
 * those were linker symbols bracketing free RAM. Here the port owns the memory, so
 * both ends point into one real allocation.
 *
 * 8 MB matched an Expansion Pak N64, which is what the TLB-free build assumes, and it is
 * not enough here: the same game content occupies more memory at 64-bit. Every Gfx
 * command is 16 bytes instead of 8, every pointer 8 instead of 4, and every struct
 * holding pointers grew to match. Loading a real level (55 room display lists, all
 * converted 8->16 bytes) exhausts MEMPOOL_STAGE with "MEMP FAIL (exhausted, and
 * permanent pool cannot cover it): pool=4 want=48 free=0".
 *
 * mempSetBankStarts() scales every pool proportionally to this total
 * (bankstarts[i] * mempLen / mempRequested, in s64), so raising it here widens each pool
 * rather than needing per-pool tuning.
 *
 * 32 MB is 4x the N64, comfortably above the ~2x the width change implies, and nothing on
 * an Apple TV cares. boss.c passes the span as (s32), so this must stay well under 2 GB.
 *
 * Raising this does not fix "MEMP FAIL pool=4 want=48 free=0": at 8, 32, 64 and 128 MB
 * the STAGE pool is exhausted to the last byte every time, because
 * load_object_fill_header() grabs the whole remainder of the bank per model and relies
 * on fileSetSize() handing the tail back. When that shrink is refused, no heap size is
 * large enough. See the note in objecthandler_2.c.
 */
#define GE_HEAP_SIZE (32 * 1024 * 1024)

static u8 ge_heap[GE_HEAP_SIZE] __attribute__((aligned(16)));

/* Exposed as functions rather than by aliasing the linker symbol _bssSegmentEnd:
 * __attribute__((alias)) is not supported on Mach-O. boss.c calls these directly
 * under GE_PORT_NATIVE. */
u8 *gePortHeapStart(void) { return ge_heap; }
u8 *gePortHeapEnd(void)   { return ge_heap + GE_HEAP_SIZE; }

/* ---- TLB ---------------------------------------------------------------- */

#define GE_TLB_BLOCK_SIZE 0x2000

void tlbmanageEstablishManagementTable(void)
{
    printf("[getv] TLB: not present on tvOS; heap = %p .. %p (%d MB)\n",
           (void *)ge_heap, (void *)(ge_heap + GE_HEAP_SIZE),
           GE_HEAP_SIZE / (1024 * 1024));
}

/* The top of the heap. On the N64 this was the base of the TLB mapping block,
 * which happened to sit just above free RAM. */
u8 (*tlbmanageGetTlbAllocatedBlock(void))[GE_TLB_BLOCK_SIZE]
{
    return (u8 (*)[GE_TLB_BLOCK_SIZE])(ge_heap + GE_HEAP_SIZE);
}

/* ---- message queues: implemented ---------------------------------------- */

/* OS_MESG_BLOCK cannot block. libultra would park the calling thread until the
 * queue drained; this port is single-threaded, so blocking would deadlock outright.
 * Both flags therefore behave as OS_MESG_NOBLOCK and return -1 when the queue is
 * full/empty. Callers that ignore the return value will lose messages -- if
 * something goes missing at boot, look here first. */

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msgBuf, s32 count)
{
    mq->mtqueue    = NULL;
    mq->fullqueue  = NULL;
    mq->validCount = 0;
    mq->first      = 0;
    mq->msgCount   = count;
    mq->msg        = msgBuf;
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    s32 slot;

    (void)flag;
    if (mq == NULL || mq->validCount >= mq->msgCount) {
        return -1;
    }
    slot = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[slot] = msg;
    mq->validCount++;
    return 0;
}

/* Synthesised vertical-retrace message -- see the note in osRecvMesg(). Layout must
 * match sched.h's OSScMsg { short type; char misc[30]; }; only `type` is read. */
static struct { short type; char misc[30]; } ge_retrace_msg = { 1 /* OS_SC_RETRACE_MSG */, { 0 } };
static OSMesgQueue *ge_retrace_q = NULL;

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag)
{
    /* msgCount == 0 means the queue was never created; without this the `% mq->msgCount`
     * below is a divide-by-zero. Checking it first also makes a never-initialised queue
     * behave as "empty" rather than faulting. */
    if (mq == NULL || mq->validCount <= 0 || mq->msgCount <= 0 || mq->msg == NULL) {
        /* A blocking receive on the frame queue is the N64 waiting for the video
         * scheduler's vertical-retrace message. There is no scheduler thread here and
         * nothing ever posts one, so the N64 semantics ("sleep until the message
         * arrives") would be a permanent deadlock. In the port the host owns the frame
         * boundary, so the honest translation of "wait for retrace" is "the retrace
         * just happened" -- synthesise it and return.
         *
         * Scoped to the one registered queue: a blocking receive on any
         * other queue still reports empty, because inventing messages for queues whose
         * contents the caller inspects would substitute silent wrong behaviour for a
         * visible failure. */
        if (flag == OS_MESG_BLOCK && mq != NULL && mq == ge_retrace_q) {
            if (msg != NULL) {
                *msg = (OSMesg)&ge_retrace_msg;
            }
            return 0;
        }
        return -1;
    }
    (void)flag;
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}

/* Events are hardware interrupts (VI retrace, SI, PI DMA completion). None of them
 * fire here, so registration is recorded nowhere. */
void osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg msg)
{
    (void)e; (void)mq; (void)msg;
}

/* ---- timers ------------------------------------------------------------- */

/* The N64 counter runs at half the CPU clock. Nothing here depends on the absolute
 * rate, only that it advances monotonically. */
u64 osClockRate = 46875000;

/* GETV_REALCLOCK=1 swaps the synthetic counter for a real host timebase. It is off by
 * default, which is a deliberate fidelity/determinism trade.
 *
 * The one consumer that matters is `frametiming.c:waitForNextFrame()`:
 *
 *     do { n = ((osGetCount() - prev) + 387937) / 775875; } while (n < frameDelay);
 *     updateFrameCounters(n);          // n -> speedgraphframes -> g_ClockTimer
 *
 * 775875 counts is one NTSC video frame at the N64's 46.875 MHz count rate.
 *
 *  - Synthetic (default): +1000 per call means the loop exits on call 388 with
 *    delta = 388000, and (388000 + 387937) / 775875 is exactly 1, on every frame,
 *    every launch, every host. `speedgraphframes` is a constant, gameplay frames
 *    stay byte-reproducible, and the game's wall-clock speed is whatever the render
 *    rate is (now 60 fps -- see GETV_FPS in gfx_sdl2.c).
 *
 *  - Real: `n` becomes the number of video frames that genuinely elapsed, which is
 *    N64 semantics -- a host that cannot hold 60 fps advances the world 2 or 3
 *    frames per render and keeps wall-clock time correct instead of going into slow
 *    motion. It also makes `speedgraphframes` load-dependent, so every physics
 *    integrator's step count varies between runs and no gameplay frame is comparable
 *    across runs. That breaks the reproducibility this project relies on
 *    (PORTING_PLAYBOOK.md §2.10), and that is why it is opt-in.
 *
 * Both deltas stay separate either way. This function feeds `speedgraphframes`, the
 * presentation counter; `g_ClockTimer` is derived from it in `lv.c:1040-1047` and is
 * still zeroed on pause there. Nothing here collapses them.
 *
 * With GETV_REALCLOCK=1 and GETV_FPS=0, waitForNextFrame() becomes a genuine
 * busy-spin on a core until the next video frame boundary. That is what the N64 did;
 * it is not free on a laptop. */
u32 osGetCount(void)
{
    static int real = -1;
    static u32 count = 0;

    if (real == -1) {
        const char *e = getenv("GETV_REALCLOCK");
        real = (e != NULL && *e == '1') ? 1 : 0;
        printf("[getv] clock: %s timebase (GETV_REALCLOCK=%d) -- speedgraphframes is %s\n",
               real ? "REAL host" : "synthetic", real,
               real ? "load-dependent" : "deterministically 1");
        fflush(stdout);
    }

    if (real) {
        /* Host performance counter scaled to the N64's 46.875 MHz count rate; see
         * gePortHostN64Count() in port_support.c. The u32 wrap it carries is exactly
         * the wrap osGetCount() had on hardware, and waitForNextFrame() only ever
         * subtracts two samples, which is wrap-safe. */
        return (u32)gePortHostN64Count();
    }

    /* One tick per call. See the note above for why the quotient is always 1. */
    return count += 1000;
}

int osSetTimer(OSTimer *t, OSTime countdown, OSTime interval, OSMesgQueue *mq, OSMesg msg)
{
    (void)countdown; (void)interval;
    if (t != NULL) {
        t->next = NULL;
        t->prev = NULL;
        t->mq   = mq;
        t->msg  = msg;
    }
    /* No timer interrupt exists, so the message never fires. */
    return 0;
}

/* ---- cache / interrupts ------------------------------------------------- */

/* The RCP read straight out of RDRAM, so the CPU had to write its cache back
 * before handing over a display list. Cache coherency is the hardware's problem
 * here. */
void osWritebackDCacheAll(void) { }

OSIntMask osSetIntMask(OSIntMask im) { (void)im; return 0; }

/* ---- controllers -------------------------------------------------------- */

/* Input: SDL GameController -> OSContPad, replacing the N64's SI bus.
 *
 * joy.c's poll loop (joy.c:463) is gated on
 *     `g_ContInitDone && osRecvMesg(&g_ContInputMessageQueue, &msg, 0) == 0`
 * so a controller read only happens if osContStartReadData posts to that queue. On the
 * N64 the SI interrupt posted when the transfer completed; here the pad state is
 * available immediately, so post straight away.
 *
 * Without that post the entire read path is unreachable, and the linker dead-strips it,
 * so osContGetReadData appears neither as defined nor as undefined in the built binary.
 * A missing input implementation then looks exactly like a working one.
 *
 * The SDL half is in port_input.c; see port_input.h for why it cannot live here.
 */

/* How many N64 ports report connected this instant.
 *
 * Always at least one. tvOS enumerates pads several frames after the game has run
 * osContInit(), and a port that reports absent at boot is not recoverable for the menu
 * path. An attached-but-idle port reads as all-zero, which is harmless; an unclaimed
 * one is not. Ports 2-4 have no such problem: joyCheckStatus() re-queries every 120
 * frames (joy.c:482) and rebuilds g_ConnectedControllers from these errno fields, so a
 * pad that arrives late does light up its port later. */
/* ---- GETV_DUALANALOG: one physical gamepad -> the two N64 pads style 2.2 wants ----
 *
 * GoldenEye already has dual-analog. Styles 2.2 Galore and 2.4 Goodhead are
 * pad-2-moves / pad-1-looks (GE_RETAIL_BEHAVIOUR.md §1.3, `bondview2.c:5056-5068`),
 * which is exactly a modern twin-stick pad. Nothing has to be invented; the two sticks
 * just have to arrive on two N64 ports.
 *
 *   N64 port 0  = "controller 1"  X = turn,   Y = pitch   <- physical RIGHT stick
 *   N64 port 1  = "controller 2"  X = strafe, Y = walk    <- physical LEFT  stick
 *
 * `bondview2.c:5017-5019` reads the second pad at
 * `joyGetStickX(get_cur_playernum() + getPlayerCount())`, which in 1P is index 1.
 *
 * The failure mode here is silent: every joy.c accessor gates on
 * `g_ConnectedControllers >> contpadnum & 1` (joy.c:540, :551, :562, :573, :584, :595)
 * and returns 0 with no diagnostic when the bit is clear. So claiming port 1 in
 * gePortLiveCount() is not cosmetic -- without it the second stick reads as a dead
 * centre forever and looks exactly like a mapping bug.
 *
 * Triggers, for style 2.2 Galore (`bondview2.c:5070-5085`): FIRE is Z on controller 1
 * and AIM is Z on controller 2, so RT -> port 0 and LT -> port 1 gives the modern
 * right-trigger-shoots / left-trigger-aims layout with no further translation.
 * (2.4 Goodhead is the same sticks with the two Zs swapped.)
 *
 * Two known consequences, which are why this defaults to off:
 *   - The front-end reads port 0's stick for menu navigation (`front.c:1149-1150`,
 *     `joyGetStickX(PLAYER_1)`), and port 0 is now the look stick, so menus are driven
 *     by the right stick. The physical D-pad still works and is unaffected.
 *   - A second reported port makes the main menu's multiplayer entry selectable
 *     (`front.c:3095`, `:3243`, both gated on `joyGetControllerCount() >= 2`) even
 *     though only one physical pad exists.
 *
 * Port 1 is only ever claimed when there is exactly one physical pad. With two real
 * pads the player genuinely has two controllers and the retail assignment applies
 * unchanged; stealing port 1 there would break real 2-player. */
static int geDualAnalog(void)
{
    static int on = -1;
    if (on == -1) {
        const char *e = getenv("GETV_DUALANALOG");
        if (e != NULL && *e != '\0') {
            /* Explicit wins, either way. */
            on = (*e == '1') ? 1 : 0;
        } else {
            /* Derived from the chosen style rather than being a second independent
             * default. The split only makes sense for a two-controller style, and a
             * two-controller style is unplayable on one pad without it, so one setting
             * must not be able to contradict the other. GETV_CONTROLS is what the
             * config layer's `controls=` key exports (ge_config.c:314); the fallback
             * matches file2.c's own default so the two agree when neither a config
             * file nor an environment override is present.
             * CONTROLLER_CONFIG_PLENTY == 4 is the first two-controller style. */
            const char *c = getenv("GETV_CONTROLS");
            int style = (c != NULL && *c != '\0') ? atoi(c) : 5 /* GALORE */;
            on = (style >= 4 && style <= 7) ? 1 : 0;
        }
        if (on) {
            printf("[getv] input: dual-analog ON -- one pad presented as N64 ports 0+1 "
                   "(port 0 = right stick/look, port 1 = left stick/move). "
                   "GETV_DUALANALOG=0 to disable.\n");
            fflush(stdout);
        }
    }
    return on;
}

/* ---- the front-end stick fix -------------------------------------------------
 *
 * Without this the front-end appears broken. Every front-end stick read goes through
 * `joyGetStickX/Y(PLAYER_1)` -- directly at `front.c:1149-1150` and `:2335`, and
 * indirectly via `joyGetStickXInRange` (`joy.c:636`), which every other menu call site
 * uses. PLAYER_1 is N64 port 0, and in a 2.x style port 0 is the look pad, so menu
 * navigation would answer only to the right stick.
 *
 * The fix is to or both physical sticks onto port 0 while the front-end is up. It has
 * to be conditional: doing it in-game would make walking forward also pitch the
 * camera, because port 0's Y is the look axis there.
 *
 * `current_menu` is the front-end state machine's own variable (`front.c:8934`
 * `frontChangeMenu`); port_input.c already samples it for GETV_FRONTTRACE. The two
 * states in which a level is running are MENU_RUN_STAGE and MENU_SPECTRUM_EMU, paired
 * exactly this way at `front.c:8952`. Everything else is a menu.
 *
 * Declared `int` and compared against literals rather than including bondconstants.h:
 * PORTFLAGS carries no decomp include path, and pulling front.h in would drag the PR/
 * headers into a translation unit that must not see them. The enum is
 * `MENU_INVALID = -1` followed by unvalued members (bondconstants.h:1939-1966), so the
 * ordinals are fixed by the C standard, and the names are spelled out below so a
 * reordering is at least greppable. */
#define GE_MENU_RUN_STAGE    11   /* bondconstants.h:1950 MENU_RUN_STAGE   */
#define GE_MENU_SPECTRUM_EMU 25   /* bondconstants.h:1964 MENU_SPECTRUM_EMU */

static int geInFrontEnd(void)
{
    extern int current_menu;    /* front.c; see port_input.c:339 for the same extern */

    /* MENU_INVALID (-1) means "in-game", not "in a menu". On a GETV_STAGE direct boot
     * the front-end state machine never runs at all and `current_menu` stays at its
     * initial -1 for the whole session. Treating that as "front-end" would or the two
     * sticks together during gameplay -- walking forward would also pitch the camera --
     * and would leave port 1 permanently unreported, so the move stick would be dead.
     * Only states 0..N that the machine actually reaches are menus. */
    if (current_menu < 0) { return 0; }
    return current_menu != GE_MENU_RUN_STAGE && current_menu != GE_MENU_SPECTRUM_EMU;
}

/* Pick whichever stick is actually being pushed. Sum-and-clamp rather than "left if
 * non-zero" so a diagonal held across both sticks does not fight itself, and so the
 * result still saturates at the N64's own +-80 rail that front.c's thresholds
 * (`joyGetStickX(0) >= 6`, `< -0x2D`, ...) are written against. */
/* ---- action bindings ------------------------------------------------------------
 *
 * Every game action resolves through here instead of naming a pad field inline, so a
 * player can move fire off the right trigger without a rebuild. Keys are read with
 * getenv(), which is deliberate: the config layer (`ge_config.c`) publishes every
 * `goldeneye.cfg` key by setenv(), so file, environment and CLI all arrive through one
 * lookup and this translation unit keeps no link coupling to the config translation
 * unit -- important because the tvOS build must keep linking whether or not it
 * compiles ge_config.c.
 *
 * Sources are positional, matching SDL and `struct GePadState`: "a" is the bottom face
 * button on every pad, including Nintendo's (where it is labelled B). See the profile
 * note in port_input.h. A binding therefore never needs to know the pad type; only a
 * printed prompt does.
 *
 * Crouch is not bindable here, and that is a property of the game rather than an
 * omission. In the two-controller styles crouch is not a button at all: it is
 * controller 2's stick Y crossing +-30 while aiming (`bondview2.c:5027-5085`,
 * GE_RETAIL_BEHAVIOUR.md §1.3), the same axis that walks you when not aiming. Binding
 * a button to it would mean synthesising a stick deflection that fights the move
 * stick. It needs a design decision, not a key. */
enum {
    GE_SRC_NONE = 0,
    GE_SRC_A, GE_SRC_B, GE_SRC_X, GE_SRC_Y,
    GE_SRC_LB, GE_SRC_RB, GE_SRC_LT, GE_SRC_RT,
    GE_SRC_START, GE_SRC_BACK
};

enum {
    GE_ACT_FIRE = 0, GE_ACT_AIM, GE_ACT_USE,
    GE_ACT_WEAPON_NEXT, GE_ACT_WEAPON_PREV, GE_ACT_PAUSE,
    GE_ACT_MAX
};

/* No <string.h> in this translation unit -- <PR/os.h> and the system string headers
 * cannot coexist (see the include note at the top). Six lines of comparator is cheaper
 * than reopening that, and makes the accepted spelling explicit. */
static int geStrEq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0' && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static int geParseSrc(const char *v, int fallback)
{
    if (v == NULL || *v == '\0')  { return fallback; }
    if (geStrEq(v, "a"))          { return GE_SRC_A; }
    if (geStrEq(v, "b"))          { return GE_SRC_B; }
    if (geStrEq(v, "x"))          { return GE_SRC_X; }
    if (geStrEq(v, "y"))          { return GE_SRC_Y; }
    if (geStrEq(v, "lb"))         { return GE_SRC_LB; }
    if (geStrEq(v, "rb"))         { return GE_SRC_RB; }
    if (geStrEq(v, "lt"))         { return GE_SRC_LT; }
    if (geStrEq(v, "rt"))         { return GE_SRC_RT; }
    if (geStrEq(v, "start"))      { return GE_SRC_START; }
    if (geStrEq(v, "back"))       { return GE_SRC_BACK; }
    if (geStrEq(v, "none"))       { return GE_SRC_NONE; }
    printf("[getv] input: binding \"%s\" not recognised -- expected a/b/x/y/lb/rb/"
           "lt/rt/start/back/none; keeping the default\n", v);
    return fallback;
}

/* Compose a binding key into `dst`, either "GETV_BIND_<ACT>" (player < 1) or
 * "GETV_P<n>_BIND_<ACT>". Written by hand because this translation unit has no <string.h>:
 * <PR/os.h> and the system string headers cannot coexist here (see the include note at the
 * top of the file), which is also why geStrEq exists. */
static void geBindKey(char *dst, int cap, int player, const char *act)
{
    static const char pre[]  = "GETV_P";
    static const char mid[]  = "_BIND_";
    static const char glob[] = "GETV_BIND_";
    int i = 0, k;

    if (player >= 1) {
        for (k = 0; pre[k] != '\0' && i < cap - 1; k++) { dst[i++] = pre[k]; }
        if (i < cap - 1) { dst[i++] = (char)('0' + player); }
        for (k = 0; mid[k] != '\0' && i < cap - 1; k++) { dst[i++] = mid[k]; }
    } else {
        for (k = 0; glob[k] != '\0' && i < cap - 1; k++) { dst[i++] = glob[k]; }
    }
    for (k = 0; act[k] != '\0' && i < cap - 1; k++) { dst[i++] = act[k]; }
    dst[i] = '\0';
}

/* The trigger defaults below are a judgement call, not a settled fact. RT=fire /
 * LT=aim is the modern-shooter convention, but a meaningful minority of players expect
 * the inverse and GoldenEye's own retail scheme has neither. Swapping them is
 * `fire=lt aim=rt`, one config line, precisely so the choice is cheap to reverse.
 *
 * Bindings are PER PLAYER, resolved in three steps: `GETV_P2_BIND_FIRE` if set, else the
 * global `GETV_BIND_FIRE`, else the default. Split-screen is the whole reason -- with one
 * global table, moving fire off the right trigger for a player on a Nintendo pad moved it for
 * everyone, so a mixed set of controllers could not be accommodated at all. The global key
 * keeps working and still means "all four", so nothing that was configured before changes. */
static int geBindSrc(int player, int act)
{
    static int resolved = 0;
    static int src[GE_PORT_MAX_PADS][GE_ACT_MAX];

    /* Ordinal hazard, the same class as the CHEAT_ID table in ge_config.c: these tables are
     * positional and must track the GE_SRC_* / GE_ACT_* enums above verbatim. Adding or
     * removing an enum member shifts every later ordinal, and the report below then prints
     * the wrong name -- worse than no line at all, because it looks authoritative. The enums
     * are GE_SRC_* = none,a,b,x,y,lb,rb,lt,rt,start,back (11) and
     * GE_ACT_* = fire,aim,use,weapon_next,weapon_prev,pause (6, GE_ACT_MAX).
     * `dflt` is now a third table under the same rule. Re-check all three on any change. */
    static const char *const nm[] = {
        "none", "a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "back"
    };
    static const char *const act_nm[] = {
        "fire", "aim", "use", "weapon_next", "weapon_prev", "pause"
    };
    static const char *const act_key[] = {
        "FIRE", "AIM", "USE", "WEAPON_NEXT", "WEAPON_PREV", "PAUSE"
    };
    static const int dflt[] = {
        GE_SRC_RT, GE_SRC_LT, GE_SRC_B, GE_SRC_A, GE_SRC_NONE, GE_SRC_START
    };

    if (!resolved) {
        int p, a;

        for (a = 0; a < GE_ACT_MAX; a++) {
            char key[64];
            int g;

            geBindKey(key, (int)sizeof key, 0, act_key[a]);
            g = geParseSrc(getenv(key), dflt[a]);

            for (p = 0; p < GE_PORT_MAX_PADS; p++) {
                geBindKey(key, (int)sizeof key, p + 1, act_key[a]);
                src[p][a] = geParseSrc(getenv(key), g);
            }
        }
        resolved = 1;

        /* Positive confirmation of what each action resolved to. With only the
         * "not recognised" warning, a correctly-applied binding produced no evidence
         * at all, so a config key that silently failed to reach here was
         * indistinguishable from one that worked. Verifying a setting requires the
         * consumer to say what it actually got.
         *
         * Player 1 is always printed; the others only when they differ from it, so the
         * common case stays one line and a per-player override is impossible to miss. */
        for (p = 0; p < GE_PORT_MAX_PADS; p++) {
            int differs = 0;
            for (a = 0; a < GE_ACT_MAX; a++) {
                if (src[p][a] != src[0][a]) { differs = 1; }
            }
            if (p > 0 && !differs) { continue; }

            printf("[getv] input: bindings resolved, player %d --", p + 1);
            for (a = 0; a < GE_ACT_MAX && a < (int)(sizeof act_nm / sizeof act_nm[0]); a++) {
                int v = src[p][a];
                printf(" %s=%s", act_nm[a],
                       (v >= 0 && v < (int)(sizeof nm / sizeof nm[0])) ? nm[v] : "?");
            }
            printf("\n");
        }
    }

    if (player < 0 || player >= GE_PORT_MAX_PADS) { player = 0; }
    return (act >= 0 && act < GE_ACT_MAX) ? src[player][act] : GE_SRC_NONE;
}

/* Is the input bound to `act` held this frame, for the player on `player`? */
static int geHeld(const struct GePadState *st, int player, int act)
{
    switch (geBindSrc(player, act)) {
        case GE_SRC_A:     return st->a;
        case GE_SRC_B:     return st->b;
        case GE_SRC_X:     return st->x;
        case GE_SRC_Y:     return st->y;
        case GE_SRC_LB:    return st->lshoulder;
        case GE_SRC_RB:    return st->rshoulder;
        case GE_SRC_LT:    return st->ltrigger;
        case GE_SRC_RT:    return st->rtrigger;
        case GE_SRC_START: return st->start || st->back;
        case GE_SRC_BACK:  return st->back;
        default:           return 0;
    }
}

static s8 geStick(int v);   /* defined below, next to the C-button thresholds */

static s8 geStickOr(int a, int b)
{
    int v = (int)geStick(a) + (int)geStick(b);
    if (v >  80) { v =  80; }
    if (v < -80) { v = -80; }
    return (s8)v;
}

/* 1 when the split is actually in effect this frame: enabled, and at most one physical
 * pad. `<= 1` rather than `== 1` because a scripted or synthesised port (GETV_SCRIPT /
 * GETV_PADS) reports a hardware count of 0 while still delivering a present pad, and
 * gePortLiveCount() floors the port count to 1 regardless. With two real pads the
 * player genuinely has two controllers and retail assignment applies untouched. */
static int geDualAnalogActive(void)
{
    return geDualAnalog() && gePortInputPadCount() <= 1;
}

static int gePortLiveCount(void)
{
    static int last = -1;
    int n = gePortInputPadCount();

    if (n < 1) { n = 1; }
    if (n > GE_PORT_MAX_PADS) { n = GE_PORT_MAX_PADS; }

    /* Claim the second port so joy.c sets bit 1 of g_ConnectedControllers. See above:
     * without this the second stick is silently dead.
     *
     * Only once a level is running. `joyGetControllerCount()` (joy.c:273) is the index
     * of the first clear bit of that mask, and the front-end sizes multiplayer from it:
     * `front.c:3095` and `:3243` enable the MP entry at `>= 2`, `:4775` sets
     * `numplayers` from it, and `:4829` resizes the match to it. Reporting a phantom
     * second port in the menus would let someone start a two-player match with one
     * physical pad, and player 2 would not merely be idle -- it would be a second body
     * mirroring player 1's own left stick. Gating on "a level is running" gates the MP
     * entry on the real device count.
     *
     * The port count therefore changes at level entry. joy.c rebuilds the mask from the
     * errno fields on any changed edge (and re-queries every 120 frames anyway,
     * joy.c:482), so the second port lights up as the level starts rather than up to
     * two seconds later. The edge happens once per level, not per frame. */
    if (geDualAnalog() && n == 1 && !geInFrontEnd()) { n = 2; }

    /* Announce every change, not just the first value. The point of the re-query is
     * that ports appear late; a single startup line cannot tell "one pad" from "one
     * pad so far". */
    if (n != last) {
        printf("[getv] input: N64 ports connected = %d (bitpattern 0x%x)\n",
               n, (1 << n) - 1);
        last = n;
    }
    return n;
}

/* joy.c builds its slot mask from `g_ContStatus[i].errno & CONT_NO_RESPONSE_ERROR`,
 * and joyGetControllerCount() then returns the index of the first clear bit, so the
 * connected ports have to be contiguous from 0. port_input.c guarantees that; this
 * just mirrors the count. */
static void gePortContStatus(OSContStatus *status)
{
    int i;
    int live;

    if (status == NULL) {
        return;
    }
    live = gePortLiveCount();
    bzero(status, sizeof(*status) * GE_PORT_MAX_PADS);
    for (i = 0; i < GE_PORT_MAX_PADS; i++) {
        if (i < live) {
            status[i].type   = CONT_TYPE_NORMAL;
            status[i].status = 0;
            status[i].errno  = 0;
        } else {
            status[i].errno = CONT_NO_RESPONSE_ERROR;
        }
    }
}

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status)
{
    /* Prime the queue. joy.c's read branch (joy.c:463) needs a message already
     * waiting, but the only things that post one are the osContStartReadData calls
     * inside joyPoll's own gated branches (joy.c:450, :500), so the first read can
     * never happen. joy.c:450 sits behind an enable-poll request, and joyEnablePoll()
     * has no caller anywhere in the compiled tree; on hardware the SI interrupt is what
     * broke the circle.
     *
     * joy.c calls this with &g_ContInputMessageQueue, so posting one message here is
     * exactly the "an initial read has completed" state the poll loop expects. After
     * that joy.c:500 re-arms it every frame and this never matters again. */
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)0, OS_MESG_NOBLOCK);
    }

    /* Claim controller 1 even if no pad has enumerated yet -- see gePortLiveCount().
     * Beyond that, report one bit per connected port, contiguous from bit 0, so
     * `bitpattern` is 0x1 / 0x3 / 0x7 / 0xF for 1..4 pads. joy.c stores this straight
     * into g_ConnectedControllers, which gates every joyGetStickX/joyGetButtons
     * accessor for ports 1-3; with a hard-coded 1, players 2-4 are input-dead no matter
     * how many pads are attached. */
    if (bitpattern != NULL) {
        *bitpattern = (u8)((1 << gePortLiveCount()) - 1);
        printf("[getv] input: osContInit bitpattern = 0x%x\n", (unsigned)*bitpattern);
    }
    gePortContStatus(status);
    return 0;
}

s32 osContStartQuery(OSMesgQueue *mq)
{
    /* joy.c follows this with a BLOCKING osRecvMesg. This port's queues cannot block
     * (see the message-queue note above), so the message has to be there already. */
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)0, OS_MESG_NOBLOCK);
    }
    return 0;
}

void osContGetQuery(OSContStatus *status) { gePortContStatus(status); }

s32 osContStartReadData(OSMesgQueue *mq)
{
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)0, OS_MESG_NOBLOCK);
    }
    return 0;
}

/* SDL axes are -32768..32767; the N64 stick is a signed char the game treats as
 * -80..80. A deadzone is not optional -- analogue rest drift becomes a permanent slow
 * turn, and GoldenEye has no in-game deadzone setting to trim it out with.
 *
 * The deadzone must rescale, not just cut. A naive
 *
 *     if (v > -3200 && v < 3200) return 0;
 *     out = (v * 80) / 32767;
 *
 * subtracts nothing after the cut, so the output leaps straight from 0 to
 * (3200*80)/32767 = 7 the instant the axis crosses the threshold: a largest
 * single-count step of 7 of 80, 8.75% of full deflection appearing in one raw count.
 * In an FPS that is the familiar "the stick does nothing, then Bond snaps into a turn"
 * feel, and it is worst exactly where fine aim happens.
 *
 * The game's own safe zone does not hide it. bondview2.c's bondviewProcessInput()
 * subtracts 5 from |stick| (controlStickXSafe), so that jump still lands at 2 of 75
 * usable counts rather than at 0 -- smaller, but still a step out of nothing -- and
 * joyGetStickX()'s other consumers (joy.c's clamp helpers at :636/:696, which map the
 * raw value through +60/120) get the full 7.
 *
 * So rescale the surviving range: the curve is then continuous at the edge, still
 * reaches full deflection at the rail, and the largest single-count step is 1.
 * Endpoints are unchanged at +-80.
 *
 * per-axis rather than radial. The N64 stick's gate is
 * square-cornered-octagonal and the game clamps each axis independently
 * (bondview2.c tests `stick_x > 60` and `stick_y > 60` separately), so an axial
 * deadzone is closer to what the code was written against than a circular one.
 */
/* 20% of the SDL axis. This matches the `deadzone = 20` written into the default
 * config, so a run with no config file behaves the same as a run with the stock one.
 * The two used to disagree (3200, ~9.8%) and the difference was only visible on a
 * machine where the config could not be written. Chosen against a real Xbox pad. */
#define GE_STICK_DEADZONE  6553     /* 20% of the SDL axis */
#define GE_STICK_MAX         80     /* N64 full deflection, as the game reads it */
#define GE_SDL_AXIS_MAX   32767

static int geStickDeadzone = GE_STICK_DEADZONE;

static s8 geStick(int v)
{
    int mag, out, sign;

    if (v < 0) { sign = -1; mag = -v; }
    else       { sign =  1; mag =  v; }

    /* SDL's negative rail is -32768, one count past the positive rail. Clamp the
     * magnitude first so the rescale below cannot overshoot GE_STICK_MAX. */
    if (mag > GE_SDL_AXIS_MAX) { mag = GE_SDL_AXIS_MAX; }

    /* GETV_DEADZONE=<percent 0..40>. Exposed because an Xbox stick has a noticeably
     * larger physical dead centre than an N64 stick, and a worn pad's drift becomes a
     * permanent slow turn with no in-game setting to trim it out. The default is
     * unchanged from the value below, so leaving the key unset is a no-op.
     * This is the port's deadzone on the raw SDL axis. It is not the game's own
     * aim +-60 / walk +-5 thresholds (bondview2.c:4892-4908, :5330-5364), which are
     * applied downstream in N64 counts and are left alone. */
    {
        static int dz = -1;
        if (dz < 0) {
            const char *e = getenv("GETV_DEADZONE");
            dz = GE_STICK_DEADZONE;
            if (e != NULL && *e != '\0') {
                int pct = atoi(e);
                if (pct < 0)  { pct = 0; }
                if (pct > 40) { pct = 40; }
                dz = (GE_SDL_AXIS_MAX * pct) / 100;
                printf("[getv] input: deadzone %d%% (%d/%d counts)\n",
                       pct, dz, GE_SDL_AXIS_MAX);
                fflush(stdout);
            }
        }
        geStickDeadzone = dz;
    }

    if (mag <= geStickDeadzone) {
        return 0;
    }

    out = ((mag - geStickDeadzone) * GE_STICK_MAX)
        / (GE_SDL_AXIS_MAX - geStickDeadzone);

    if (out > GE_STICK_MAX) { out = GE_STICK_MAX; }
    return (s8)(sign * out);
}

/* The right stick has to be turned into the four digital C buttons, and a bare
 * threshold chatters: parked near the edge, sensor noise of a few hundred counts
 * toggles the bit every frame, which the game sees as a machine-gun tap. Two
 * thresholds (Schmitt trigger): it takes GE_C_ON to switch a direction on and it
 * stays on until the axis falls back below GE_C_OFF.
 *
 * The state must be static -- the point is remembering last frame. */
#define GE_C_ON   12000
#define GE_C_OFF   8000

static int geCEdge(int v, int *held)
{
    if (*held) {
        if (v < GE_C_OFF) { *held = 0; }
    } else {
        if (v > GE_C_ON)  { *held = 1; }
    }
    return *held;
}

/* GETV_INPUT_DEBUG -- prints the pad state the game reads, not the state SDL reports.
 *
 * It logs after the mapping, from the OSContPad about to be handed to joy.c. A log of
 * the SDL side would confirm that SDL sees a controller and say nothing about whether
 * the N64 bits came out right, which is the half that can be wrong.
 *
 *   GETV_INPUT_DEBUG=1  one line whenever the decoded pad changes, plus a 2s heartbeat.
 *                       Prefer this: 60 identical lines a second through
 *                       `simctl launch --console-pty` buries every other message.
 *   GETV_INPUT_DEBUG=2  one line every frame the game polls, changed or not. Use when
 *                       the question is "is the game polling at all".
 *
 * The level is read through gePortInputDebugLevel() in port_input.c, not getenv() here:
 * this translation unit includes <PR/os.h>, which cannot safely sit next to the system
 * string headers a libc include drags in. Same reason the two halves are split at all.
 */
static void gePortInputTrace(int port, const struct GePadState *st, u16 button, s8 sx, s8 sy)
{
    /* Per-port state, not one set of statics. With four ports sharing a single
     * `lastb`, every port would look "changed" on every frame simply because the
     * previous port's value was still in the slot: level 1 would degrade into level 2
     * and the change detection would be meaningless. */
    static int  frame[GE_PORT_MAX_PADS];
    static u16  lastb[GE_PORT_MAX_PADS] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    static int  lastx[GE_PORT_MAX_PADS] = { 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF };
    static int  lasty[GE_PORT_MAX_PADS] = { 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF };
    static int  primed[GE_PORT_MAX_PADS];
    int level = gePortInputDebugLevel();
    int changed;

    if (level <= 0 || port < 0 || port >= GE_PORT_MAX_PADS) {
        return;
    }
    frame[port]++;

    changed = (!primed[port]) || (button != lastb[port])
           || ((int)sx != lastx[port]) || ((int)sy != lasty[port]);
    primed[port] = 1;
    lastb[port] = button; lastx[port] = sx; lasty[port] = sy;

    /* level 1: on change, or a heartbeat every ~2s at 60Hz so a silent log is
     * distinguishable from a stalled one. */
    if (level < 2 && !changed && (frame[port] % 120) != 0) {
        return;
    }

    printf("[getv] pad p%d f=%d btn=%04x %s%s%s%s%s%s%s%s%s%s%s%s%s%s stick=(%4d,%4d) "
           "sdl L(%6d,%6d) R(%6d,%6d) T(%5d,%5d) real=%d syn=%d '%s'\n",
           port, frame[port], (unsigned)button,
           (button & CONT_A)     ? "A "   : "",
           (button & CONT_B)     ? "B "   : "",
           (button & CONT_G)     ? "Z "   : "",
           (button & CONT_START) ? "St "  : "",
           (button & CONT_L)     ? "L "   : "",
           (button & CONT_R)     ? "R "   : "",
           (button & CONT_E)     ? "Cu "  : "",
           (button & CONT_D)     ? "Cd "  : "",
           (button & CONT_C)     ? "Cl "  : "",
           (button & CONT_F)     ? "Cr "  : "",
           (button & CONT_UP)    ? "du "  : "",
           (button & CONT_DOWN)  ? "dd "  : "",
           (button & CONT_LEFT)  ? "dl "  : "",
           (button & CONT_RIGHT) ? "dr "  : "",
           (int)sx, (int)sy,
           st->lx, st->ly, st->rx, st->ry, st->lt_raw, st->rt_raw,
           st->real_gamepad, st->synthetic, gePortInputPadName(port));
}

/* Decode one port's device state into the N64 pad the game reads.
 *
 * Every piece of remembered state in here (the C-button Schmitt triggers) is indexed by
 * port. Shared hysteresis across four players would let player 1's right stick latch
 * player 3's C-buttons -- invisible in solo and unplayable in multiplayer.
 */
static void gePortDecodePad(int port, const struct GePadState *st, OSContPad *pad)
{
    u16 b = 0;

    if (geHeld(st, port, GE_ACT_WEAPON_NEXT)) { b |= CONT_A; }   /* GE's "inventory" button */
    if (geHeld(st, port, GE_ACT_USE))   { b |= CONT_B; }
    if (st->lshoulder) { b |= CONT_L; }
    if (st->rshoulder) { b |= CONT_R; }

    /* Aim has its own input. With both analogue triggers ORed onto Z (fire) and
     * nothing reaching the N64's L/R, a modern pad could not aim from a trigger at all
     * in the single-controller styles, only from the shoulders. Honey/Solitaire take
     * aim from `L_TRIG|R_TRIG` (bondview2.c:5196-5210), so binding aim there is what
     * makes LT-aims work. The physical shoulders still map to L/R as well; the or is
     * harmless. */
    if (geHeld(st, port, GE_ACT_AIM)) { b |= CONT_L | CONT_R; }

    /* Start is the pause menu and, in solo, Bond's watch. Aliasing the pad's Back /
     * Menu button onto it costs nothing -- the N64 has no fifth face bit for Back to
     * map to, and every front.c menu branch accepts START_BUTTON. */
    if (geHeld(st, port, GE_ACT_PAUSE)) { b |= CONT_START; }

    /* Z is the N64 trigger and, in the default control style, GoldenEye's FIRE button:
     * bondview2.c picks `shootButtons = Z_TRIG` for every config except KISSY and
     * GOODNIGHT. Either analogue trigger works, so it does not matter which hand the
     * player expects it under. */
    if (geHeld(st, port, GE_ACT_FIRE)) { b |= CONT_G; }

    if (st->dup)    { b |= CONT_UP; }
    if (st->ddown)  { b |= CONT_DOWN; }
    if (st->dleft)  { b |= CONT_LEFT; }
    if (st->dright) { b |= CONT_RIGHT; }

    /* The C buttons are digital on the N64, so the right stick has to be thresholded
     * rather than scaled -- there is no analogue look axis to map onto.
     *
     * What the C buttons do, per bondview2.c: the game ORs them with the D-pad
     * everywhere (`buttons & (L_JPAD | L_CBUTTONS)`), and the meaning depends on aim
     * mode --
     *   hip-fire  : C-left/right  -> digitalStepLeft/Right (strafe)
     *               C-up/down     -> speedVertaDown/Up      (step forward/back)
     *   aim mode  : C-left/right  -> aimTurnLeft/RightSpeed (look)
     * so right-stick-to-C gives a workable modern layout: strafe from the hip, look
     * while aiming. X/Y double up C-up/C-down for anything that wants them discretely.
     *
     * Hysteresis (geCEdge) rather than a bare compare -- see the note on the constants.
     *
     * This is a first-pass mapping chosen to make the game controllable. True
     * dual-analog (right stick as a continuous look axis) is not reachable through
     * OSContPad at all: it needs either control style 2.x driven from a synthesised
     * second pad, or a native change in bondview2.c. Neither belongs here. */
    {
        static int cu[GE_PORT_MAX_PADS], cd[GE_PORT_MAX_PADS];
        static int cl[GE_PORT_MAX_PADS], cr[GE_PORT_MAX_PADS];

        if (geCEdge(-st->ry, &cu[port]) || st->y) { b |= CONT_E; }   /* C-up    */
        if (geCEdge( st->ry, &cd[port]) || st->x) { b |= CONT_D; }   /* C-down  */
        if (geCEdge(-st->rx, &cl[port]))          { b |= CONT_C; }   /* C-left  */
        if (geCEdge( st->rx, &cr[port]))          { b |= CONT_F; }   /* C-right */
    }

    pad->button  = b;
    pad->stick_x = geStick(st->lx);
    pad->stick_y = (s8)(-(int)geStick(st->ly));   /* SDL +Y is down, the N64's is up */
    pad->errno   = 0;
}

/* The two halves of one physical pad, written straight into two OSContPads.
 * Everything downstream -- joy.c's sample ring, its edge detector, bondview2.c's
 * style dispatch -- is the production path and sees two ordinary controllers. */
static void geDecodeDualAnalog(const struct GePadState *st, OSContPad *p0, OSContPad *p1)
{
    u16 common = 0;
    /* Player 1's bindings throughout. The two OSContPads here are two N64 PORTS driven by
     * one physical pad held by one human, not two players -- geDecodeDualAnalog is only
     * reached when a single pad is present (see the geDualAnalogActive() branch in the
     * caller), so p1 is still player 1's second controller and must not read player 2's
     * keys. */
    const int player = 0;

    if (geHeld(st, player, GE_ACT_WEAPON_NEXT)) { common |= CONT_A; }  /* cycle: either pad */
    if (geHeld(st, player, GE_ACT_USE))         { common |= CONT_B; }  /* btap (tank): either pad */
    if (geHeld(st, player, GE_ACT_PAUSE))       { common |= CONT_START; }

    /* WEAPON_PREV, default unbound. GoldenEye has no back-cycle button: the retail
     * gesture is hold-inventory + tap-fire (`bondview2.c:5091-5111`), and `triggerOn`
     * is suppressed while inventory is held (`:5447-5450`) so it cannot discharge the
     * gun. Synthesising exactly that pair is therefore faithful rather than a hack,
     * but it is unverified on hardware, so it stays opt-in. */
    if (geHeld(st, player, GE_ACT_WEAPON_PREV)) { common |= CONT_A; }
    if (st->lshoulder)            { common |= CONT_L; }
    if (st->rshoulder)            { common |= CONT_R; }
    if (st->dup)                  { common |= CONT_UP; }
    if (st->ddown)                { common |= CONT_DOWN; }
    if (st->dleft)                { common |= CONT_LEFT; }
    if (st->dright)               { common |= CONT_RIGHT; }

    /* No C-buttons here. gePortDecodePad() derives them from the right stick with a
     * Schmitt trigger, which is right for the single-pad styles and wrong here: the
     * right stick has become a real analogue axis. GE_RETAIL_BEHAVIOUR.md §1.3 also
     * records that the 2.x styles never read the C-buttons at all, so synthesising them
     * could only cause spurious menu input. */

    /* 2.2 Galore: FIRE is Z on controller 1, AIM is Z on controller 2
     * (`bondview2.c:5070-5085`). The binding layer decides which physical input each
     * one is; the port assignment is fixed by the style. */
    p0->button  = common | (geHeld(st, player, GE_ACT_FIRE) ? CONT_G : 0)
                         | (geHeld(st, player, GE_ACT_WEAPON_PREV) ? CONT_G : 0);
    p0->errno   = 0;

    if (geInFrontEnd()) {
        /* Menus: EITHER stick drives the cursor. See geInFrontEnd() above. */
        p0->stick_x = geStickOr(st->rx, st->lx);
        p0->stick_y = (s8)(-(int)geStickOr(st->ry, st->ly));
    } else {
        p0->stick_x = geStick(st->rx);
        p0->stick_y = (s8)(-(int)geStick(st->ry));        /* SDL +Y down, N64 +Y up */
    }

    p1->button  = common | (geHeld(st, player, GE_ACT_AIM) ? CONT_G : 0);
    p1->stick_x = geStick(st->lx);
    p1->stick_y = (s8)(-(int)geStick(st->ly));
    p1->errno   = 0;
}

void osContGetReadData(OSContPad *pad)
{
    struct GePadState st;
    int live;
    int i;

    if (pad == NULL) {
        return;
    }

    /* joy.c hands us `g_ContData[0].samples[index].pads`, which is
     * OSContPad[MAXCONTROLLERS] with MAXCONTROLLERS == 4. Writing five entries would
     * overrun it. */
    _Static_assert(GE_PORT_MAX_PADS == 4, "OSContPad[] handed in by joy.c is MAXCONTROLLERS==4");

    bzero(pad, sizeof(*pad) * GE_PORT_MAX_PADS);

    /* Port 0's poll is what pumps SDL and rescans devices, so read it first and let
     * the port count it establishes govern the rest of this frame. */
    gePortInputPollPort(0, &st);
    live = gePortLiveCount();

    if (geDualAnalogActive() && st.present) {
        geDecodeDualAnalog(&st, &pad[0], &pad[1]);
        gePortInputTrace(0, &st, pad[0].button, pad[0].stick_x, pad[0].stick_y);
        gePortInputTrace(1, &st, pad[1].button, pad[1].stick_x, pad[1].stick_y);
        for (i = 2; i < GE_PORT_MAX_PADS; i++) {
            pad[i].errno = CONT_NO_RESPONSE_ERROR;
        }
        return;
    }

    for (i = 0; i < GE_PORT_MAX_PADS; i++) {
        if (i != 0) {
            gePortInputPollPort(i, &st);
        }

        if (i >= live || !st.present) {
            /* errno 0 = "present but idle", not NO_RESPONSE, for a port we are
             * CLAIMING (i < live) whose device has not enumerated yet: joy.c calls
             * joyCheckStatus() on any errno-changed edge, so flapping this would
             * thrash it every frame until a pad appears. A port beyond `live` is
             * genuinely absent and says so. */
            if (i >= live) {
                pad[i].errno = CONT_NO_RESPONSE_ERROR;
            }
            gePortInputTrace(i, &st, 0, 0, 0);
            continue;
        }

        gePortDecodePad(i, &st, &pad[i]);
        gePortInputTrace(i, &st, pad[i].button, pad[i].stick_x, pad[i].stick_y);
    }
}

/* ---- the SI bus, as far as motor.c needs it ------------------------------
 *
 * These five appeared the moment joyPoll() became reachable: joyPoll -> joyRumblePakTick
 * -> motor.c, which talks to the Rumble Pak by hand-assembling a PIF RAM command block
 * and pushing it over the Serial Interface.
 *
 * There is no SI and no PIF here. Rumble is a real feature worth having later through
 * SDL_GameControllerRumble, but it belongs in port_input.c with the rest of the pad,
 * not in a fake PIF buffer. So the transport is stubbed and motor.c is left to run
 * against it harmlessly.
 *
 * __osPfsPifRam must be real storage, not a function stub: motor.c takes its address
 * and writes command bytes into it. A zero-returning function stub here would be a
 * write through a code address.
 */
u32 __osPfsPifRam[16];          /* 64 bytes, the PIF RAM block motor.c fills in */
u8  __osContLastCmd = 0;

s32  __osSiRawStartDma(s32 dir, void *dramAddr) { (void)dir; (void)dramAddr; return 0; }
void __osSiGetAccess(void) { }
void __osSiRelAccess(void) { }

/* Controller Pak (mempak) and Rumble Pak, used by motor.c. */
s32 osPfsInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    (void)mq; (void)pfs; (void)channel;
    return 1;   /* non-zero = no pak present */
}

u32 __osContAddressCrc(u32 addr) { return addr & 0x1f; }
s32 __osContRamRead(OSMesgQueue *mq, s32 channel, u16 address, u8 *buffer)
{
    (void)mq; (void)channel; (void)address; (void)buffer;
    return 1;
}
s32 __osContRamWrite(OSMesgQueue *mq, s32 channel, u16 address, u8 *buffer, s32 force)
{
    (void)mq; (void)channel; (void)address; (void)buffer; (void)force;
    return 1;
}

/* Cache invalidation: the RCP read straight out of RDRAM, so the CPU had to push or
 * drop cache lines around any buffer it shared. Cache coherency is the hardware's
 * problem on arm64. Paired with osWritebackDCacheAll above. */
void osInvalDCache(void *vaddr, s32 nbytes) { (void)vaddr; (void)nbytes; }
void osInvalICache(void *vaddr, s32 nbytes) { (void)vaddr; (void)nbytes; }
void osWritebackDCache(void *vaddr, s32 nbytes) { (void)vaddr; (void)nbytes; }

/* ---- queues the harness owns -------------------------------------------- */

/* The harness drives bossMainloop() directly and never runs init.c's mainproc(), so
 * schedulerInitThread() -- which is what normally calls
 * osCreateMesgQueue(&gfxFrameMsgQ, &gfxFrameMsgBuf, 32) -- never happens.
 *
 * init.c does not compile yet, so gfxFrameMsgQ is a generated stub, and
 * gePortStubInit() fills the first 4096 bytes of every stub with 0xFF poison. An
 * uninitialised OSMesgQueue therefore reads as validCount = 0xFFFFFFFF with
 * msg = 0xFFFFFFFFFFFFFFFF, so bossMainloop's first osRecvMesg() drain loop
 * dereferences a maximally-invalid pointer and takes SIGSEGV.
 *
 * Creating the queue for real is the right fix: the harness has taken over init.c's
 * job, so it owes init.c's setup. */
static OSMesg ge_gfx_frame_msgbuf[32];

void gePortCreateHarnessQueues(void)
{
    extern OSMesgQueue gfxFrameMsgQ;   /* stub-backed storage; real type is OSMesgQueue */

    osCreateMesgQueue(&gfxFrameMsgQ, ge_gfx_frame_msgbuf, 32);
    ge_retrace_q = &gfxFrameMsgQ;   /* the one queue allowed to synthesise retraces */
    printf("[getv] harness queues: gfxFrameMsgQ created (%d slots)\n", 32);
    fflush(stdout);
}

/* ---- graphics task submission ------------------------------------------- */

/* Called from rspGfxTaskStart() in place of handing the task to the N64 video
 * scheduler. Two jobs:
 *   1. acknowledge the task, so bossMainloop's `pendingGfx--` fires and the frame loop
 *      can advance to the next frame. Without this it spins forever.
 *   2. (later) hand firstGdl..gdl to Fast3D.
 *
 * Rendering is not wired up in this step. GoldenEye's display lists are
 * built for its own gsp3D microcode, and Fast3D's F3D parser has not been proven
 * against them yet; doing both at once would make a crash ambiguous between "the loop
 * still does not close" and "the display list is not F3D enough". Close the loop first.
 */
/* Takes the fields it needs rather than an OSScTask*: OSScTask lives in the decomp's
 * src/sched.h, which the port cannot include (the decomp ships math.h/string.h/stddef.h
 * that shadow the system headers), and mirroring a struct that embeds OSTask would
 * couple the port to a layout it cannot verify. */
void gePortSubmitGfxTask(void *firstGdl, void *gdl, OSMesgQueue *replyQ, OSMesg replyMsg,
                         void *framebuffer)
{
    static int frames = 0;

    /* First few frames in detail, then a heartbeat. A per-frame print produces
     * hundreds of megabytes of log; a heartbeat still proves frames are advancing. */
    if (frames < 3 || (frames % 300) == 0) {
        printf("[getv] gfx task %d: %ld commands (%p..%p), cfb=%p\n",
               frames, (long)(((char *)gdl - (char *)firstGdl) / 8),
               firstGdl, gdl, framebuffer);
        fflush(stdout);
    }
    frames++;

    /* Hand the display list to Fast3D. firstGdl..gdl is exactly what the RSP would have
     * executed. See port_render.c -- the include worlds are kept apart. */
    { extern void gePortRenderDisplayList(void *firstGdl); gePortRenderDisplayList(firstGdl); }

    /* Reply exactly as the scheduler would: the message the caller supplied, to the
     * queue the caller nominated. bossMainloop reads its type and decrements pendingGfx. */
    if (replyQ != NULL) {
        osSendMesg(replyQ, replyMsg, OS_MESG_NOBLOCK);
    }
}
