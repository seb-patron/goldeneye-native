/* Initial read-only commands for the port-owned developer console.
 *
 * The handlers depend on this bounded provider table rather than game globals.  Production
 * installs verified game-side accessors once during boot; ROM-free tests install fakes.  Every
 * player read names an explicit slot and every objective read is capped before it reaches the
 * provider, so a corrupt count cannot create an unbounded game-thread walk.
 */
#ifndef GE_CONSOLE_COMMANDS_H
#define GE_CONSOLE_COMMANDS_H

#include "ge_console.h"
#include "ge_player_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GE_CONSOLE_READ_SCHEMA_VERSION 1u
#define GE_CONSOLE_OBJECTIVE_CAPACITY  10

/* Stable command IDs.  Diagnostics and future automation record these numbers, not display
 * names, so append new values rather than renumbering existing ones. */
typedef enum GeConsoleReadCommandId {
    GE_CONSOLE_COMMAND_HELP           = 0x1001,
    GE_CONSOLE_COMMAND_COMMANDS       = 0x1002,
    GE_CONSOLE_COMMAND_BUILD          = 0x1003,
    GE_CONSOLE_COMMAND_STATUS         = 0x1004,
    GE_CONSOLE_COMMAND_PLAYER_LIST    = 0x1010,
    GE_CONSOLE_COMMAND_PLAYER_SHOW    = 0x1011,
    GE_CONSOLE_COMMAND_WHERE          = 0x1012,
    GE_CONSOLE_COMMAND_OBJECTIVE_LIST = 0x1020
} GeConsoleReadCommandId;

/* Stable payload field IDs.  Integer coordinate fields use hundredths of one game unit. */
typedef enum GeConsoleReadFieldId {
    GE_CONSOLE_FIELD_TOTAL             = 0x2001,
    GE_CONSOLE_FIELD_CAPTURED          = 0x2002,
    GE_CONSOLE_FIELD_TRUNCATED         = 0x2003,
    GE_CONSOLE_FIELD_COMMAND_ID        = 0x2004,
    GE_CONSOLE_FIELD_COMMAND_VERSION   = 0x2005,
    GE_CONSOLE_FIELD_COMMAND_FLAGS     = 0x2006,
    GE_CONSOLE_FIELD_ARGUMENT_COUNT    = 0x2007,

    GE_CONSOLE_FIELD_PLATFORM          = 0x2010,
    GE_CONSOLE_FIELD_ARCHITECTURE      = 0x2011,
    GE_CONSOLE_FIELD_RENDERER          = 0x2012,
    GE_CONSOLE_FIELD_CONSOLE_SCHEMA    = 0x2013,

    GE_CONSOLE_FIELD_STAGE             = 0x2020,
    GE_CONSOLE_FIELD_DIFFICULTY        = 0x2021,
    GE_CONSOLE_FIELD_PLAYER_MASK       = 0x2022,
    GE_CONSOLE_FIELD_NETPLAY           = 0x2023,

    GE_CONSOLE_FIELD_PLAYER_FIELDS     = 0x2030,
    GE_CONSOLE_FIELD_HEALTH_MILLI      = 0x2031,
    GE_CONSOLE_FIELD_ARMOUR_MILLI      = 0x2032,
    GE_CONSOLE_FIELD_WEAPON            = 0x2033,
    GE_CONSOLE_FIELD_POSITION_X_CENTI  = 0x2034,
    GE_CONSOLE_FIELD_POSITION_Y_CENTI  = 0x2035,
    GE_CONSOLE_FIELD_POSITION_Z_CENTI  = 0x2036,

    GE_CONSOLE_FIELD_OBJECTIVE_PRESENT = 0x2040,
    GE_CONSOLE_FIELD_OBJECTIVE_STATUS  = 0x2041
} GeConsoleReadFieldId;

typedef enum GeConsoleBuildPlatform {
    GE_CONSOLE_PLATFORM_UNKNOWN = 0,
    GE_CONSOLE_PLATFORM_MACOS   = 1,
    GE_CONSOLE_PLATFORM_IOS     = 2,
    GE_CONSOLE_PLATFORM_TVOS    = 3,
    GE_CONSOLE_PLATFORM_LINUX   = 4,
    GE_CONSOLE_PLATFORM_WINDOWS = 5
} GeConsoleBuildPlatform;

typedef enum GeConsoleBuildArchitecture {
    GE_CONSOLE_ARCH_UNKNOWN = 0,
    GE_CONSOLE_ARCH_ARM64   = 1,
    GE_CONSOLE_ARCH_X86_64  = 2,
    GE_CONSOLE_ARCH_X86     = 3,
    GE_CONSOLE_ARCH_ARM32   = 4
} GeConsoleBuildArchitecture;

typedef enum GeConsoleBuildRenderer {
    GE_CONSOLE_RENDERER_UNKNOWN = 0,
    GE_CONSOLE_RENDERER_OPENGL  = 1,
    GE_CONSOLE_RENDERER_METAL   = 2
} GeConsoleBuildRenderer;

typedef struct GeConsoleReadProvider {
    int (*stage_id)(void);
    int (*difficulty)(void);
    int (*netplay_active)(void);
    int (*player_state)(int slot, GePlayerState *out);
    int (*objective_count)(void);
    int (*objective_status)(int index, int *out_status);
} GeConsoleReadProvider;

/* Deep-copies the provider and registers the initial read-only command set.  Call after
 * geConsoleReset(), before the first admitted game tick. */
GeConsoleStatus geConsoleReadInstall(const GeConsoleReadProvider *provider);

#ifdef __cplusplus
}
#endif
#endif /* GE_CONSOLE_COMMANDS_H */
