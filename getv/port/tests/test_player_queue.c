/* The player API's queue discipline -- the seam everything else rides on.
 *
 * Bots, the Lua bridge and netplay all reach the game through gePlayerPost, so its contract is
 * necessary in a way none of the consumers are individually. test_intent.c covers which BIT an
 * intent becomes; this covers WHEN input is applied, for how long, and what happens when it is
 * not.
 *
 * Three properties matter more than the rest, and all three fail quietly:
 *
 *   A LATE POST IS REFUSED, NOT DROPPED. In netplay a post for a tick that has already run IS the
 *   desync. A caller that cannot tell "applied" from "too late" has no way to notice one.
 *
 *   AN EXPIRED HOLD GOES NEUTRAL, NOT "the last thing forever". A button left down indefinitely
 *   produces exactly one press and then blocks the idle timers several screens rely on, so a bot
 *   that stopped posting would wedge the front end rather than idle.
 *
 *   A FULL QUEUE IS ALSO A REFUSAL. Silently overwriting the oldest entry would make a bot that
 *   over-posts look like a bot with a planning bug.
 *
 * The real playback hook is driven here rather than the pieces, so tick advance, queue promotion,
 * hold countdown and the neutral fallback are exercised the way they actually run.
 */

#include <stdio.h>
#include <string.h>

static unsigned int g_test_style;

/* Everything the unit reaches for. They report "nothing here" on purpose: a stub returning
 * plausible data would be inventing a game, and the queue would then be tested against my guess
 * at one rather than against its own logic. */
unsigned int get_player_control_style(int playernum) { (void) playernum; return g_test_style; }
int  gePortPlayerControlStyle(int idx) { return (idx >= 0 && idx < 4) ? (int) g_test_style : -1; }
int  getPlayerCount(void) { return 4; }
unsigned short joyGetButtons(signed char p, unsigned short m) { (void) p; (void) m; return 0; }
signed char joyGetStickX(signed char p) { (void) p; return 0; }
signed char joyGetStickY(signed char p) { (void) p; return 0; }
void joySetContDataIndex(int i) { (void) i; }
int  gePortPlayerPos(int idx, float *out) { (void) idx; (void) out; return 0; }

/* the accessors. Absent here, which by their own contract means "unavailable" and is the
 * correct answer for a build with no game attached. */
int gePortPlayerMovePad(int idx) { (void) idx; return -1; }
int gePortPlayerAngle(int idx, float *d) { (void) idx; (void) d; return 0; }
int gePortPlayerRoom(int idx, int *r) { (void) idx; (void) r; return 0; }
int gePortPlayerHealth(int idx, float *h, float *a, int *d)
{ (void) idx; (void) h; (void) a; (void) d; return 0; }
int gePortPlayerWeapon(int idx, int *w, int *c, int *r)
{ (void) idx; (void) w; (void) c; (void) r; return 0; }
int gePortPlayerScore(int idx, int *k, int *d, int *s)
{ (void) idx; (void) d; (void) k; (void) s; return 0; }

#include "ge_player_api.c"

void joySetPlaybackFunc(ge_contplaybackfunc func, s32 count) { (void) func; (void) count; }
u64  g_randomSeed = 0xAB8D9F7781280783ULL;

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %-52s %d\n", what, got);
    } else {
        printf("  FAIL  %-52s got %d want %d\n", what, got, want);
        failures++;
    }
}

/* One frame of the real hook.
 *
 * THE ARGUMENT IS joy.c's 20-DEEP SAMPLE RING, NOT ONE SAMPLE. ge_playback computes
 * `index = (curlast + 1) % 20` and writes samples[index], so handing it a single struct puts a
 * whole contsample past the end of the buffer.
 *
 * The first version of this file did exactly that, and the way it failed is worth keeping: the
 * overrun landed on this file's own `failures` counter. Seven checks printed FAIL and the suite
 * then reported "PASSED -- 0 failure(s)" and exited 0, because the counter was being overwritten
 * with the stick_y value the test had just posted -- 0x3C000001 for 60, 0x2D000001 for 45. A test
 * harness that corrupts its own bookkeeping reports success while failing, which is worse than
 * either outcome on its own.
 */
#define GE_JOY_RING 20

static struct contsample ring[GE_JOY_RING];

static int run_frame(int slot)
{
    memset(ring, 0, sizeof ring);
    ge_playback(ring, 0);
    return ring[1].pads[slot].stick_y;   /* (curlast + 1) % 20, with curlast 0 */
}

