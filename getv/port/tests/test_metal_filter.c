/* Metal receives both the process-wide filtering mode and a per-tile decision from Fast3D.
 * This checks the backend's small state bridge without a GPU, window, ROM, or extracted assets.
 * The native Dam capture remains the integration check for the generated MSL itself. */
#include <stddef.h>
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
    printf("Metal three-point filtering state\n");

    check_int(gfx_metal_three_point_active(2, 1), 1,
              "three-point mode enables the shader filter for a filtered tile");
    check_int(gfx_metal_three_point_active(2, 0), 0,
              "an RDP point-sampled tile bypasses the shader filter");
    check_int(gfx_metal_three_point_active(1, 1), 0,
              "bilinear mode keeps direct sampler filtering");
    check_int(gfx_metal_three_point_active(0, 1), 0,
              "point mode keeps direct nearest sampling");
    check_int(gfx_metal_three_point_active(3, 1), 0,
              "an invalid mode cannot accidentally enable three-point filtering");

    check_int((int)sizeof(struct GfxMetalDrawUniforms), 32,
              "C draw uniforms include explicit Metal alignment padding");
    check_int((int)offsetof(struct GfxMetalDrawUniforms, tex0_size), 0,
              "TEXEL0 size starts at byte 0");
    check_int((int)offsetof(struct GfxMetalDrawUniforms, tex1_size), 8,
              "TEXEL1 size starts at byte 8");
    check_int((int)offsetof(struct GfxMetalDrawUniforms, has_height), 16,
              "height flag starts at byte 16");
    check_int((int)offsetof(struct GfxMetalDrawUniforms, tex0_filter), 20,
              "TEXEL0 filter flag starts at byte 20");
    check_int((int)offsetof(struct GfxMetalDrawUniforms, tex1_filter), 24,
              "TEXEL1 filter flag starts at byte 24");

    printf("\n%s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
