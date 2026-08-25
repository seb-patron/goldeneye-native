/* World knowledge at runtime. See ge_world_api.h for what this is for.
 *
 * READ BY EXPLICIT OFFSET, NEVER BY CASTING TO A STRUCT.
 *
 * The file is written by tools/pack_world.py with fixed field widths and no padding. Casting a
 * byte pointer to a C struct would work on the compilers this happens to build with and break
 * silently on one that aligns differently -- every field after the first misalignment reads its
 * neighbour, and the numbers stay plausible. Positions would drift rather than crash. So each
 * field is memcpy'd from a named offset, and the offsets are stated once here next to the
 * layout they mirror.
 *
 * The load checks that the declared counts account for exactly the bytes present. That single
 * comparison catches the whole class of packer-versus-reader drift: if the packer adds a field
 * and this does not, the arithmetic stops matching and the level refuses to load rather than
 * serving quiet nonsense.
 *
 *   GETV_WORLD_DIR=<path>   where the .gew files live (default: build/world beside the exe)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_world_api.h"

/* Layout, mirroring pack_world.py. Changing either without the other is what the size check
 * exists to catch. */
#define HDR_SIZE   40
#define HDR_MAGIC   0
#define HDR_VERSION 4
#define HDR_NAME    8
#define HDR_NWP    24
#define HDR_NGD    28
#define HDR_NOB    32
#define HDR_NST    36

#define WP_SIZE 16          /* id u16, room u16, x f32, y f32, z f32 */
#define GD_SIZE 16          /* chrnum u16, room u16, x f32, y f32, z f32 */
#define OB_SIZE 24          /* idx u16, diff u16, targets u16, steps u16, first u32, x y z f32 */
#define ST_SIZE 24          /* from u16, to u16, dist f32, heading f32, turn f32, pad f32,
                               threats u16, 2 pad */

static struct {
    unsigned char *blob;
    long           size;
    char           level[24];
    int            nwp, ngd, nob, nst;
    long           owp, ogd, oob, ost;      /* byte offsets of each table */
} ge_w;

static unsigned int rd_u32(const unsigned char *p)
{
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8) |
           ((unsigned int) p[2] << 16) | ((unsigned int) p[3] << 24);
}

static int rd_u16(const unsigned char *p)
{
    return (int) ((unsigned int) p[0] | ((unsigned int) p[1] << 8));
}

static float rd_f32(const unsigned char *p)
{
    /* Through memcpy rather than a pointer cast: the bytes are little-endian IEEE-754 and every
     * platform the port targets is too, but aliasing a float through an unsigned char* cast is
     * undefined behaviour that compilers do optimise on. */
    float f;
    unsigned int u = rd_u32(p);
    memcpy(&f, &u, sizeof f);
    return f;
}

void geWorldUnload(void)
{
    if (ge_w.blob != NULL) { free(ge_w.blob); }
    memset(&ge_w, 0, sizeof ge_w);
}

