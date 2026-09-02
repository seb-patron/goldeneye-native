/* Reload-safe native fixups for statically linked setup sections. */
#include <stddef.h>

#include "ge_setup_fixups.h"
#include "ge_world_levels.h"

/* A stage can select one solo setup and one multiplayer setup, each with at most one pad section
 * and one bound-pad section. Four times the complete loadable-stage catalog therefore covers
 * every section that can require native coordinate scaling without allocating from a game pool. */
#define GE_SETUP_SCALE_SECTION_CAPACITY (GE_WORLD_STAGE_COUNT * 4u)

static const void *ge_scaled_setup_sections[GE_SETUP_SCALE_SECTION_CAPACITY];
static size_t ge_scaled_setup_section_count;

int gePortSetupSectionNeedsScale(const void *section)
{
    size_t i;

    if (section == NULL) { return 0; }
    for (i = 0; i < ge_scaled_setup_section_count; i++) {
        if (ge_scaled_setup_sections[i] == section) { return 0; }
    }

    /* The bound is derived from the full stage catalog above. If that invariant changes, skip
     * an untracked transform instead of risking repeated destructive scaling on a later reload. */
    if (ge_scaled_setup_section_count >= GE_SETUP_SCALE_SECTION_CAPACITY) { return 0; }
    ge_scaled_setup_sections[ge_scaled_setup_section_count++] = section;
    return 1;
}
