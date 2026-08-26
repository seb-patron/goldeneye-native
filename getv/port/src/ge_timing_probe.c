/* Constant-input probe for frame-timing measurements.
 *
 * The timing work needs a way to move the player that does not depend on what any policy decides,
 * because a follower's choices vary run to run and comparing distance across clock settings then
 * measures the follower as much as the clock. This holds full forward and logs position, the
 * game clock and the wall clock.
 *
 * It lives in its own file because it kept being lost. It was written three times inside
 * ge_bot_route.c and removed three times by an integration from the other tree, once taking the
 * wall-clock timestamp with it and leaving a log that timed everything with processor time.
 *
 *   GETV_TIMING_PROBE=<slot>   drive this slot forward and log
 *   GETV_TIMING_LOG=<path>     where to write, default build/timing-probe.tsv
 *
 * Columns: frame, wall ms, x, z, g_GlobalTimer. Game time is counted in video fields, so a
 * correct clock gives 60 a second whatever the renderer is doing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_player_api.h"

extern unsigned int gePortHostMillis(void);
extern int  g_GlobalTimer;
extern int  gePortPlayerPos(int idx, float *out);

static FILE *ge_tp_log;
static int   ge_tp_slot = -2;

void gePortTimingProbeFrame(int frame)
{
    GePlayerInput in;
    float pos[3] = { 0.0f, 0.0f, 0.0f };

    if (ge_tp_slot == -2) {
        const char *e = getenv("GETV_TIMING_PROBE");
        ge_tp_slot = (e && *e) ? atoi(e) : -1;
        if (ge_tp_slot >= 0) {
            const char *path = getenv("GETV_TIMING_LOG");
            if (path == NULL || *path == '\0') { path = "build/timing-probe.tsv"; }
            ge_tp_log = fopen(path, "w");
            if (ge_tp_log != NULL) {
                fprintf(ge_tp_log, "frame\tms\tx\tz\tgametime\n");
            }
            printf("[getv][timing] probe driving slot %d, logging to %s\n", ge_tp_slot, path);
            fflush(stdout);
        }
    }
    if (ge_tp_slot < 0 || ge_tp_log == NULL) { return; }

    gePortPlayerPos(ge_tp_slot, pos);

    memset(&in, 0, sizeof in);
    in.stick_y = 60;                       /* full forward, the walk deadzone takes about 5 */
    gePlayerPost(ge_tp_slot, gePlayerTick() + 1, &in, 1);

    fprintf(ge_tp_log, "%d\t%u\t%.0f\t%.0f\t%d\n",
            frame, gePortHostMillis(), (double) pos[0], (double) pos[2], g_GlobalTimer);
    if ((frame & 255) == 0) { fflush(ge_tp_log); }
}
