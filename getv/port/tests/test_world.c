/* The world knowledge reader.
 *
 * ge_world_api reads a packed binary by EXPLICIT OFFSET. That is the right shape for a format
 * written by one tool and read by another, and it has one failure mode that matters: if
 * pack_world.py gains a field and this reader does not, every read after that field lands one
 * slot out. Nothing crashes. Waypoints get a neighbour's coordinates, objectives point at the
 * wrong step, and the level looks subtly wrong rather than broken.
 *
 * The loader's defence is a total-size check -- the declared counts must account for exactly the
 * bytes present -- and THAT is what most of this file tests, because it is the assertion standing
 * between a format change and a silently wrong world.
 *
 * The files are synthesised here rather than taken from build/world, so this tests the FORMAT
 * CONTRACT rather than whatever the packer last happened to emit. A test that reads the packer's
 * output would agree with the packer by construction, including when both are wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_world_api.c"

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %-50s %d\n", what, got);
    } else {
        printf("  FAIL  %-50s got %d want %d\n", what, got, want);
        failures++;
    }
}

static void checkf(const char *what, float got, float want)
{
    float d = got - want;
    if (d < 0) { d = -d; }
    if (d < 0.01f) { printf("  ok    %-50s %.1f\n", what, (double) got); }
    else { printf("  FAIL  %-50s got %.2f want %.2f\n", what, (double) got, (double) want);
           failures++; }
}

/* ---------------------------------------------------------------- building a .gew */

static unsigned char buf[4096];
static int blen;

static void put_u32(int off, unsigned int v) { memcpy(buf + off, &v, 4); }
static void put_u16(int off, unsigned short v) { memcpy(buf + off, &v, 2); }
static void put_f32(int off, float v) { memcpy(buf + off, &v, 4); }

/* Two waypoints, one guard, one objective, one step. Small enough to assert every field. */
static void build(unsigned int magic, unsigned int version, int fudge_counts)
{
    int o;
    memset(buf, 0, sizeof buf);

    put_u32(HDR_MAGIC, magic);
    put_u32(HDR_VERSION, version);
    memcpy(buf + HDR_NAME, "testlevel", 9);
    put_u32(HDR_NWP, 2 + (unsigned) fudge_counts);   /* fudge makes counts disagree with size */
    put_u32(HDR_NGD, 1);
    put_u32(HDR_NOB, 1);
    put_u32(HDR_NST, 1);

    o = HDR_SIZE;
    put_u16(o + 0, 10); put_u16(o + 2, 3);
    put_f32(o + 4, 100.0f); put_f32(o + 8, 0.0f); put_f32(o + 12, 0.0f);
    o += WP_SIZE;
    put_u16(o + 0, 20); put_u16(o + 2, 4);
    put_f32(o + 4, 900.0f); put_f32(o + 8, 0.0f); put_f32(o + 12, 0.0f);
    o += WP_SIZE;

    put_u16(o + 0, 77); put_u16(o + 2, 3);
    put_f32(o + 4, 150.0f); put_f32(o + 8, 0.0f); put_f32(o + 12, 0.0f);
    o += GD_SIZE;

    put_u16(o + 0, 0); put_u16(o + 2, 1); put_u16(o + 4, 1); put_u16(o + 6, 1);
    put_u32(o + 8, 0);
    put_f32(o + 12, 900.0f); put_f32(o + 16, 0.0f); put_f32(o + 20, 0.0f);
    o += OB_SIZE;

    put_u16(o + 0, 10); put_u16(o + 2, 20);
    put_f32(o + 4, 800.0f); put_f32(o + 8, 90.0f); put_f32(o + 12, 0.0f);
    o += ST_SIZE;

    blen = o;   /* NOTE: the real byte length, regardless of what the counts claim */
}

static void write_gew(const char *dir, const char *level)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.gew", dir, level);
    f = fopen(path, "wb");
    if (f == NULL) { printf("  (cannot write %s)\n", path); return; }
    fwrite(buf, 1, (size_t) blen, f);
    fclose(f);
}

static char envbuf[512];