int geWorldLoad(const char *level)
{
    char path[512];
    const char *dir;
    FILE *f;
    long size;
    unsigned char *blob;
    unsigned int magic, version;
    long need;

    geWorldUnload();
    if (level == NULL || *level == '\0') { return 0; }

    dir = getenv("GETV_WORLD_DIR");
    if (dir == NULL || *dir == '\0') { dir = "build/world"; }
    snprintf(path, sizeof path, "%s/%s.gew", dir, level);

    f = fopen(path, "rb");
    if (f == NULL) {
        /* Not an error worth shouting about: four levels have no background model and several
         * have no routed objectives. A caller must cope with knowing nothing. */
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < HDR_SIZE) { fclose(f); return 0; }

    blob = (unsigned char *) malloc((size_t) size);
    if (blob == NULL) { fclose(f); return 0; }
    if (fread(blob, 1, (size_t) size, f) != (size_t) size) {
        free(blob);
        fclose(f);
        return 0;
    }
    fclose(f);

    magic   = rd_u32(blob + HDR_MAGIC);
    version = rd_u32(blob + HDR_VERSION);
    if (magic != GE_WORLD_MAGIC) {
        printf("[getv][world] %s: not a world file (magic %08x)\n", path, magic);
        free(blob);
        return 0;
    }
    if (version != GE_WORLD_VERSION) {
        printf("[getv][world] %s: version %u, expected %u -- repack with tools/pack_world.py\n",
               path, version, (unsigned) GE_WORLD_VERSION);
        free(blob);
        return 0;
    }

    ge_w.nwp = (int) rd_u32(blob + HDR_NWP);
    ge_w.ngd = (int) rd_u32(blob + HDR_NGD);
    ge_w.nob = (int) rd_u32(blob + HDR_NOB);
    ge_w.nst = (int) rd_u32(blob + HDR_NST);

    /* The declared counts must account for exactly the bytes present. This is the drift check:
     * if the packer gains a field and this reader does not, the totals stop agreeing and the
     * level refuses rather than reading every subsequent field one slot out. */
    need = (long) HDR_SIZE
         + (long) ge_w.nwp * WP_SIZE + (long) ge_w.ngd * GD_SIZE
         + (long) ge_w.nob * OB_SIZE + (long) ge_w.nst * ST_SIZE;
    if (need != size) {
        printf("[getv][world] %s: layout mismatch -- %ld bytes on disk, %ld implied by counts "
               "(%d/%d/%d/%d). Reader and packer disagree; repack.\n",
               path, size, need, ge_w.nwp, ge_w.ngd, ge_w.nob, ge_w.nst);
        free(blob);
        memset(&ge_w, 0, sizeof ge_w);
        return 0;
    }

    ge_w.blob = blob;
    ge_w.size = size;
    memcpy(ge_w.level, blob + HDR_NAME, 16);
    ge_w.level[16] = '\0';
    ge_w.owp = HDR_SIZE;
    ge_w.ogd = ge_w.owp + (long) ge_w.nwp * WP_SIZE;
    ge_w.oob = ge_w.ogd + (long) ge_w.ngd * GD_SIZE;
    ge_w.ost = ge_w.oob + (long) ge_w.nob * OB_SIZE;

    printf("[getv][world] %s: %d waypoints, %d guards, %d objectives, %d steps (%ld bytes)\n",
           ge_w.level, ge_w.nwp, ge_w.ngd, ge_w.nob, ge_w.nst, size);
    fflush(stdout);
    return 1;
}

int geWorldLoaded(void)          { return ge_w.blob != NULL; }
const char *geWorldLevel(void)   { return ge_w.blob ? ge_w.level : ""; }
int geWorldWaypointCount(void)   { return ge_w.nwp; }
int geWorldGuardCount(void)      { return ge_w.ngd; }
int geWorldObjectiveCount(void)  { return ge_w.nob; }
int geWorldStepCount(void)       { return ge_w.nst; }

int geWorldWaypoint(int i, GeWorldWaypoint *out)
{
    const unsigned char *p;
    if (ge_w.blob == NULL || out == NULL || i < 0 || i >= ge_w.nwp) { return 0; }
    p = ge_w.blob + ge_w.owp + (long) i * WP_SIZE;
    out->id   = rd_u16(p + 0);
    out->room = rd_u16(p + 2);
    out->x    = rd_f32(p + 4);
    out->y    = rd_f32(p + 8);
    out->z    = rd_f32(p + 12);
    return 1;
}

int geWorldGuard(int i, GeWorldGuard *out)
{
    const unsigned char *p;
    if (ge_w.blob == NULL || out == NULL || i < 0 || i >= ge_w.ngd) { return 0; }
    p = ge_w.blob + ge_w.ogd + (long) i * GD_SIZE;
    out->chrnum = rd_u16(p + 0);
    out->room   = rd_u16(p + 2);
    out->x      = rd_f32(p + 4);
    out->y      = rd_f32(p + 8);
    out->z      = rd_f32(p + 12);
    return 1;
}

