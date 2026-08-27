/* Attach compiled bot AI lists to characters in a running stage.
 *
 * THIS IS THE NPC PATH, AND IT IS NOT THE SAME THING AS ge_bot.c.
 *
 * ge_bot.c drives a PLAYER SLOT by injecting controller input, which is deliberate: a bot, a
 * network peer and an RL agent then all share one path into the game, and its own comment
 * explains why it stays away from the guard AI. That decision still stands for anything that
 * has to occupy a player slot.
 *
 * This file does the other thing. The archetypes in data/bots are compiled to AI-list bytecode
 * by tools/asm_bot_ai.py, and an AI list drives a CHARACTER -- a chr, the same entity every
 * guard in the campaign is. So these are NPC opponents added to a stage, not players. The two
 * mechanisms compose: four human or injected players in the slots, plus as many AI-driven
 * characters as the stage will carry.
 *
 * The attachment itself is one call the game already provides:
 *
 *     chrSpawnAtPad(self, bodynum, headnum, padid, ailist, flags)
 *
 * where ailist is a pointer to bytecode. chrai.c does exactly this for the campaign's own
 * spawn command, passing the result of ailistFindById. We pass our assembled bytes instead,
 * which is why no registration in g_CurrentSetup.ailists is needed: the pointer IS the list.
 *
 *   GETV_BOT_AI=<archetype>[:count][@pad]   e.g. "dark", "hard:3", "kaze:2@12"
 *   GETV_BOT_AI_BODY=<n>                    body model id, default 37 (a campaign guard body)
 *   GETV_BOT_AI_DELAY=<frames>              wait before spawning, default 180
 *   GETV_BOT_AI_LIST=1                      print the available archetypes and do nothing else
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_bot_ai_lists.h"

/* Declared with plain types rather than by including the game's headers, matching how the rest
 * of the port layer reaches into the game (see ge_player_api.c). s32 is int here. */
extern void *chrSpawnAtPad(void *self, int bodynum, int headnum, int padid,
                           const void *ailist, int flags);
extern int   gePortPlayerPos(int idx, float *out);

/* PAD_PRESET1 is the sentinel that makes chrResolvePadId dereference `self`. We pass NULL for
 * self, so a pad id equal to it would crash rather than spawn. Refused explicitly below. */
#define GE_PAD_PRESET_SENTINEL 0x3FF

static int ge_bai_ready;
static int ge_bai_done;
static int ge_bai_delay = 180;
static int ge_bai_body  = 37;
static int ge_bai_count = 1;
static int ge_bai_pad   = 1;
static const GeBotAiList *ge_bai_list;

static const GeBotAiList *ge_bai_find(const char *name, size_t len)
{
    int i;
    for (i = 0; i < GE_BOT_AI_LIST_COUNT; i++) {
        if (strlen(ge_bot_ai_lists[i].name) == len &&
            strncmp(ge_bot_ai_lists[i].name, name, len) == 0) {
            return &ge_bot_ai_lists[i];
        }
    }
    return NULL;
}

static void ge_bai_print_available(void)
{
    int i;
    printf("[getv][botai] %d archetypes available:\n", GE_BOT_AI_LIST_COUNT);
    for (i = 0; i < GE_BOT_AI_LIST_COUNT; i++) {
        printf("[getv][botai]   %-10s %-12s %u bytes\n",
               ge_bot_ai_lists[i].name, ge_bot_ai_lists[i].kind, ge_bot_ai_lists[i].len);
    }
    fflush(stdout);
}

void gePortBotAiInit(void)
{
    const char *spec;
    const char *e;
    const char *at;
    const char *colon;
    size_t namelen;

    if (ge_bai_ready) { return; }
    ge_bai_ready = 1;

    if (getenv("GETV_BOT_AI_LIST") != NULL) {
        ge_bai_print_available();
    }

    spec = getenv("GETV_BOT_AI");
    if (spec == NULL || *spec == '\0') { return; }

    /* <name>[:count][@pad] -- parsed leniently, but every field is reported back so a typo in
     * the spec shows up as the wrong number rather than as silence. */
    at    = strchr(spec, '@');
    colon = strchr(spec, ':');
    namelen = strlen(spec);
    if (colon != NULL && (size_t)(colon - spec) < namelen) { namelen = (size_t)(colon - spec); }
    if (at != NULL && (size_t)(at - spec) < namelen)       { namelen = (size_t)(at - spec); }

    ge_bai_list = ge_bai_find(spec, namelen);
    if (ge_bai_list == NULL) {
        printf("[getv][botai] no archetype named '%.*s'\n", (int) namelen, spec);
        ge_bai_print_available();
        return;
    }
    if (colon != NULL) { ge_bai_count = atoi(colon + 1); }
    if (at != NULL)    { ge_bai_pad   = atoi(at + 1); }
    if (ge_bai_count < 1)  { ge_bai_count = 1; }
    if (ge_bai_count > 16) { ge_bai_count = 16; }

    if ((e = getenv("GETV_BOT_AI_BODY"))  != NULL) { ge_bai_body  = atoi(e); }
    if ((e = getenv("GETV_BOT_AI_DELAY")) != NULL) { ge_bai_delay = atoi(e); }

    printf("[getv][botai] armed: %s (%s, %u bytes) x%d at pad %d, body %d, after %d frames\n",
           ge_bai_list->name, ge_bai_list->kind, ge_bai_list->len,
           ge_bai_count, ge_bai_pad, ge_bai_body, ge_bai_delay);
    fflush(stdout);
}

/* Called once per rendered frame, beside gePortBotFrame. */
void gePortBotAiFrame(int frame)
{
    float pos[3];
    int i, spawned = 0;

    if (!ge_bai_ready) { gePortBotAiInit(); }
    if (ge_bai_list == NULL || ge_bai_done) { return; }
    if (frame < ge_bai_delay) { return; }

    /* Only spawn into a stage that is actually running. gePortPlayerPos is the port's own
     * accessor and returns zero when there is no player in the world, which is the cheapest
     * honest test for "a level is live" available here -- spawning into a menu would either do
     * nothing or crash, and both are worse than waiting. */
    if (!gePortPlayerPos(0, pos)) { return; }

    ge_bai_done = 1;

    for (i = 0; i < ge_bai_count; i++) {
        int   pad = ge_bai_pad + i;
        void *prop;

        if (pad == GE_PAD_PRESET_SENTINEL) {
            printf("[getv][botai] refusing pad %d: it is the preset sentinel, which requires a "
                   "chr context we do not have\n", pad);
            continue;
        }

        /* The bytecode is const here but the game's parameter is not; the interpreter only
         * reads it. Cast is deliberate and local. */
        prop = chrSpawnAtPad(NULL, ge_bai_body, -1, pad,
                             (const void *) ge_bai_list->code, 0);
        if (prop != NULL) {
            spawned++;
            printf("[getv][botai] spawned %s at pad %d (body %d)\n",
                   ge_bai_list->name, pad, ge_bai_body);
        } else {
            printf("[getv][botai] spawn FAILED at pad %d -- pad out of range for this stage, or "
                   "the character budget is full\n", pad);
        }
    }

    printf("[getv][botai] %d of %d spawned on frame %d\n", spawned, ge_bai_count, frame);
    fflush(stdout);
}
