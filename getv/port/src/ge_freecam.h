/* Free camera: state and input, with no game types in it.
 *
 * Deliberately free of <ultra64.h> and the PR headers. ge_config.c already carries the note
 * about why port-layer code should not pull a game header in, and this file would be the worst
 * place to ignore it: it is compiled into the port batch beside the fast3d sources, where
 * PR/os.h's bcopy/bcmp/bzero redeclarations and the errno undef have both bitten already. So
 * positions are bare floats here and the coord3d/Mtxf conversion happens on the decompilation
 * side, where those types are native.
 */
#ifndef GE_FREECAM_H
#define GE_FREECAM_H

/* Resolved once from GETV_FREECAM. Off means every hook below compiles to a branch that is
 * never taken, so a normal run is unaffected. */
int  gePortFreecamEnabled(void);

/* Whether the camera is flying RIGHT NOW. The render hooks ask this every frame. */
int  gePortFreecamActive(void);

/* Seeded from the player the moment it is switched ON -- not the moment it is enabled.
 *
 * The distinction is the whole thing. Enabling happens at frame 1; switching on happens
 * whenever F8 is pressed or GETV_FREECAM's start frame arrives. Seeding at enable time parks
 * the camera wherever Bond stood during the boot and intro, he then walks the length of the
 * level, and the camera is left over a thousand units away in another room -- the view moves,
 * every room culls, and it reads as broken visibility rather than a stale position. */
void gePortFreecamSeed(float x, float y, float z, float yaw, float pitch);
int  gePortFreecamSeeded(void);

/* On, but with nowhere to be yet. The caller seeds when this is true, because only it can read
 * the player's position. */
int  gePortFreecamNeedsSeed(void);

/* Reads the keyboard and integrates one frame of movement. */
void gePortFreecamTick(void);

void gePortFreecamGetPos(float *out_xyz);
void gePortFreecamGetAngles(float *out_yaw, float *out_pitch);

#endif /* GE_FREECAM_H */