int main(void)
{
    GePlayerInput in;
    int i, accepted;

    printf("player API queue discipline\n\n");

    memset(&in, 0, sizeof in);
    gePlayerApiInit();
    gePlayerClaim(0, GE_SLOT_INJECTED);

    /* ---------------- when input applies ---------------- */

    in.stick_y = 60;
    check("post for the next tick",       gePlayerPost(0, ge_tick + 1, &in, 1), 1);
    check("  not applied yet",            run_frame(0), 0);   /* this frame is ge_tick */
    check("  applied on its tick",        run_frame(0), 60);
    check("  and only once",              run_frame(0), 0);

    /* tick 0 means "now" rather than "the epoch" -- a caller that omits a tick wants the
     * soonest one, and treating 0 literally would refuse every such post forever. */
    in.stick_y = 45;
    check("tick 0 means now",             gePlayerPost(0, 0, &in, 1), 1);
    check("  applies immediately",        run_frame(0), 45);

    /* ---------------- a late post is REFUSED ---------------- */

    in.stick_y = 70;
    check("post for a tick already run",  gePlayerPost(0, ge_tick - 1, &in, 1), 0);
    check("  and nothing was applied",    run_frame(0), 0);
    check("post for the current tick",    gePlayerPost(0, ge_tick, &in, 1), 1);

    gePlayerClearQueue(0);
    run_frame(0);

    /* ---------------- holds ---------------- */

    in.stick_y = 55;
    check("post with a 3-tick hold",      gePlayerPost(0, ge_tick + 1, &in, 3), 1);
    run_frame(0);                                  /* the tick before it starts */
    check("hold tick 1",                  run_frame(0), 55);
    check("hold tick 2",                  run_frame(0), 55);
    check("hold tick 3",                  run_frame(0), 55);
    /* THE ONE THAT MATTERS: not "the last thing forever". */
    check("expired hold goes NEUTRAL",    run_frame(0), 0);
    check("  and stays neutral",          run_frame(0), 0);

    /* A hold of 0 is one tick, not zero ticks -- posting something that never applies is never
     * what a caller meant. */
    in.stick_y = 33;
    gePlayerPost(0, ge_tick + 1, &in, 0);
    run_frame(0);
    check("hold 0 applies for one tick",  run_frame(0), 33);
    check("  then neutral",               run_frame(0), 0);

    /* ---------------- clear_queue ---------------- */

    in.stick_y = 80;
    gePlayerPost(0, ge_tick + 2, &in, 5);
    gePlayerClearQueue(0);
    run_frame(0);
    run_frame(0);
    check("cleared post never applies",   run_frame(0), 0);

    /* Clearing mid-hold stops the hold too, or a bot that changed its mind keeps executing the
     * tail of the plan it abandoned. */
    in.stick_y = 65;
    gePlayerPost(0, ge_tick + 1, &in, 10);
    run_frame(0);
    check("hold is running",              run_frame(0), 65);
    gePlayerClearQueue(0);
    check("clear stops a running hold",   run_frame(0), 0);

    /* ---------------- refusals ---------------- */

    check("bad slot refused",             gePlayerPost(9, ge_tick + 1, &in, 1), 0);
    check("negative slot refused",        gePlayerPost(-1, ge_tick + 1, &in, 1), 0);
    check("NULL input refused",           gePlayerPost(0, ge_tick + 1, NULL, 1), 0);

    /* A full queue refuses rather than overwriting. Silently dropping the oldest would make an
     * over-posting bot look like it had a planning bug. */
    gePlayerClearQueue(0);
    accepted = 0;
    for (i = 0; i < GE_QUEUE_LEN + 8; i++) {
        if (gePlayerPost(0, ge_tick + 1 + (unsigned long) i, &in, 1)) { accepted++; }
    }
    check("queue accepts exactly its len", accepted, GE_QUEUE_LEN);
    check("  and then refuses",            gePlayerPost(0, ge_tick + 1, &in, 1), 0);

    /* ---------------- an unclaimed slot is not driven ---------------- */

    gePlayerClearQueue(0);
    gePlayerClaim(0, GE_SLOT_HARDWARE);
    in.stick_y = 77;
    gePlayerPost(0, ge_tick + 1, &in, 1);
    run_frame(0);
    check("hardware slot ignores posts",  run_frame(0), 0);

    printf("\n%s -- %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
