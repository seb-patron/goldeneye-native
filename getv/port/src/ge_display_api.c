#include <stdint.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "ge_display_api.h"
#include "gfx_pc.h"

/* Computed from width/height directly, not from GfxDimensions.aspect_ratio -- that field
 * is only ever assigned on gfx_current_dimensions (gfx_pc.c), never on gfx_output_dimensions,
 * so reading it here would silently return 0.0f forever. width/height are the ones this
 * struct actually keeps current. */
float gePortRealAspect(void)
{
    if (gfx_output_dimensions.height == 0) {
        return 1.3333334f;   /* ASPECT_RATIO_SD (fr.h), the same fallback player.c starts at */
    }
    return (float) gfx_output_dimensions.width / (float) gfx_output_dimensions.height;
}
