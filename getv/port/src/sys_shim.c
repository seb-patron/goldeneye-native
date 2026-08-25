/* GoldenEye native port - host services for Fast3D.
 *
 * Perfect Dark provides these from its platform layer. GoldenEye's decomp has none,
 * so the port supplies them. Deliberately minimal: Fast3D only needs logging, a
 * fatal path, two argv queries (meaningless on tvOS, where there is no command
 * line), a spin hint and a sleep. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>

void sysLogPrintf(int level, const char *fmt, ...)
{
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

void sysFatalError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[getv] FATAL: ");
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    /* stdout to devicectl --console is block-buffered; flush or the message that
     * explains the crash is exactly the part that gets lost. */
    fflush(stdout);
    abort();
}

/* tvOS apps have no command line, so every option falls back to its default. */
bool sysArgCheck(const char *name) { (void)name; return false; }
const char *sysArgGetString(const char *name) { (void)name; return NULL; }

void sysCpuRelax(void) { }

void sysSleep(double sec)
{
    if (sec > 0.0) {
        SDL_Delay((Uint32)(sec * 1000.0));
    }
}
