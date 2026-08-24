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

/* Deliberately NOT <windows.h>. Everything below needs only stdlib and string, and this
 * header is force-included into the GAME batch as well as the port layer -- where windows.h
 * would be actively dangerous, because it defines `near`, `far` and `BOOL` as macros and the
 * decomp uses all three as ordinary identifiers. Keeping this header free of it is what
 * makes it safe to apply tree-wide. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* errno is undefined here, deliberately and globally.
 *
 * PR/os.h declares struct fields literally named `errno` -- OSContStatus and OSContPad both
 * carry one, and joy.c reads them. That is legal C until a libc defines errno as a macro, at
 * which point the field expands and the struct fails to parse. MinGW's headers do define it;
 * glibc and Darwin only bite if <errno.h> happens to have been included first, which in the
 * port layer it is not.
 *
 * This was first scoped to port_os.c on the reasoning that it was the only port translation
 * unit including PR/os.h. That was simply wrong -- port_vi.c includes it too, and any future
 * file might -- so the undef belongs here, where it cannot be missed.
 *
 * The cost is that the four places wanting the real errno (port_save.c, ge_launcher.cpp)
 * must say so explicitly; they use ge_errno, defined in those files for every platform. */
#undef errno

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

/* usleep is deliberately NOT shimmed here.
 *
 * It was, as `#define usleep(u) ...`, and that broke the build: MinGW's <unistd.h> declares
 * usleep itself, the macro expanded inside that declaration, and gcc reported the error
 * against this header's line rather than the real one -- which is a genuinely confusing
 * place to be sent. A function-like macro for a name the platform may also declare is the
 * wrong tool.
 *
 * There is exactly one call site in the whole tree (ge_tvos_main.c), so it branches there
 * on Sleep() instead. One ifdef at one call site beats a macro that can collide with any
 * header that happens to declare the same name.
 */

/* bcopy and bzero are NOT shimmed here, and the reason is the same one that removed the
 * usleep macro: the decomp declares both itself -- include/bstring.h:28 and PR/os.h:1003,
 * as `void bcopy(const void *, void *, int)` -- so a function-like macro of that name
 * expands inside those declarations and the file fails to parse.
 *
 * They are implemented as real functions in getv/port/src/ge_link_stubs.c instead, matching
 * that prototype exactly, so the decomp's own declaration serves as the prototype and
 * nothing has to be redeclared. That is twice now that a macro was the wrong tool for a
 * name the rest of the tree already knows about.
 */

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
