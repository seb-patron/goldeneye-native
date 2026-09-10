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

/* Enforce the launcher's Base Game choice after config/CLI parsing and after the
 * launcher stores its model (including platforms that continue without exec). */
void geConfigApplyLauncherProfile(void);

/* Non-static on purpose: `nm <binary> | grep ge_config_loaded` is the build-integrity
 * proof that a port-layer rebuild actually landed, the same way gfx_sdl2.c's
 * ge_pace_framerate is used. 0 = no file found, 1 = a file was read. */
extern int ge_config_loaded;

/* Resolved control style, 0..7 in CONTROLLER_CONFIG_* order, or -1 if the user did not
 * ask for one. Nothing consumes this yet; see the "controls" key in ge_config.c. */
extern int ge_config_controls;

/* ---- saving --------------------------------------------------------------
 *
 * Where settings are read from and, now, written back to. Never NULL; an empty string
 * means no config file could be located or created, and saving is unavailable -- the
 * caller must say so rather than silently writing into the working directory. */
const char *geConfigPath(void);

/* Merge `count` key/value pairs into that file and return 0 on success.
 *
 * A rewrite in place: comments, ordering and every key this port does not recognise
 * survive, and a key already present is updated where it sits -- including one that is
 * commented out, which is uncommented rather than duplicated at the end. A NULL or
 * empty value comments the key out, so the documentation around it stays meaningful.
 *
 * Written to a temporary file and renamed, so an interrupted save leaves the previous
 * config intact rather than a truncated one.
 *
 * This is what makes the launcher's control remapping persist. Before it, settings
 * existed only as environment variables handed to the re-exec'd game and were gone at
 * the end of the session. */
int geConfigSave(const char *const *keys, const char *const *values, int count);

#ifdef __cplusplus
}
#endif
#endif /* GE_CONFIG_H */
