/* Where you are, in the level's own terms rather than in coordinates.
 *
 * Every spatial answer this port gives is a number: node 3818, room 2, x -283. None of that tells
 * a reader whether the bot is making progress, because a node id means nothing beside the graph
 * that indexes it and a coordinate means nothing without the level's shape.
 *
 * The walkthrough material describes the same levels the way a player sees them, and
 * tools/gen_level_places.py turns that description into coordinates by measuring the floor mesh
 * against it. Train comes out as six carriages of 5,964 units along x, which is about 206 game
 * units per metre -- corroborated independently by the width, 739 units against a stated four
 * metres.
 *
 * So a position can be reported as "car 2 of 6" instead of "x -6000", and a route step that goes
 * backwards a carriage becomes visible as a mistake rather than a different large number.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GE_PLACES_MAX 16

typedef struct { int index; float from, to, entry; } GePlaceBound;

static GePlaceBound ge_pl[GE_PLACES_MAX];
static int   ge_pl_n = -1;
static char  ge_pl_kind[16] = "section";
static char  ge_pl_axis = 'x';
static float ge_pl_upm = 0.0f;

/* Scanned rather than parsed. One file, four fields, and a JSON parser here would be more code
 * than the feature it serves. */
static void ge_pl_load(const char *level)
{
    char path[512];
    const char *dir;
    FILE *f;
    long size;
    char *buf, *p;

    ge_pl_n = 0;
    if (level == NULL || *level == '\0') { return; }
    dir = getenv("GETV_PLACES_DIR");
    if (dir == NULL || *dir == '\0') { dir = "build/levels"; }
    snprintf(path, sizeof path, "%s/%s.places.json", dir, level);

    f = fopen(path, "r");
    if (f == NULL) { return; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (1 << 20)) { fclose(f); return; }
    buf = (char *) malloc((size_t) size + 1);
    if (buf == NULL) { fclose(f); return; }
    size = (long) fread(buf, 1, (size_t) size, f);
    fclose(f);
    buf[size] = '\0';

    p = strstr(buf, "\"division_name\":");
    if (p != NULL) {
        char *q = strchr(p + 16, '"');
        if (q != NULL) {
            char *e = strchr(q + 1, '"');
            if (e != NULL && (size_t) (e - q - 1) < sizeof ge_pl_kind) {
                memcpy(ge_pl_kind, q + 1, (size_t) (e - q - 1));
                ge_pl_kind[e - q - 1] = '\0';
            }
        }
    }
    p = strstr(buf, "\"axis\":");
    if (p != NULL) {
        char *q = strchr(p + 7, '"');
        if (q != NULL) { ge_pl_axis = q[1]; }
    }
    p = strstr(buf, "\"units_per_metre\":");
    if (p != NULL) { ge_pl_upm = (float) atof(p + 18); }

    p = strstr(buf, "\"bounds\"");
    while (p != NULL && ge_pl_n < GE_PLACES_MAX) {
        char *ix = strstr(p, "\"index\":");
        char *fr, *to, *en;
        if (ix == NULL) { break; }
        fr = strstr(ix, "\"from\":");
        to = strstr(ix, "\"to\":");
        if (fr == NULL || to == NULL) { break; }
        ge_pl[ge_pl_n].index = atoi(ix + 8);
        ge_pl[ge_pl_n].from  = (float) atof(fr + 7);
        ge_pl[ge_pl_n].to    = (float) atof(to + 5);
        en = strstr(to, "\"entry\":");
        ge_pl[ge_pl_n].entry = (en != NULL) ? (float) atof(en + 8) : ge_pl[ge_pl_n].from;
        ge_pl_n++;
        p = to;
    }
    free(buf);
}

/* Writes something like "car 2 of 6, 41m in" and returns 1, or returns 0 when the level has no
 * description to report against. */
int gePortPlaceName(const char *level, float x, float z, char *out, int n)
{
    float along;
    int i;

    if (out == NULL || n <= 0) { return 0; }
    if (ge_pl_n < 0) { ge_pl_load(level); }
    if (ge_pl_n <= 0) { return 0; }

    along = (ge_pl_axis == 'z') ? z : x;
    for (i = 0; i < ge_pl_n; i++) {
        if (along >= ge_pl[i].from && along <= ge_pl[i].to) {
            if (ge_pl_upm > 1.0f) {
                float into = (along - ge_pl[i].entry) / ge_pl_upm;
                if (into < 0.0f) { into = -into; }
                snprintf(out, (size_t) n, "%s %d of %d, %.0fm in",
                         ge_pl_kind, ge_pl[i].index, ge_pl_n, (double) into);
            } else {
                snprintf(out, (size_t) n, "%s %d of %d", ge_pl_kind, ge_pl[i].index, ge_pl_n);
            }
            return 1;
        }
    }
    snprintf(out, (size_t) n, "outside the mapped %ss", ge_pl_kind);
    return 1;
}
