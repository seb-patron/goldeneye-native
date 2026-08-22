/* GoldenEye tvOS port — the six host services Fast3D expects.
 *
 * Perfect Dark supplies these from its own platform layer; GoldenEye's decomp has
 * no such layer, so the port provides them. See port/src/sys_shim.c. */
#ifndef GE_PORT_SYSTEM_H
#define GE_PORT_SYSTEM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void  sysLogPrintf(int level, const char *fmt, ...);
void  sysFatalError(const char *fmt, ...);
bool  sysArgCheck(const char *name);
const char *sysArgGetString(const char *name);
void  sysCpuRelax(void);
void  sysSleep(double sec);

#ifdef __cplusplus
}
#endif

#endif
