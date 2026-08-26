/* Post-process parameters. See ge_postfx.h for why they live here and not in gfx_opengl.c. */
#include <stdlib.h>
#include <stdio.h>

#include "ge_postfx.h"

/* The CRT preset. These are the numbers mods/crt_screen/mod.lua ships with, repeated here so
 * that GETV_CRT=1 alone -- with no mod loaded at all -- still gives the same picture. Two
 * copies of five constants is worth it: the environment gate has to keep working for anyone
 * driving the game from a shell or a config file, and a gate whose defaults differ from the
 * mod's would produce two different "CRT modes". */
#define GE_CRT_SCANLINE 0.28f
#define GE_CRT_MASK 0.18f
#define GE_CRT_CURVE 0.025f
#define GE_CRT_VIGNETTE 0.22f
#define GE_CRT_LINES 240.0f

static GePostfx ge_fx;
static int      ge_fx_ready;

static float ge_envf(const char *k, float dflt)
{
    const char *e = getenv(k);
    return (e != NULL && *e != '\0') ? (float) atof(e) : dflt;
}

static float ge_clampf(float v, float lo, float hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

void gePostfxInit(void)
{
    int crt;

    if (ge_fx_ready) { return; }
    ge_fx_ready = 1;

    crt = (int) ge_envf("GETV_CRT", 0.0f);

    /* GETV_CRT=1 is a preset, not a switch with hidden constants: each term stays
     * individually overridable, because tuning a look means moving one number at a time and
     * a preset that cannot be taken apart cannot be tuned. */
    ge_fx.crt      = crt;
    ge_fx.scanline = ge_envf("GETV_CRT_SCANLINE", crt ? GE_CRT_SCANLINE : 0.0f);
    ge_fx.mask     = ge_envf("GETV_CRT_MASK",     crt ? GE_CRT_MASK     : 0.0f);
    ge_fx.curve    = ge_envf("GETV_CRT_CURVE",    crt ? GE_CRT_CURVE    : 0.0f);
    ge_fx.vignette = ge_envf("GETV_CRT_VIGNETTE", crt ? GE_CRT_VIGNETTE : 0.0f);
    ge_fx.lines    = ge_envf("GETV_CRT_LINES",    GE_CRT_LINES);
    ge_fx.fxaa     = (ge_envf("GETV_FXAA", 0.0f) > 0.0f) ? 1 : 0;
}

const GePostfx *gePostfxGet(void)
{
    if (!ge_fx_ready) { gePostfxInit(); }
    return &ge_fx;
}

void gePostfxSet(const GePostfx *p)
{
    if (p == NULL) { return; }
    if (!ge_fx_ready) { gePostfxInit(); }

    /* Clamped rather than validated-and-rejected. This is reached from Lua, where a typo is a
     * number rather than an error, and the failure mode of an unclamped curve or scanline is
     * a black screen that looks like a crash in the renderer. */
    ge_fx.crt      = p->crt ? 1 : 0;
    ge_fx.scanline = ge_clampf(p->scanline, 0.0f, 1.0f);
    ge_fx.mask     = ge_clampf(p->mask,     0.0f, 1.0f);
    ge_fx.curve    = ge_clampf(p->curve,    0.0f, 0.10f);
    ge_fx.vignette = ge_clampf(p->vignette, 0.0f, 1.0f);
    ge_fx.lines    = ge_clampf(p->lines,    1.0f, 2160.0f);
    ge_fx.fxaa     = p->fxaa ? 1 : 0;
}

int gePostfxWanted(void)
{
    const GePostfx *p = gePostfxGet();

    /* crt is the master switch, but the four terms are also independently meaningful: a mod
     * that only wants a vignette should not have to claim to be a CRT. */
    if (p->fxaa) { return 1; }
    if (p->crt && (p->scanline > 0.0f || p->mask > 0.0f ||
                   p->curve > 0.0f || p->vignette > 0.0f)) {
        return 1;
    }
    return 0;
}
