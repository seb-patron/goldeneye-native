/* GoldenEye port — where the host keeps user files, and how to create a directory.
 *
 * Why this file exists.
 *
 * The port layer is otherwise plain C99 plus SDL2, and SDL2 is the portable half of the
 * problem by construction. The non-portable half was four hand-rolled Apple assumptions
 * scattered across three files:
 *
 *   port_save.c:101-109   getenv("HOME") + "/Library/Application Support/Goldeneye-Native"
 *   ge_config.c:893-897   getenv("HOME") + "/Library/Application Support/GoldenEye"
 *   ge_config.c:1131-1139 the same again, plus system("/bin/mkdir -p '...'")
 *   port_save.c:118       mkdir(base, 0755), which is <sys/stat.h> and POSIX-only
 *
 * None of them is hard to port; the problem was that there were four of them and no
 * single place to change. There is now one.
 *
 * The platform matrix:
 *
 *   macOS       $HOME/Library/Application Support/<app>
 *               Hand-built rather than delegated to SDL_GetPrefPath. SDL would in
 *               fact return the same directory here, but going through it would make
 *               the Mac path depend on tvOS's choice -- see the note in port_save.c.
 *               This branch is byte-for-byte the code that used to be inline. It is the
 *               shipping target and does not change.
 *
 *   everything  SDL_GetPrefPath(org, app).
 *   else        On Windows that is %APPDATA%\<org>\<app>\ via SHGetFolderPath, which
 *               is the correct answer and is why no _WIN32 branch is needed here.
 *               On Linux it is $XDG_DATA_HOME/<app>/ (or ~/.local/share/<app>/),
 *               also correct.
 *               On tvOS this lands in Library/Caches, which the OS is free to purge, so
 *               saves do not survive. That is a real tvOS bug; it predates this file and
 *               is left visible rather than papered over. See the header comment in
 *               port_save.c.
 *
 * Note the trailing-separator asymmetry. The macOS branch returns a path with no trailing
 * separator, while SDL_GetPrefPath always appends one. Both existing callers then append
 * "/<filename>" unconditionally, so the non-Mac path picks up a doubled slash. Every POSIX
 * and Win32 filesystem collapses that, the behaviour predates this file, and normalising it
 * here would change the tvOS build for no gain, so it is preserved on purpose.
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "port_paths.h"

#ifdef _WIN32
/* Untested: no Windows host has ever built this tree. See docs/PORTING.md. */
# include <direct.h>
# define GE_MKDIR(p, m)  (_mkdir(p))
# define GE_PATHSEP(c)   ((c) == '/' || (c) == '\\')
#else
# include <sys/stat.h>
# include <sys/types.h>
# define GE_MKDIR(p, m)  (mkdir((p), (mode_t)(m)))
# define GE_PATHSEP(c)   ((c) == '/')
#endif

int gePortUserDataDir(const char *org, const char *app, char *out, size_t outsz)
{
    if (out == NULL || outsz == 0 || app == NULL || *app == '\0') {
        return -1;
    }

#ifdef GE_PLATFORM_MAC
    (void)org;
    {
        const char *home = getenv("HOME");
        if (home == NULL || *home == '\0') {
            return -1;
        }
        snprintf(out, outsz, "%s/Library/Application Support/%s", home, app);
        return 0;
    }
#else
    {
        char *pref = SDL_GetPrefPath(org != NULL ? org : app, app);
        if (pref == NULL) {
            return -1;
        }
        snprintf(out, outsz, "%s", pref);
        SDL_free(pref);
        return 0;
    }
#endif
}

int gePortMakeDir(const char *path, unsigned mode)
{
    if (path == NULL || *path == '\0') {
        errno = EINVAL;
        return -1;
    }
    /* An existing directory is success. Every caller wants "make sure this is there",
     * not "be the one who created it". errno is left as the platform set it so the
     * caller's strerror() still says something true on the failure path. */
    if (GE_MKDIR(path, mode) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

int gePortMakeDirTree(const char *path, unsigned mode)
{
    char buf[1024];
    size_t n, i;

    if (path == NULL || *path == '\0') {
        errno = EINVAL;
        return -1;
    }
    n = strlen(path);
    if (n >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, n + 1);

    /* Strip trailing separators so the final mkdir below is not a no-op on "a/b/". */
    while (n > 1 && GE_PATHSEP(buf[n - 1])) {
        buf[--n] = '\0';
    }

    /* Create each intermediate level by temporarily truncating at its separator.
     * i starts at 1 so a leading '/' is never treated as a component to create.
     * A failing intermediate is NOT fatal on its own -- it is usually EEXIST on a
     * directory we do not own, or a Windows drive prefix like "C:" -- so only the
     * final mkdir decides the return value. */
    for (i = 1; i < n; i++) {
        if (GE_PATHSEP(buf[i])) {
            char sep = buf[i];
            buf[i] = '\0';
            (void)gePortMakeDir(buf, mode);
            buf[i] = sep;
        }
    }
    return gePortMakeDir(buf, mode);
}
