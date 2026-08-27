/* One navigation decision per tick. See ge_bot_nav.c for the derivation -- ported from Perfect
 * Dark's chrNavTickMain, the state machine guards actually run, replacing five independently
 * triggered heuristics that could steer against each other in the same frame. */
#ifndef GE_BOT_NAV_H
#define GE_BOT_NAV_H

typedef struct GeNavState {
    int   mode;
    int   iter;
    float target_x, target_z;
    float aim_x, aim_z;
    int   have_target;
} GeNavState;

/* Call once per tick per navigating body. out_aim_x/z is where to steer THIS tick -- feed it
 * straight into whatever turns a bearing into a heading. Resets its own state when the target
 * moves more than a body-width from where it was, so a caller does not manage that itself. */
void gePortNavTick(GeNavState *nav, float px, float pz, float target_x, float target_z,
                   float *out_aim_x, float *out_aim_z);

#endif
