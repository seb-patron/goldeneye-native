/* A bot, as the first real consumer of the player API.
 *
 * docs/SURFACE_QUEUE.md is explicit that the API must be designed against a real consumer:
 * "Take one bot that walks to a pad and fires, and let its needs decide the shape. An API
 * designed in the abstract will be wrong in ways nobody notices until the third consumer."
 * This is that bot.
 *
 * It is stupid -- walk forward, sweep the aim, fire in bursts. It is not trying to
 * be good at GoldenEye. Its whole job is to exercise every part of the seam under real
 * conditions: claim a slot, post on a numbered tick, hold for a duration, release, and read
 * state back. A cleverer bot would exercise exactly the same surface.
 *
 * What it is NOT: it does not touch the guard AI. Perfect Dark's simulants act directly on
 * chrdata as NPCs, so their protocol has no SVC_CHR_MOVE and why their own docs say
 * "simulants don't work in netgames". A bot here is a policy that emits controller input into a
 * player slot, exactly like a network peer or an RL agent, so all three share one path.
 *
 *  GETV_BOT=<slots> comma-separated slots to drive, e.g. "1,2,3" or "0" to drive the
 *  player in a solo stage. Off unless set.
 *  GETV_BOT_TRACE=1 log what the bot posts, once a second.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_player_api.h"

static int  ge_bot_slots[GE_MAX_SLOTS];
static int  ge_bot_count;
static int  ge_bot_ready;
static int  ge_bot_trace;

void gePortBotInit(void)
{
    const char *e;
    const char *p;

    if (ge_bot_ready) { return; }
    ge_bot_ready = 1;

    e = getenv("GETV_BOT");
    if (e == NULL || *e == '\0') { return; }

    ge_bot_trace = (getenv("GETV_BOT_TRACE") != NULL);

    for (p = e; *p != '\0'; ) {
        while (*p == ',' || *p == ' ') { p++; }
        if (*p >= '0' && *p <= '3') {
            int slot = *p - '0';
            if (ge_bot_count < GE_MAX_SLOTS) { ge_bot_slots[ge_bot_count++] = slot; }
        }
        while (*p != '\0' && *p != ',') { p++; }
    }

    if (ge_bot_count == 0) { return; }

    gePlayerApiInit();
    {
        int i;
        printf("[getv][bot] driving slot(s)");
        for (i = 0; i < ge_bot_count; i++) {
            gePlayerClaim(ge_bot_slots[i], GE_SLOT_INJECTED);
            printf(" %d", ge_bot_slots[i]);
        }
        printf("\n");
        fflush(stdout);
    }
}

/* Called once per rendered frame. Posts for the NEXT tick rather than the current one: the
 * playback handler has already run for this frame by the time anything downstream executes, so
 * posting "now" would be posting into the past, and gePlayerPost would correctly refuse it.
 * That refusal is the API working -- it is the same condition that means "desync" in netplay. */
void gePortBotFrame(int frame)
{
    int i;

    if (!ge_bot_ready) { gePortBotInit(); }
    if (ge_bot_count == 0) { return; }

    for (i = 0; i < ge_bot_count; i++) {
        int slot = ge_bot_slots[i];
        GePlayerInput in;
        int phase = (frame + slot * 37) % 240;   /* offset per slot so they do not act in unison */

        memset(&in, 0, sizeof in);

        /* Walk forward continuously. -80..80 are N64 counts, not SDL units; the game's walk
         * deadzone is +-5 SUBTRACTED, so 60 is a real jog rather than a nudge. */
        in.stick_y = 60;

        /* Sweep the aim so it is obvious from a state dump that the bot is doing something
         * deliberate rather than drifting. */
        in.stick_x = (phase < 120) ? 25 : -25;

        /* Fire in bursts. Held for 6 ticks, which is comfortably above the 2-frame minimum the
         * edge detector needs and matches the harness default of 4-plus. */
        if ((phase % 60) < 8) { in.buttons |= GE_IN_FIRE; }

        if (!gePlayerPost(slot, gePlayerTick() + 1, &in, 1)) {
            /* Refused. Worth saying out loud rather than dropping: in netplay this is the
             * desync condition, and a bot that silently stops acting looks like a policy bug. */
            static int moaned = 0;
            if (!moaned) {
                moaned = 1;
                printf("[getv][bot] post REFUSED for slot %d at tick %lu\n",
                       slot, gePlayerTick() + 1);
                fflush(stdout);
            }
        }
    }

    if (ge_bot_trace && (frame % 60) == 0) {
        for (i = 0; i < ge_bot_count; i++) {
            GePlayerState st;
            int slot = ge_bot_slots[i];
            /* Read back what the GAME sees, not what we posted. These are joy.c's own
             * accessors -- the exact calls lv.c:1732 makes to drive movement -- so if they
             * disagree with what was posted, the injection is not landing and the problem is
             * upstream of the game rather than in the policy. */
            extern signed char joyGetStickX(signed char);
            extern signed char joyGetStickY(signed char);
            extern unsigned short joyGetButtons(signed char, unsigned short);
            int rbx = joyGetStickX((signed char) slot);
            int rby = joyGetStickY((signed char) slot);
            int rbb = joyGetButtons((signed char) slot, 0xffff);

            if (gePlayerStateGet(slot, &st) && (st.fields & GE_ST_POSITION)) {
                printf("[getv][bot] f%d slot%d tick=%lu pos=(%.1f %.1f %.1f) "
                       "readback stick=(%d,%d) btn=%04x seed=%08x\n",
                       frame, slot, gePlayerTick(),
                       (double) st.x, (double) st.y, (double) st.z,
                       rbx, rby, (unsigned) rbb,
                       gePlayerSeedFingerprint());
            } else {
                printf("[getv][bot] f%d slot%d tick=%lu (slot empty)\n",
                       frame, slot, gePlayerTick());
            }
        }
        fflush(stdout);
    }
}
