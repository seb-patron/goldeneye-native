/* See ge_sense_api.h. Thin on purpose: the engine already knows all of this, and every line here
 * that reimplements rather than asks is a line that can disagree with the game. */
#include <math.h>
#include <string.h>

#include "ge_sense_api.h"
#include "ge_enemy_api.h"

extern int gePortSenseLine(float from_x, float from_z, float to_x, float to_z);
extern int gePortSenseVisibleTo(int chr_index, int player_index);

#define GE_SENSE_SAMPLES 6

static float ge_deg2rad(float d) { return d * 3.14159265358979f / 180.0f; }

unsigned int geSenseLine(float from_x, float from_z, float to_x, float to_z)
{
    return (unsigned int) gePortSenseLine(from_x, from_z, to_x, to_z);
}

int geSenseAhead(float x, float z, float heading_deg, float reach, GeSenseContact *out)
{
    float a = ge_deg2rad(heading_deg);
    float sn = (float) sin((double) a), cs = (float) cos((double) a);
    int i;

    if (out == NULL) { return 0; }
    memset(out, 0, sizeof *out);
    out->x = x;
    out->z = z;
    out->distance = reach;

    /* Sampled outward rather than one test to the far end, because "blocked somewhere along
     * here" is not actionable and "blocked in 60 units" is. The first blocked sample also gives
     * the last CLEAR point, which is where a body would actually come to rest. */
    for (i = 1; i <= GE_SENSE_SAMPLES; i++) {
        float d = reach * ((float) i / (float) GE_SENSE_SAMPLES);
        float tx = x + sn * d, tz = z + cs * d;
        unsigned int hit = geSenseLine(x, z, tx, tz);

        if (hit != GE_SENSE_CLEAR) {
            out->what = hit;
            out->distance = d;
            return 1;
        }
        out->x = tx;
        out->z = tz;
    }
    return 1;
}

float geSenseClearestHeading(float x, float z, float heading_deg, float span, float reach)
{
    /* Outward from straight ahead in both directions, so the smallest correction that works is
     * the one returned. A sweep that scanned left-to-right would swing a bot across the corridor
     * to take the first opening it happened to meet. */
    static const float step[9] = { 0.0f, 20.0f, 40.0f, 60.0f, 90.0f, 120.0f, 145.0f, 165.0f, 180.0f };
    int i, sgn;

    for (i = 0; i < 9; i++) {
        if (step[i] > span) { break; }
        for (sgn = 1; sgn >= -1; sgn -= 2) {
            GeSenseContact c;
            float h = heading_deg + (step[i] * (float) sgn);
            if (geSenseAhead(x, z, h, reach, &c) && c.what == GE_SENSE_CLEAR) { return h; }
            if (step[i] == 0.0f) { break; }   /* straight ahead has no mirror */
        }
    }
    /* Nothing open. Returning the input rather than an arbitrary direction keeps the caller's
     * own fallback in charge instead of quietly substituting ours. */
    return heading_deg;
}

int geSenseVisibleTo(int enemy_index, int player_slot)
{
    return gePortSenseVisibleTo(enemy_index, player_slot);
}

int geSenseWatchers(int player_slot)
{
    int i, n, seen = 0;

    if (!geEnemySourceInstalled()) { return 0; }
    n = geEnemyCount();
    for (i = 0; i < n; i++) {
        GeEnemy e;
        if (!geEnemy(i, &e) || !e.alive) { continue; }
        if (geSenseVisibleTo(i, player_slot)) { seen++; }
    }
    return seen;
}
