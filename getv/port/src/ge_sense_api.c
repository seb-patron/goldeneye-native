/* See ge_sense_api.h. Thin on purpose: the engine already knows all of this, and every line here
 * that reimplements rather than asks is a line that can disagree with the game. */
#include <math.h>
#include <string.h>

#include "ge_sense_api.h"
#include "ge_enemy_api.h"
#include "ge_player_api.h"   /* GePlayerState, gePlayerStateGet, GE_ST_POSITION, GE_MAX_SLOTS */

extern int gePortSenseLine(float from_x, float from_z, float to_x, float to_z);
extern int gePortSenseVisibleTo(int chr_index, int player_index);

#define GE_SENSE_SAMPLES 6

/* How far out the first segment starts: roughly a body's own width. What is already touching the
 * bot is not information about which way to go, and including it makes every direction blocked. */
#define GE_SENSE_SKIN    35.0f

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
    /* Each segment is tested from the LAST CLEAR POINT, not from the origin.
     *
     * Testing every sample from the start point makes anything near the body block the whole ray,
     * in every direction at once -- a bot pressed against a crate reported OBJECT at 50 units on
     * all twelve headings and concluded it was walled in, because each ray began inside the thing
     * it was asking about. Stepping outward asks "can I get from here to the next step", which is
     * the question a walking body actually has.
     *
     * The first segment starts a body-width out for the same reason: whatever is already touching
     * the bot is not information about which way to go. */
    {
        float px = x + sn * GE_SENSE_SKIN, pz = z + cs * GE_SENSE_SKIN;

        out->x = px;
        out->z = pz;

        for (i = 1; i <= GE_SENSE_SAMPLES; i++) {
            float d = GE_SENSE_SKIN
                    + ((reach - GE_SENSE_SKIN) * ((float) i / (float) GE_SENSE_SAMPLES));
            float tx = x + sn * d, tz = z + cs * d;
            unsigned int hit = geSenseLine(px, pz, tx, tz);

            if (hit != GE_SENSE_CLEAR) {
                out->what = hit;
                out->distance = d;
                return 1;
            }
            px = tx;
            pz = tz;
            out->x = tx;
            out->z = tz;
        }
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
            if (geSenseAhead(x, z, h, reach, &c) && (c.what & GE_SENSE_SOLID) == 0) { return h; }
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

/* ---------------------------------------------------------------- 1a: attention
 *
 * REQUIRED GAME-SIDE ACCESSOR. A view cone needs the character's facing, and nothing exposes it.
 * Declared here the same way gePortSenseLine is, so the shape is agreed before the shim exists:
 *
 *     int gePortEnemyFacing(int chr_index, float *out_deg);
 *
 * Degrees, atan2(x, z), matching every other angle in this layer. Returns 0 when the character has
 * no facing to report, which is a real state for one that has not spawned.
 */
extern int gePortEnemyFacing(int chr_index, float *out_deg);

static float ge_norm180(float a)
{
    while (a > 180.0f)  { a -= 360.0f; }
    while (a < -180.0f) { a += 360.0f; }
    return a;
}

unsigned int geSenseNoticedBy(int enemy_index, int player_slot)
{
    GeEnemy e;
    GePlayerState st;
    unsigned int bits = GE_NOTICE_NONE;
    float facing, bearing;

    if (!geEnemySourceInstalled())                    { return GE_NOTICE_NONE; }
    if (!geEnemy(enemy_index, &e) || !e.alive)        { return GE_NOTICE_NONE; }
    if (!gePlayerStateGet(player_slot, &st) || !st.present) { return GE_NOTICE_NONE; }
    if (!(st.fields & GE_ST_POSITION))                { return GE_NOTICE_NONE; }
    if (!(e.fields & GE_EN_POSITION))                 { return GE_NOTICE_NONE; }

    if (geSenseVisibleTo(enemy_index, player_slot)) { bits |= GE_NOTICE_LINE; }

    /* Alertness is the game's own idea of whether this character is paying attention. Anything
     * above zero counts: a guard part-way to alarmed is still looking, and demanding a high value
     * would make a bot confident in front of someone who is halfway to shooting it. */
    if ((e.fields & GE_EN_ALERT) && e.alertness > 0) { bits |= GE_NOTICE_ALERT; }

    /* THE CONE. Absent facing is reported as UNKNOWN rather than as facing-away, because those
     * lead to opposite behaviour: one means walk behind it, the other means do not assume you
     * can. A build without the shim must not read as "nobody is looking". */
    if (gePortEnemyFacing(enemy_index, &facing)) {
        bearing = (float)(atan2((double)(st.x - e.x), (double)(st.z - e.z)) * 180.0 / 3.14159265358979);
        if (fabsf(ge_norm180(bearing - facing)) <= GE_NOTICE_CONE_DEG) {
            bits |= GE_NOTICE_FACING;
        }
    } else {
        bits |= GE_NOTICE_FACE_UNKNOWN;
    }

    return bits;
}

int geSenseNoticing(int player_slot)
{
    int i, n, seen = 0;

    if (!geEnemySourceInstalled()) { return 0; }
    n = geEnemyCount();
    for (i = 0; i < n; i++) {
        if ((geSenseNoticedBy(i, player_slot) & GE_NOTICE_SEEN) == GE_NOTICE_SEEN) { seen++; }
    }
    return seen;
}

/* ---------------------------------------------------------------- 1b: contact */

#define GE_CONTACT_HISTORY 16
#define GE_CONTACT_EPSILON 1.5f     /* below this a move is noise, not travel */

static struct {
    float x[GE_CONTACT_HISTORY];
    float z[GE_CONTACT_HISTORY];
    unsigned char moved[GE_CONTACT_HISTORY];   /* was movement COMMANDED on this tick */
    int   n;                                    /* how many entries are valid */
    int   head;
} ge_contact[GE_MAX_SLOTS];

void geSenseContactUpdate(int player_slot, float x, float z, int commanded_move)
{
    int h;
    if (player_slot < 0 || player_slot >= GE_MAX_SLOTS) { return; }
    h = ge_contact[player_slot].head;
    ge_contact[player_slot].x[h] = x;
    ge_contact[player_slot].z[h] = z;
    ge_contact[player_slot].moved[h] = (unsigned char)(commanded_move ? 1 : 0);
    ge_contact[player_slot].head = (h + 1) % GE_CONTACT_HISTORY;
    if (ge_contact[player_slot].n < GE_CONTACT_HISTORY) { ge_contact[player_slot].n++; }
}

int geSenseIsStuck(int player_slot, int ticks)
{
    int i, h, prev, commanded = 0;
    float dx, dz, moved = 0.0f;

    if (player_slot < 0 || player_slot >= GE_MAX_SLOTS) { return 0; }
    if (ticks < 2) { ticks = 2; }
    if (ticks > GE_CONTACT_HISTORY) { ticks = GE_CONTACT_HISTORY; }
    if (ge_contact[player_slot].n < ticks) { return 0; }   /* not enough history to say */

    for (i = 1; i < ticks; i++) {
        h = (ge_contact[player_slot].head - i + GE_CONTACT_HISTORY) % GE_CONTACT_HISTORY;
        prev = (h - 1 + GE_CONTACT_HISTORY) % GE_CONTACT_HISTORY;
        dx = ge_contact[player_slot].x[h] - ge_contact[player_slot].x[prev];
        dz = ge_contact[player_slot].z[h] - ge_contact[player_slot].z[prev];
        moved += (float) sqrt((double)(dx * dx + dz * dz));
        if (ge_contact[player_slot].moved[h]) { commanded++; }
    }

    /* Both halves are required. A bot standing still on purpose is not stuck, and a bot moving
     * freely is not stuck however long it has been going. Only "told to move and did not" is. */
    return (commanded >= ticks - 1) && (moved < GE_CONTACT_EPSILON * (float)(ticks - 1));
}

float geSenseRecentTravel(int player_slot)
{
    int i, h, prev, n;
    float dx, dz, moved = 0.0f;

    if (player_slot < 0 || player_slot >= GE_MAX_SLOTS) { return 0.0f; }
    n = ge_contact[player_slot].n;
    if (n < 2) { return 0.0f; }
    for (i = 1; i < n; i++) {
        h = (ge_contact[player_slot].head - i + GE_CONTACT_HISTORY) % GE_CONTACT_HISTORY;
        prev = (h - 1 + GE_CONTACT_HISTORY) % GE_CONTACT_HISTORY;
        dx = ge_contact[player_slot].x[h] - ge_contact[player_slot].x[prev];
        dz = ge_contact[player_slot].z[h] - ge_contact[player_slot].z[prev];
        moved += (float) sqrt((double)(dx * dx + dz * dz));
    }
    return moved;
}

/* ---------------------------------------------------------------- 1c: a body, not a line */

int geSenseAheadForBody(float x, float z, float heading_deg, float reach, GeSenseContact *out)
{
    GeSenseContact centre, side;
    float rad = ge_deg2rad(heading_deg);
    float px = (float) cos((double) rad);      /* perpendicular to the heading */
    float pz = -(float) sin((double) rad);
    int i;

    if (out == NULL) { return 0; }
    geSenseAhead(x, z, heading_deg, reach, &centre);
    *out = centre;

    /* Three lines: centre, and one a body-radius out on each side. The NEAREST obstruction of the
     * three wins, because a body stops at whichever shoulder meets something first. Sampling only
     * the centre is what lets a follower walk confidently into a gap narrower than it is. */
    for (i = -1; i <= 1; i += 2) {
        float ox = x + px * GE_BODY_RADIUS * (float) i;
        float oz = z + pz * GE_BODY_RADIUS * (float) i;
        geSenseAhead(ox, oz, heading_deg, reach, &side);
        /* GE_SENSE_SOLID, not "anything at all". The line starts at the asking position, so the
         * asker's own collision sets GE_SENSE_BODY on every reading -- judging on != CLEAR made
         * all three lines report blocked everywhere, which is the bug that had
         * geSenseClearestHeading finding nothing open on any map. A shoulder with a person in it
         * is still a shoulder a body can move through; a wall or a crate is not. */
        if ((side.what & GE_SENSE_SOLID) != 0 &&
            ((out->what & GE_SENSE_SOLID) == 0 || side.distance < out->distance)) {
            out->what = side.what;
            out->distance = side.distance;
            /* The stopping point stays the CENTRE line's last clear point: that is where the
             * body's middle can reach, which is what a caller steers toward. A shoulder's clear
             * point is beside the path, not on it. */
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- 1d: what can I act on
 *
 * REQUIRED GAME-SIDE ACCESSOR, same arrangement as the facing one above:
 *
 *     int gePortUsableAt(int index, float *out);   out[0..2] xyz, out[3] kind, out[4] prop id
 *     int gePortUsableCount(void);
 *
 * The prop table knows where doors, keys and switches are; only the game knows which of them are
 * live right now -- a collected key is still in the table and is no longer there to pick up.
 * Reading the static export instead would send a bot to fetch something it already has.
 */
extern int gePortUsableCount(void);
extern int gePortUsableAt(int index, float *out);

/* WEAK FALLBACKS so the port links before the game side exists.
 *
 * These are the ONLY two accessors it is safe to default, and the distinction matters. A
 * placeholder gePortPlayerMovePad would have silently disabled the two-pad movement fix while the
 * tree looked merged -- a build that runs and quietly does the wrong thing, which is worse than
 * one that will not link. Refusing to stub that was right.
 *
 * These answer "nothing is within reach", which geSenseUsable already treats as a legitimate
 * state and which every caller must handle anyway. The failure mode is a bot that never notices a
 * door, not a bot that acts on a door that is not there.
 *
 * Weak, so mac-getv's real implementations override them at link with no edit here. When they
 * land these become dead and should be deleted -- a fallback that outlives its reason is how a
 * subsystem quietly stays switched off.
 */
/* test_sense.c includes this file and supplies its own definitions so it can drive a fake
 * population; both would land in one translation unit. It defines GE_SENSE_NO_WEAK_USABLE to
 * suppress these rather than lose that coverage. */
#if defined(__GNUC__) && !defined(GE_SENSE_NO_WEAK_USABLE)
__attribute__((weak)) int gePortUsableCount(void)
{
    return 0;
}

__attribute__((weak)) int gePortUsableAt(int index, float *out)
{
    (void) index;
    (void) out;
    return 0;
}
#endif

int geSenseUsable(float x, float y, float z, GeUsable *out, int max)
{
    int i, n, written = 0;
    float r2 = GE_USABLE_REACH * GE_USABLE_REACH;

    (void) y;   /* horizontal reach, like the rest of the navigation layer */

    if (out == NULL || max <= 0) { return 0; }
    n = gePortUsableCount();
    for (i = 0; i < n && written < max; i++) {
        float f[5], dx, dz, d2;
        int j;
        if (!gePortUsableAt(i, f)) { continue; }
        dx = f[0] - x;
        dz = f[2] - z;
        d2 = dx * dx + dz * dz;
        if (d2 > r2) { continue; }

        /* Nearest first: a bot reaching for something wants the closest one, and a caller that
         * takes out[0] should get the right answer without sorting. */
        for (j = written; j > 0 && out[j - 1].distance > (float) sqrt((double) d2); j--) {
            out[j] = out[j - 1];
        }
        out[j].kind = (unsigned int) f[3];
        out[j].prop = (int) f[4];
        out[j].x = f[0];
        out[j].y = f[1];
        out[j].z = f[2];
        out[j].distance = (float) sqrt((double) d2);
        written++;
    }
    return written;
}
