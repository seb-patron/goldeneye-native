/* Metal's depth-state policy can be checked without a GPU, window, ROM, or extracted assets.
 * Equal-depth fragments are significant for Fast3D effects such as depth-tested,
 * non-depth-writing geometry, so both ordinary and decal state variants stay inclusive. */
#include <stdbool.h>
#include <stdio.h>

#include "gfx_metal.h"

static int fails;

static void check_int(int actual, int expected, const char *what)
{
    if (actual != expected) {
        printf("  FAIL: %s: got %d, want %d\n", what, actual, expected);
        fails++;
    } else {
        printf("  ok  : %s\n", what);
    }
}

int main(void)
{
    printf("Metal depth policy\n");

    check_int(gfx_metal_depth_compare(false, false), GFX_METAL_DEPTH_COMPARE_ALWAYS,
              "disabled ordinary depth testing always passes");
    check_int(gfx_metal_depth_compare(false, true), GFX_METAL_DEPTH_COMPARE_ALWAYS,
              "disabled decal depth testing always passes");
    check_int(gfx_metal_depth_compare(true, false), GFX_METAL_DEPTH_COMPARE_LESS_EQUAL,
              "ordinary depth testing accepts equal-depth fragments");
    check_int(gfx_metal_depth_compare(true, true), GFX_METAL_DEPTH_COMPARE_LESS_EQUAL,
              "decal depth testing accepts equal-depth fragments");

    check_int((int)gfx_metal_depth_slope_bias(false, false, false, true), 0,
              "depth-disabled geometry has no slope bias");
    check_int((int)gfx_metal_depth_slope_bias(true, true, false, true), 0,
              "opaque depth-writing geometry has no slope bias");
    check_int((int)gfx_metal_depth_slope_bias(true, false, false, false), 0,
              "ordinary translucent geometry has no slope bias");
    check_int((int)gfx_metal_depth_slope_bias(true, false, false, true), -8,
              "cloud-mode translucent geometry gets the Metal parity bias");
    check_int((int)gfx_metal_depth_slope_bias(true, false, true, false), -2,
              "decal geometry retains its existing slope bias");

    printf("\n%s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
