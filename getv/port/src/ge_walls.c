/* WALLS AS DATA.
 *
 * Everything the bot knew about walls it learned by ray, one frame at a time, and a ray answers
 * the wrong question: "is something 79 units ahead of me right now". By the time that comes back
 * true the body is already walking at it, and the recovery has to undo a commitment instead of
 * never making it. Ten runs of Train end the same way -- around one crate, into the next wall.
 *
 * A wall is not a discovery. It is a fixed fact about the level, and it should be in hand before
 * the first frame like the floor is. gen_level_walls.py derives the set from the floor mesh: a
 * span of a tile's edge that no neighbour covers is a span no body can cross. Where a neighbour
 * does cover it, that is a doorway or an open join and it is free. Inside the mesh is where the
 * bot can go; a boundary span is where it has no chance of going.
 *
 * Loaded flat and tested by segment intersection. 1,405 segments on Train, 7,519 on Surface --
 * small enough that the obvious loop is the right one, and no acceleration structure earns its
 * complexity until something proves it does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ge_walls.h"

typedef struct { float x0, z0, x1, z1, y; } GeWallSeg;

static GeWallSeg *ge_wl = NULL;
static int        ge_wl_n = 0;
static int        ge_wl_tried = 0;

void geWallsUnload(void)
{
    free(ge_wl);
    ge_wl = NULL;
    ge_wl_n = 0;
    ge_wl_tried = 0;
}

int geWallsCount(void) { return ge_wl_n; }

/* The file is a flat array of five-number arrays. Scanning for numbers is enough to read it and
 * costs nothing; a JSON parser here would be more code than the feature it serves. */
int geWallsLoad(const char *level)
{
    char path[512];
    const char *dir;
    FILE *f;
    long size;
    char *buf, *p;
    float scale = 1.0f;
    int cap = 0;

    geWallsUnload();
    ge_wl_tried = 1;
    if (level == NULL || *level == '\0') { return 0; }

    dir = getenv("GETV_WALLS_DIR");
    if (dir == NULL || *dir == '\0') { dir = "build/levels"; }
    snprintf(path, sizeof path, "%s/%s.walls.json", dir, level);

    f = fopen(path, "r");
    if (f == NULL) { return 0; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return 0; }
    buf = (char *) malloc((size_t) size + 1);
    if (buf == NULL) { fclose(f); return 0; }
    size = (long) fread(buf, 1, (size_t) size, f);
    fclose(f);
    buf[size] = '\0';

    /* 🔑 asset = runtime * levelscale. The segments are asset space like every other JSON here,
     * so they are divided ONCE, at this boundary. A consumer that has to remember to convert is
     * a consumer that will eventually forget. */
    p = strstr(buf, "\"levelscale\":");
    if (p != NULL) {
        float s = (float) atof(p + 13);
        if (s > 0.0f) { scale = s; }
    }

    p = strstr(buf, "\"segments\":");
    if (p == NULL) { free(buf); return 0; }
    p = strchr(p, '[');
    if (p == NULL) { free(buf); return 0; }
    p++;

    while (*p) {
        float v[5];
        int got = 0;

        while (*p && *p != '[' && *p != ']') { p++; }
        if (*p != '[') { break; }
        p++;
        while (got < 5 && *p) {
            char *end;
            float d = (float) strtod(p, &end);
            if (end == p) { p++; continue; }
            v[got++] = d;
            p = end;
            while (*p == ',' || *p == ' ') { p++; }
            if (*p == ']') { break; }
        }
        if (*p == ']') { p++; }
        if (got < 5) { continue; }

        if (ge_wl_n == cap) {
            int ncap = cap ? cap * 2 : 256;
            GeWallSeg *nw = (GeWallSeg *) realloc(ge_wl, (size_t) ncap * sizeof *nw);
            if (nw == NULL) { break; }
            ge_wl = nw;
            cap = ncap;
        }
        ge_wl[ge_wl_n].x0 = v[0] / scale;
        ge_wl[ge_wl_n].z0 = v[1] / scale;
        ge_wl[ge_wl_n].x1 = v[2] / scale;
        ge_wl[ge_wl_n].z1 = v[3] / scale;
        ge_wl[ge_wl_n].y  = v[4] / scale;
        ge_wl_n++;
    }
    free(buf);

    printf("[getv][walls] %s: %d wall segment(s) loaded (levelscale %.6f)\n",
           level, ge_wl_n, (double) scale);
    fflush(stdout);
    return ge_wl_n > 0;
}

static int seg_cross(float ax, float az, float bx, float bz,
                     float cx, float cz, float dx, float dz)
{
    float r_x = bx - ax, r_z = bz - az;
    float s_x = dx - cx, s_z = dz - cz;
    float den = r_x * s_z - r_z * s_x;
    float t, u;

    if (fabsf(den) < 1e-9f) { return 0; }   /* parallel: a wall run alongside is not a crossing */
    t = ((cx - ax) * s_z - (cz - az) * s_x) / den;
    u = ((cx - ax) * r_z - (cz - az) * r_x) / den;
    return (t > 0.0f && t < 1.0f && u > 0.0f && u < 1.0f);
}

int geWallsBlocked(float x0, float z0, float x1, float z1)
{
    int i;
    for (i = 0; i < ge_wl_n; i++) {
        if (seg_cross(x0, z0, x1, z1,
                      ge_wl[i].x0, ge_wl[i].z0, ge_wl[i].x1, ge_wl[i].z1)) {
            return 1;
        }
    }
    return 0;
}

/* A body is not a line. Testing the centre alone walks the bot's shoulder into the corner it just
 * proved its middle could clear -- the same mistake the line-only sense probe made, which is why
 * geSenseClearestHeadingForBody exists at all. */
int geWallsBlockedForBody(float x0, float z0, float x1, float z1, float halfwidth)
{
    float dx = x1 - x0, dz = z1 - z0;
    float len = sqrtf(dx * dx + dz * dz);
    float px, pz;

    if (geWallsBlocked(x0, z0, x1, z1)) { return 1; }
    if (len < 1e-4f || halfwidth <= 0.0f) { return 0; }

    px = -(dz / len) * halfwidth;
    pz =  (dx / len) * halfwidth;
    if (geWallsBlocked(x0 + px, z0 + pz, x1 + px, z1 + pz)) { return 1; }
    if (geWallsBlocked(x0 - px, z0 - pz, x1 - px, z1 - pz)) { return 1; }
    return 0;
}
