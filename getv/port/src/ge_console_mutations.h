/* Initial state-changing commands for the port-owned developer console.
 *
 * Mutation handlers run only through gePortConsoleGameTick(). The command core refuses every
 * mutating request during netplay before a provider callback can run. This provider is copied at
 * installation so later player/session adapters can remain narrow and ROM-free tests can use
 * fakes without linking game code.
 */
#ifndef GE_CONSOLE_MUTATIONS_H
#define GE_CONSOLE_MUTATIONS_H

#include "ge_console.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GE_CONSOLE_MUTATION_SCHEMA_VERSION 1u

typedef enum GeConsoleMutationCommandId {
    GE_CONSOLE_COMMAND_GIBS = 0x1110
} GeConsoleMutationCommandId;

typedef enum GeConsoleMutationFieldId {
    GE_CONSOLE_FIELD_GIBS_PREVIOUS_MODE = 0x2100,
    GE_CONSOLE_FIELD_GIBS_CURRENT_MODE  = 0x2101
} GeConsoleMutationFieldId;

typedef struct GeConsoleMutationProvider {
    int (*gibs_mode)(void);
    /* Atomic contract: return 1 after applying a valid mode; return 0 without changing state. */
    int (*set_gibs_mode)(int mode);
} GeConsoleMutationProvider;

/* Copies the provider and registers the currently implemented mutation commands. Call after the
 * read-only registry installs its context provider and before the first admitted game tick. */
GeConsoleStatus geConsoleMutationInstall(const GeConsoleMutationProvider *provider);

#ifdef __cplusplus
}
#endif
#endif /* GE_CONSOLE_MUTATIONS_H */