int geWorldObjective(int i, GeWorldObjective *out)
{
    const unsigned char *p;
    if (ge_w.blob == NULL || out == NULL || i < 0 || i >= ge_w.nob) { return 0; }
    p = ge_w.blob + ge_w.oob + (long) i * OB_SIZE;
    out->index          = rd_u16(p + 0);
    out->min_difficulty = rd_u16(p + 2);
    out->targets        = rd_u16(p + 4);
    out->steps          = rd_u16(p + 6);
    out->first_step     = (int) rd_u32(p + 8);
    out->tx             = rd_f32(p + 12);
    out->ty             = rd_f32(p + 16);
    out->tz             = rd_f32(p + 20);
    return 1;
}

int geWorldStep(int i, GeWorldStep *out)
{
    const unsigned char *p;
    if (ge_w.blob == NULL || out == NULL || i < 0 || i >= ge_w.nst) { return 0; }
    p = ge_w.blob + ge_w.ost + (long) i * ST_SIZE;
    out->from     = rd_u16(p + 0);
    out->to       = rd_u16(p + 2);
    out->distance = rd_f32(p + 4);
    out->heading  = rd_f32(p + 8);
    out->turn     = rd_f32(p + 12);
    /* p + 16 is reserved padding in the format, deliberately not read. */
    out->threats  = rd_u16(p + 20);
    return 1;
}

int geWorldRouteStep(int objective, int n, GeWorldStep *out)
{
    GeWorldObjective ob;
    int i;
    if (!geWorldObjective(objective, &ob)) { return 0; }
    if (n < 0 || n >= ob.steps) { return 0; }
    i = ob.first_step + n;
    return geWorldStep(i, out);
}

int geWorldWaypointById(int id, GeWorldWaypoint *out)
{
    GeWorldWaypoint w;
    int i;
    if (ge_w.blob == NULL || out == NULL) { return 0; }
    /* Linear rather than indexed: ids are not row numbers, the tables are a couple of hundred
     * rows, and a lookup happens once per step rather than once per frame. */
    for (i = 0; i < ge_w.nwp; i++) {
        if (geWorldWaypoint(i, &w) && w.id == id) { *out = w; return 1; }
    }
    return 0;
}

int geWorldNearestWaypoint(float x, float y, float z, GeWorldWaypoint *out)
{
    GeWorldWaypoint w, best;
    float bestd = 0.0f;
    int i, found = 0;

    if (ge_w.blob == NULL || out == NULL) { return 0; }
    for (i = 0; i < ge_w.nwp; i++) {
        float dx, dy, dz, d;
        if (!geWorldWaypoint(i, &w)) { continue; }
        dx = w.x - x; dy = w.y - y; dz = w.z - z;
        d = dx * dx + dy * dy + dz * dz;        /* squared: no sqrt needed to compare */
        if (!found || d < bestd) { best = w; bestd = d; found = 1; }
    }
    if (!found) { return 0; }
    *out = best;
    return 1;
}

int geWorldGuardsNear(float x, float y, float z, float radius,
                      GeWorldGuard *out, int max)
{
    GeWorldGuard g;
    float r2;
    int i, n = 0;

    if (ge_w.blob == NULL || out == NULL || max <= 0) { return 0; }
    r2 = radius * radius;
    for (i = 0; i < ge_w.ngd; i++) {
        float dx, dy, dz, d;
        int j;
        if (!geWorldGuard(i, &g)) { continue; }
        dx = g.x - x; dy = g.y - y; dz = g.z - z;
        d = dx * dx + dy * dy + dz * dz;
        if (d > r2) { continue; }

        /* Insertion sort by distance. The counts are tens, not thousands, and a caller asking
         * for the three nearest wants them in order rather than in table order. */
        for (j = n; j > 0; j--) {
            float pdx = out[j - 1].x - x, pdy = out[j - 1].y - y, pdz = out[j - 1].z - z;
            if (pdx * pdx + pdy * pdy + pdz * pdz <= d) { break; }
            if (j < max) { out[j] = out[j - 1]; }
        }
        if (j < max) { out[j] = g; }
        if (n < max) { n++; }
    }
    return n;
}
