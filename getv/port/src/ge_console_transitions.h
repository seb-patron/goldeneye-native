/* Controlled mission-transition commands for the port-owned developer console.
 *
 * Handlers run only through gePortConsoleGameTick().  The command core refuses netplay,
 * inactive missions and non-solo sessions before this provider can run.  The provider must use
 * the game's normal transition/relaunch mechanism; it must never assign raw stage globals.
 */
#ifndef GE_CONSOLE_TRANSITIONS_H
#define GE_CONSOLE_TRANSITIONS_H

#include "ge_console.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GE_CONSOLE_TRANSITION_SCHEMA_VERSION 1u

typedef enum GeConsoleTransitionCommandId {
    GE_CONSOLE_COMMAND_RESTART = 0x1114,
    GE_CONSOLE_COMMAND_LEVEL   = 0x1115
} GeConsoleTransitionCommandId;

typedef enum GeConsoleTransitionFieldId {
    GE_CONSOLE_FIELD_STAGE_PREVIOUS  = 0x2140,
    GE_CONSOLE_FIELD_STAGE_REQUESTED = 0x2141
} GeConsoleTransitionFieldId;

typedef struct GeConsoleTransitionProvider {
    /* Atomic contract: return 1 only after scheduling the validated stage through the game's
     * controlled transition path; return 0 without requesting a transition. */
    int (*request_stage)(int stage);
} GeConsoleTransitionProvider;

/* Copies the provider and registers restart/level.  Call after the read-only registry has
 * installed its authoritative execution-context provider. */
GeConsoleStatus geConsoleTransitionInstall(const GeConsoleTransitionProvider *provider);

#ifdef __cplusplus
}
#endif
#endif /* GE_CONSOLE_TRANSITIONS_H */
