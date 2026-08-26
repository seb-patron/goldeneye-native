/* Live enemy state. See ge_enemy_api.h for what this is for and why the source is installed
 * rather than linked. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ge_enemy_api.h"

static GeEnemyCountFn ge_en_count_fn;
static GeEnemyAtFn    ge_en_at_fn;

void geEnemySourceInstall(GeEnemyCountFn count_fn, GeEnemyAtFn at_fn)
{
    /* Both or neither. A half-installed source -- a count with no accessor -- would report N
     * enemies and then fail to describe any of them, which reads to a caller as "N enemies I
     * cannot see" rather than as a wiring mistake. */
    if (count_fn == NULL || at_fn == NULL) {
        if (ge_en_count_fn != NULL) {
            printf("[getv][enemy] source uninstalled\n");
        }
        ge_en_count_fn = NULL;
        ge_en_at_fn    = NULL;
        return;
    }

    ge_en_count_fn = count_fn;
    ge_en_at_fn    = at_fn;
    printf("[getv][enemy] source installed (%d slots)\n", count_fn());
}

int geEnemySourceInstalled(void)
{
    return (ge_en_count_fn != NULL && ge_en_at_fn != NULL);
}

int geEnemyCount(void)
{
    if (!geEnemySourceInstalled()) { return 0; }
    return ge_en_count_fn();
}

/* Turn one flat row into a GeEnemy. Returns 0 for an empty slot. */
static int ge_en_decode(int index, GeEnemy *out)
{
    float f[GE_ENEMY_FIELD_COUNT];
    int   mask;

    memset(f, 0, sizeof(f));
    mask = ge_en_at_fn(index, f, GE_ENEMY_FIELD_COUNT);
    if (mask == 0) { return 0; }

    memset(out, 0, sizeof(*out));
    out->fields = (unsigned) mask;
    out->id     = (int) f[GE_ENEMY_F_ID];
    out->alive  = (f[GE_ENEMY_F_ALIVE] != 0.0f);

    if (mask & GE_EN_POSITION) {
        out->x = f[GE_ENEMY_F_X];
        out->y = f[GE_ENEMY_F_Y];
        out->z = f[GE_ENEMY_F_Z];
    }

    if (mask & GE_EN_HEALTH) {
        /* The game stores damage TAKEN and the threshold to die, not health remaining. Converting
         * here means no caller has to remember the inversion -- and a caller that forgot it would
         * read a nearly-dead guard as a healthy one, which is the wrong way round to be wrong. */
        out->max_health = f[GE_ENEMY_F_MAXDAMAGE];
        out->health     = f[GE_ENEMY_F_MAXDAMAGE] - f[GE_ENEMY_F_DAMAGE];
        if (out->health < 0.0f) { out->health = 0.0f; }
    }

    if (mask & GE_EN_ALERT) {
        out->alertness        = (int) f[GE_ENEMY_F_ALERTNESS];
        out->hearing_scale    = f[GE_ENEMY_F_HEARING];
        out->saw_target_ago   = (int) f[GE_ENEMY_F_SAW_AGO];
        out->heard_target_ago = (int) f[GE_ENEMY_F_HEARD_AGO];
    }

    if (mask & GE_EN_BELIEF) {
        out->believed_x = f[GE_ENEMY_F_BELIEF_X];
        out->believed_y = f[GE_ENEMY_F_BELIEF_Y];
        out->believed_z = f[GE_ENEMY_F_BELIEF_Z];
    }

    return 1;
}

int geEnemy(int index, GeEnemy *out)
{
    if (out == NULL || !geEnemySourceInstalled()) { return 0; }
    if (index < 0 || index >= ge_en_count_fn())   { return 0; }
    return ge_en_decode(index, out);
}

int geEnemyById(int id, GeEnemy *out)
{
    int n, i;

    if (out == NULL || !geEnemySourceInstalled()) { return 0; }

    n = ge_en_count_fn();
    for (i = 0; i < n; i++) {
        if (ge_en_decode(i, out) && out->id == id) { return 1; }
    }
    return 0;
}

/* Horizontal distance. The whole navigation layer treats the world as a floor plan -- routes,
 * waypoints and the steering law all ignore Y -- and mixing a 3D distance in here would make a
 * guard on the gantry above read as closer than one across the room. */
static float ge_en_dist2(float ax, float az, float bx, float bz)
{
    float dx = ax - bx;
    float dz = az - bz;
    return dx * dx + dz * dz;
}

int geEnemiesNear(float x, float y, float z, float radius, GeEnemy *out, int max)
{
    int   n, i, written = 0;
    float r2;

    (void) y;   /* horizontal by design; see ge_en_dist2 */

    if (out == NULL || max <= 0 || !geEnemySourceInstalled()) { return 0; }

    r2 = radius * radius;
    n  = ge_en_count_fn();

    for (i = 0; i < n; i++) {
        GeEnemy e;
        float   d2;
        int     j;

        if (!ge_en_decode(i, &e))          { continue; }
        if (!e.alive)                      { continue; }   /* a corpse is not a threat */
        if (!(e.fields & GE_EN_POSITION))  { continue; }   /* cannot place it, cannot rank it */

        d2 = ge_en_dist2(e.x, e.z, x, z);
        if (d2 > r2) { continue; }

        e.distance = (float) sqrt((double) d2);

        /* Insertion sort into the caller's array. N is the character slot count -- tens, not
         * thousands -- and keeping the result sorted as it is built avoids needing scratch space
         * for a set that is usually smaller than the array it lands in. */
        for (j = written; j > 0 && out[j - 1].distance > e.distance; j--) {
            if (j < max) { out[j] = out[j - 1]; }
        }
        if (j < max) { out[j] = e; }
        if (written < max) { written++; }
    }

    return written;
}

int geEnemyThreatAt(float x, float y, float z, float radius)
{
    int   n, i, threats = 0;
    float r2;

    (void) y;

    if (!geEnemySourceInstalled()) { return 0; }

    r2 = radius * radius;
    n  = ge_en_count_fn();

    for (i = 0; i < n; i++) {
        GeEnemy e;

        if (!ge_en_decode(i, &e))        { continue; }
        if (!e.alive)                    { continue; }
        if (!(e.fields & GE_EN_BELIEF))  { continue; }   /* no belief, no opinion about this spot */

        /* not filtered on alertness. An enemy heading to where it last saw someone
         * is a threat to that spot whether or not it is currently alert -- alertness describes its
         * state now, the belief describes where it is going. Filtering on alertness here would
         * hide exactly the guard that is about to arrive. */
        if (ge_en_dist2(e.believed_x, e.believed_z, x, z) <= r2) { threats++; }
    }

    return threats;
}
