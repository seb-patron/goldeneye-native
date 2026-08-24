/* Windows/MinGW compatibility shims.
 *
 * Force-included by getv/build_windows.sh (-include), never by a source file, so that no
 * call site anywhere in the tree grows a #ifdef for a function that exists everywhere else.
 * On any other host this header is empty.
 *
 * What this is and is not. It is a small set of POSIX names that MinGW does not provide but
 * has exact Win32 or MSVCRT equivalents for -- a spelling difference, nothing more. It is
 * deliberately NOT the place for anything that needs real behaviour: the crash handler's
 * backtrace has no equivalent spelling and is branched properly in ge_tvos_main.c, and the
 * launcher's process replacement is branched in ge_launcher.cpp, because on Windows it is a
 * genuinely different operation rather than a renamed one.
 *
 * The measured motivation: setenv has 15 call sites in the port layer, unsetenv 5. The whole
 * configuration system is expressed as setenv() on GETV_ gates, so without this the Windows
 * port would either not build or would need those 15 sites edited, and every one of them
 * would then read worse on the two platforms that were already fine.
 */
#ifndef GE_WIN_COMPAT_H
#define GE_WIN_COMPAT_H

#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* MSVCRT has _putenv_s, which differs from setenv in two ways that matter: it has no
 * "overwrite" argument, and passing an empty value deletes the variable rather than setting
 * it to "". Both are handled here so callers see POSIX semantics.
 *
 * The overwrite=0 case is not decoration. ge_config.c's entire precedence model is
 * setenv(name, value, 0) for values from the config file and 1 for the command line, which
 * is what makes the environment beat the file. Ignoring the flag would silently invert that
 * ordering on Windows only, and the symptom would be a config file overriding an explicit
 * environment variable -- subtle, and very annoying to track down. */
static inline int ge_win_setenv(const char *name, const char *value, int overwrite)
{
    if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) return -1;
    if (!overwrite && getenv(name) != NULL) return 0;
    if (value == NULL) value = "";
    /* An empty value would delete the variable under _putenv_s, so a single space is not a
     * safe substitute either. Callers in this tree never set "", and if one starts, this
     * returns the deletion behaviour rather than pretending. */
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

static inline int ge_win_unsetenv(const char *name)
{
    if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) return -1;
    return _putenv_s(name, "") == 0 ? 0 : -1;   /* empty value deletes, which is what we want */
}

#define setenv(n, v, o) ge_win_setenv((n), (v), (o))
#define unsetenv(n)     ge_win_unsetenv((n))

/* usleep is POSIX and MinGW has no equivalent spelling. Sleep() takes milliseconds, so a
 * sub-millisecond request would round to zero and busy-spin; it is clamped to 1ms instead,
 * which matches what every other Sleep-based usleep shim does and is honest about the
 * platform's timer granularity. */
static inline int ge_win_usleep(unsigned long usec)
{
    DWORD ms = (DWORD) (usec / 1000UL);
    Sleep(ms == 0 && usec != 0 ? 1 : ms);
    return 0;
}
#define usleep(u) ge_win_usleep((unsigned long)(u))

/* realpath -> _fullpath. _fullpath allocates when given a NULL buffer, matching the POSIX
 * form this tree uses (realpath(path, NULL)). */
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
static inline char *ge_win_realpath(const char *path, char *resolved)
{
    return _fullpath(resolved, path, resolved ? PATH_MAX : 0);
}
#define realpath(p, r) ge_win_realpath((p), (r))

#endif /* _WIN32 */

#endif /* GE_WIN_COMPAT_H */
