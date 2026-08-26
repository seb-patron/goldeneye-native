/* WHAT THE BOT LEARNED LAST TIME.
 *
 * Every run started from nothing. The bot would reach the same doorway on Train, fail the same
 * way, and the next run would walk into it again with no idea it had ever been there -- for hours,
 * because a process that forgets cannot improve, it can only be improved. Whatever the follower
 * does, it should at least not have to rediscover where the hard places are.
 *
 * So the hard places are written down. A stall -- commanded forward, barely moving, for long
 * enough that it is not just a bump -- records its position. The next run loads them and treats
 * them as repellers: a heading that leads into a place the bot has already died on is a heading
 * it needs a better reason to choose.
 *
 * ⚠️ A REPELLER IS A BIAS, NOT A BAN. The stuck place is very often ON the route -- Train's are
 * doorways, which the bot MUST pass through -- so refusing to go there would trade a bot that
 * stalls for one that never arrives. It biases the choice between otherwise-equal headings and
 * gets out of the way when there is no alternative.
 *
 * Points also carry what RESOLVED them, when something did. "Stuck here, and last time the thing
 * that worked was asking the door to open" is worth far more than the position alone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ge_bot_memory.h"

#define GE_MEM_MAX   256
#define GE_MEM_MERGE 180.0f   /* two stalls closer than this are the same place, not two places */

typedef struct {
    float x, z;
    int   hits;
    char  fix[24];
} GeMemPoint;

static GeMemPoint ge_mem[GE_MEM_MAX];
static int        ge_mem_n = 0;
static char       ge_mem_path[512];
static int        ge_mem_dirty = 0;

void geBotMemoryLoad(const char *level)
{
    const char *dir = getenv("GETV_MEM_DIR");
    FILE *f;
    char line[256];

    ge_mem_n = 0;
    ge_mem_dirty = 0;
    if (level == NULL || *level == '\0') { return; }
    if (dir == NULL || *dir == '\0') { dir = "build/levels"; }
    snprintf(ge_mem_path, sizeof ge_mem_path, "%s/%s.stuck.tsv", dir, level);

    f = fopen(ge_mem_path, "r");
    if (f == NULL) {
        printf("[getv][memory] %s: nothing learned yet\n", level);
        fflush(stdout);
        return;
    }
    while (fgets(line, sizeof line, f) != NULL && ge_mem_n < GE_MEM_MAX) {
        float x, z;
        int hits;
        char fix[24];
        fix[0] = '\0';
        if (sscanf(line, "%f\t%f\t%d\t%23s", &x, &z, &hits, fix) >= 3) {
            ge_mem[ge_mem_n].x = x;
            ge_mem[ge_mem_n].z = z;
            ge_mem[ge_mem_n].hits = hits;
            snprintf(ge_mem[ge_mem_n].fix, sizeof ge_mem[ge_mem_n].fix, "%s", fix);
            ge_mem_n++;
        }
    }
    fclose(f);
    printf("[getv][memory] %s: %d place(s) this bot has been stuck before\n", level, ge_mem_n);
    fflush(stdout);
}

void geBotMemorySave(void)
{
    FILE *f;
    int i;

    if (!ge_mem_dirty || ge_mem_path[0] == '\0') { return; }
    f = fopen(ge_mem_path, "w");
    if (f == NULL) { return; }
    for (i = 0; i < ge_mem_n; i++) {
        fprintf(f, "%.0f\t%.0f\t%d\t%s\n", (double) ge_mem[i].x, (double) ge_mem[i].z,
                ge_mem[i].hits, ge_mem[i].fix[0] ? ge_mem[i].fix : "-");
    }
    fclose(f);
    ge_mem_dirty = 0;
}

void geBotMemoryRecord(float x, float z, const char *fix)
{
    int i;

    /* Merge with a place already known, so a hundred frames of the same stall is one lesson with
     * a rising count rather than a hundred entries that crowd out every other memory. */
    for (i = 0; i < ge_mem_n; i++) {
        float dx = x - ge_mem[i].x, dz = z - ge_mem[i].z;
        if (dx * dx + dz * dz < GE_MEM_MERGE * GE_MEM_MERGE) {
            ge_mem[i].hits++;
            if (fix != NULL && *fix) {
                snprintf(ge_mem[i].fix, sizeof ge_mem[i].fix, "%s", fix);
            }
            ge_mem_dirty = 1;
            return;
        }
    }
    if (ge_mem_n >= GE_MEM_MAX) { return; }
    ge_mem[ge_mem_n].x = x;
    ge_mem[ge_mem_n].z = z;
    ge_mem[ge_mem_n].hits = 1;
    snprintf(ge_mem[ge_mem_n].fix, sizeof ge_mem[ge_mem_n].fix, "%s", (fix && *fix) ? fix : "-");
    ge_mem_n++;
    ge_mem_dirty = 1;
}

int geBotMemoryCount(void) { return ge_mem_n; }

/* How bad is it to walk from here toward there? 0 when nothing has ever gone wrong along the way,
 * rising with how many times the bot has been stuck near the destination and how close it passes.
 * Deliberately a scalar rather than a veto: see the note at the top. */
float geBotMemoryPenalty(float x, float z, float to_x, float to_z)
{
    float worst = 0.0f;
    int i;

    for (i = 0; i < ge_mem_n; i++) {
        float dx = to_x - ge_mem[i].x, dz = to_z - ge_mem[i].z;
        float d = sqrtf(dx * dx + dz * dz);
        float p;

        if (d > 400.0f) { continue; }
        p = (1.0f - (d / 400.0f)) * (float) ge_mem[i].hits;
        if (p > worst) { worst = p; }
    }
    (void) x; (void) z;
    return worst;
}

const char *geBotMemoryFixNear(float x, float z)
{
    int i;
    for (i = 0; i < ge_mem_n; i++) {
        float dx = x - ge_mem[i].x, dz = z - ge_mem[i].z;
        if (dx * dx + dz * dz < 400.0f * 400.0f && ge_mem[i].fix[0] && ge_mem[i].fix[0] != '-') {
            return ge_mem[i].fix;
        }
    }
    return NULL;
}
