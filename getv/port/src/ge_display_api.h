#ifndef GE_DISPLAY_API_H
#define GE_DISPLAY_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* The real, current output aspect ratio (width / height) of the actual window or
 * drawable, as fast3d already computes it for its own use -- not the N64's fixed 4:3.
 * player.c's DEFAULT_ASPECT is a retail constant, set once at player init and never
 * revisited, which is why widescreen windows pillarbox instead of widening: nothing
 * downstream of that init ever asks what shape the window actually is.
 *
 * Feed this to set_cur_player_aspect() (player.c:578) once a frame, or whenever a
 * resize could have happened, to make the game's own aspect match the window's. */
float gePortRealAspect(void);

#ifdef __cplusplus
}
#endif

#endif
