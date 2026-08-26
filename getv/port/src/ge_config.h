/* ge_config.h - the user configuration layer.
 *
 * This file and ge_config.c are purely additive: nothing else in port/src/** is
 * modified by them. The single call site is getv/port/mac/ge_mac_main.c's main().
 *
 * See ge_config.c's header comment for the design and the precedence rules.
 */
#ifndef GE_CONFIG_H
#define GE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call ONCE, as the very first statement of main(), before SDL_main().
 *
 * Everything this does is expressed as setenv() on the existing GETV_* gates, so
 * every consumer in the port keeps reading getenv() exactly as it does today and
 * no other file in the port has to change. Returns 0 normally; returns a non-zero exit
 * code if the process should stop (`--help`, `--write-config`, or a fatal config
 * error). */
int geConfigInit(int argc, char **argv);

/* Non-static on purpose: `nm <binary> | grep ge_config_loaded` is the build-integrity
 * proof that a port-layer rebuild actually landed, the same way gfx_sdl2.c's
 * ge_pace_framerate is used. 0 = no file found, 1 = a file was read. */
extern int ge_config_loaded;

/* Resolved control style, 0..7 in CONTROLLER_CONFIG_* order, or -1 if the user did not
 * ask for one. Nothing consumes this yet; see the "controls" key in ge_config.c. */
extern int ge_config_controls;

#ifdef __cplusplus
}
#endif
#endif /* GE_CONFIG_H */
