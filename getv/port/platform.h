/* GoldenEye tvOS port — host services Fast3D expects from the game's platform layer.
 *
 * sm64ex declares these in src/pc/platform.h; GoldenEye's decomp has no equivalent,
 * so the port supplies them. Implemented in port/src/sys_shim.c. */
#ifndef GE_PORT_PLATFORM_H
#define GE_PORT_PLATFORM_H

#include <TargetConditionals.h>
#include <stdbool.h>

#if TARGET_OS_TV
#define PLATFORM_TVOS 1
#endif

void sys_fatal(const char *fmt, ...) __attribute__((noreturn));
void sys_sleep(double sec);

#endif