int main(void)
{
    const char *dir = ".";
    GeWorldWaypoint w;
    GeWorldGuard g[4];
    GeWorldObjective ob;
    GeWorldStep st;

    printf("world knowledge reader\n\n");

    snprintf(envbuf, sizeof envbuf, "GETV_WORLD_DIR=%s", dir);
    putenv(envbuf);

    /* ---------------- a valid file ---------------- */
    build(GE_WORLD_MAGIC, GE_WORLD_VERSION, 0);
    write_gew(dir, "good");
    check("valid file loads",            geWorldLoad("good"), 1);
    check("reports loaded",              geWorldLoaded(), 1);
    check("waypoint count",              geWorldWaypointCount(), 2);
    check("guard count",                 geWorldGuardCount(), 1);
    check("objective count",             geWorldObjectiveCount(), 1);
    check("step count",                  geWorldStepCount(), 1);
    check("level name",                  strcmp(geWorldLevel(), "testlevel") == 0, 1);

    /* By index. */
    check("waypoint(0)",                 geWorldWaypoint(0, &w) && w.id == 10, 1);
    check("  its room",                  w.room, 3);
    checkf("  its x",                    w.x, 100.0f);
    check("waypoint(1) id",              geWorldWaypoint(1, &w) && w.id == 20, 1);

    /* INDEX is not ID, and the file is built so the two differ: index 0 holds id 10. A reader
     * that confused them would pass every test where they happened to match. */
    check("byId(20) is index 1",         geWorldWaypointById(20, &w) && w.id == 20, 1);
    checkf("  and its x",                w.x, 900.0f);
    check("byId(99) absent",             geWorldWaypointById(99, &w), 0);
    check("index out of range",          geWorldWaypoint(9, &w), 0);
    check("negative index",              geWorldWaypoint(-1, &w), 0);

    check("guard(0)",                    geWorldGuard(0, &g[0]) && g[0].chrnum == 77, 1);
    check("objective(0) steps",          geWorldObjective(0, &ob) && ob.steps == 1, 1);
    check("step(0) from->to",            geWorldStep(0, &st) && st.from == 10 && st.to == 20, 1);

    /* Nearest and near-a-point. */
    check("nearest to (110,0,0) is 10",  geWorldNearestWaypoint(110.0f, 0.0f, 0.0f, &w)
                                         && w.id == 10, 1);
    check("nearest to (880,0,0) is 20",  geWorldNearestWaypoint(880.0f, 0.0f, 0.0f, &w)
                                         && w.id == 20, 1);
    check("guards within 100",           geWorldGuardsNear(150.0f, 0.0f, 0.0f, 100.0f, g, 4), 1);
    check("guards within 10",            geWorldGuardsNear(0.0f, 0.0f, 0.0f, 10.0f, g, 4), 0);

    geWorldUnload();
    check("unload clears loaded",        geWorldLoaded(), 0);
    check("unload clears count",         geWorldWaypointCount(), 0);
    check("accessors safe after unload", geWorldWaypoint(0, &w), 0);

    /* ---------------- the rejections ----------------
     *
     * Each of these is a file that would otherwise be read as a world. The layout-mismatch case
     * is the one that matters most: it is the only thing standing between a packer that gained a
     * field and a reader that silently returns a neighbour's data for every record. */
    build(0xDEADBEEFu, GE_WORLD_VERSION, 0);
    write_gew(dir, "badmagic");
    check("bad magic refused",           geWorldLoad("badmagic"), 0);

    build(GE_WORLD_MAGIC, GE_WORLD_VERSION + 7u, 0);
    write_gew(dir, "badver");
    check("wrong version refused",       geWorldLoad("badver"), 0);

    /* Counts claim one waypoint more than the bytes hold. */
    build(GE_WORLD_MAGIC, GE_WORLD_VERSION, 1);
    write_gew(dir, "drift");
    check("LAYOUT MISMATCH refused",     geWorldLoad("drift"), 0);
    check("  and nothing left loaded",   geWorldLoaded(), 0);
    check("  count is zero, not stale",  geWorldWaypointCount(), 0);

    /* Shorter than a header. */
    build(GE_WORLD_MAGIC, GE_WORLD_VERSION, 0);
    blen = 12;
    write_gew(dir, "stub");
    check("truncated header refused",    geWorldLoad("stub"), 0);

    check("missing file refused",        geWorldLoad("nosuchlevel"), 0);

    /* A refused load must not leave the previous world half-attached. Load a good one, then a
     * bad one, and confirm the reader is empty rather than still serving the old level -- which
     * would be the worst outcome: a bot routing confidently around a level it is not in. */
    build(GE_WORLD_MAGIC, GE_WORLD_VERSION, 0);
    write_gew(dir, "good");
    geWorldLoad("good");
    check("good loaded again",           geWorldLoaded(), 1);
    check("then a bad one fails",        geWorldLoad("badmagic"), 0);
    check("  and the old world is gone", geWorldLoaded(), 0);

    printf("\n%s -- %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
