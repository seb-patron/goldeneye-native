/* Metal uses an upper-left viewport/scissor origin, while GfxRenderingAPI supplies the
 * lower-left coordinates consumed directly by OpenGL. This checks the backend conversion
 * without needing a GPU, window, ROM, or extracted assets.
 *
 *   clang -I getv/port/fast3d -o /tmp/t getv/port/tests/test_metal_rect.c && /tmp/t
 */
#include <stdio.h>

#include "gfx_metal.h"

static int fails;

static void check_y(int lower_left_y, int height, int target_height,
                    int expected, const char *what)
{
    int actual = gfx_metal_upper_left_y(lower_left_y, height, target_height);
    if (actual != expected) {
        printf("  FAIL: %s: got %d, want %d\n", what, actual, expected);
        fails++;
    } else {
        printf("  ok  : %s\n", what);
    }
}

int main(void)
{
    printf("Metal lower-left to upper-left rectangle conversion\n");

    check_y(0,   960, 960, 0,   "a full-frame rectangle stays full-frame");
    check_y(0,   480, 960, 480, "the OpenGL bottom half becomes Metal's lower half");
    check_y(480, 480, 960, 0,   "the OpenGL top half becomes Metal's upper half");

    /* Complex's two-player panes at 1280x960, measured from the deterministic render
     * harness. These values exposed the bug: passing 40 and 484 straight to Metal made
     * both players draw into the wrong half, leaving the lower pane at the clear colour. */
    check_y(40,  436, 960, 484, "Complex lower pane maps below the divider");
    check_y(484, 436, 960, 40,  "Complex upper pane maps above the divider");

    /* The post-processing path renders the same layout into an inflated target. The
     * conversion must use that target's height, not the drawable's native height. */
    check_y(80,  872, 1920, 968, "Complex lower pane maps in a 2x render target");
    check_y(968, 872, 1920, 80,  "Complex upper pane maps in a 2x render target");

    check_y(100, 200, 960, 660, "an arbitrary inset preserves its extent");

    printf("\n%s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
