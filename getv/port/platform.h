/* GoldenEye tvOS port - host services Fast3D expects from the game's platform layer.
 *
 * sm64ex declares these in src/pc/platform.h; GoldenEye's decomp has no equivalent,
 * so the port supplies them. Implemented in port/src/sys_shim.c. */
#ifndef GE_PORT_PLATFORM_H
#define GE_PORT_PLATFORM_H

#include <stdbool.h>

/* TargetConditionals.h is Apple-only and does not exist on Windows or Linux, so it cannot
 * be included unconditionally: four translation units pull this header in, which would
 * make it the first thing to fail on any other platform. TARGET_OS_TV only has meaning
 * inside it, so both move behind __APPLE__ and PLATFORM_TVOS simply stays undefined
 * elsewhere, which is correct -- nothing else is a television. */
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_TV
#define PLATFORM_TVOS 1
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

void sys_fatal(const char *fmt, ...) __attribute__((noreturn));
void sys_sleep(double sec);

#ifdef __cplusplus
}
#endif

#endif
