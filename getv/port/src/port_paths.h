/* GoldenEye port — the one place that knows where the host keeps user files.
 *
 * Before this file existed, three call sites each open-coded
 * `getenv("HOME") + "/Library/Application Support/..."` and one of them shelled out to
 * `/bin/mkdir -p`. That is four independent Apple assumptions in code that is
 * otherwise plain C and SDL2. They now go through the two functions below.
 *
 * See getv/port/src/port_paths.c for the platform matrix and docs/PORTING.md for
 * what is still missing on Windows and Linux.
 */
#ifndef GE_PORT_PATHS_H
#define GE_PORT_PATHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write the per-user data directory for (org, app) into `out`.
 *
 * Returns 0 on success and -1 if the host cannot name one, in which case `out` is
 * left untouched. Callers print their own diagnostic: the two existing ones have
 * different prefixes and different fallback behaviour, and this function has no
 * business choosing between them.
 *
 * `org` is used only where the host convention has a vendor level (SDL_GetPrefPath).
 * On macOS the result is `$HOME/Library/Application Support/<app>` with NO trailing
 * separator; everywhere else it is SDL_GetPrefPath's answer, which DOES end in one.
 * That asymmetry predates this file and is preserved deliberately -- both existing
 * callers append "/<name>" unconditionally and tolerate the resulting doubled slash.
 */
int gePortUserDataDir(const char *org, const char *app, char *out, size_t outsz);

/* Create one directory level. Returns 0 if the directory was created OR already
 * existed, -1 otherwise with errno set for strerror(). `mode` is the POSIX
 * permission bits and is ignored on hosts that have no such concept. */
int gePortMakeDir(const char *path, unsigned mode);

/* Create a directory and every missing parent, i.e. `mkdir -p`. Returns 0 if the
 * full path exists afterwards, -1 otherwise. Accepts '/' as the separator on every
 * host, and also '\\' where the host uses it. */
int gePortMakeDirTree(const char *path, unsigned mode);

#ifdef __cplusplus
}
#endif

#endif /* GE_PORT_PATHS_H */
