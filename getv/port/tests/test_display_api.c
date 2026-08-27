/* Does gePortRealAspect report the window's real shape, not the N64's fixed 4:3?
 *
 * The bug this guards against: GfxDimensions.aspect_ratio is only ever assigned on
 * gfx_current_dimensions (gfx_pc.c), never on gfx_output_dimensions -- the struct this
 * accessor reads. A version that trusted that field would silently return 0.0f forever.
 * So gePortRealAspect computes from width/height directly, and the test that matters most
 * here is the one that sets aspect_ratio to a wrong, stale value while width/height are
 * correct, and checks the wrong field gets ignored.
 */
#include <stdint.h>
#include <stdio.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_pc.h"

/* The global the unit under test reads. Defined here, not in gfx_pc.c -- pulling that file
 * in would drag OpenGL and SDL into a test that only needs one struct. */
struct GfxDimensions gfx_output_dimensions;
struct GfxDimensions gfx_current_dimensions;
unsigned int gfx_supersample;

#include "../src/ge_display_api.c"

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
    else       { printf("  ok  : %s\n", what); }
}

static int close_enough(float a, float b) { float d = a - b; return d > -0.001f && d < 0.001f; }

int main(void)
{
    printf("real window shapes\n");

    gfx_output_dimensions.width = 1706; gfx_output_dimensions.height = 960;
    check(close_enough(gePortRealAspect(), 1706.0f / 960.0f), "16:9-ish window reports its own ratio");

    gfx_output_dimensions.width = 1280; gfx_output_dimensions.height = 960;
    check(close_enough(gePortRealAspect(), 4.0f / 3.0f), "4:3 window reports 4:3");

    gfx_output_dimensions.width = 1000; gfx_output_dimensions.height = 1000;
    check(close_enough(gePortRealAspect(), 1.0f), "square window reports 1:1");

    printf("\nthe field this is deliberately NOT reading\n");

    gfx_output_dimensions.width = 1706; gfx_output_dimensions.height = 960;
    gfx_output_dimensions.aspect_ratio = 0.0f;    /* what a naive read would return */
    check(close_enough(gePortRealAspect(), 1706.0f / 960.0f),
          "a zeroed aspect_ratio field does not leak into the answer");

    gfx_output_dimensions.aspect_ratio = 99.0f;   /* an implausible stale value */
    check(close_enough(gePortRealAspect(), 1706.0f / 960.0f),
          "a stale, wrong aspect_ratio field does not leak into the answer either");

    printf("\nno drawable yet\n");

    gfx_output_dimensions.width = 800; gfx_output_dimensions.height = 0;
    check(close_enough(gePortRealAspect(), 1.3333334f),
          "zero height falls back to 4:3 (ASPECT_RATIO_SD) instead of dividing by zero");

    printf("\n%s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
