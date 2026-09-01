/* Port-owned developer-console command core.
 *
 * This subsystem deliberately has no ImGui, SDL, renderer, game-state or allocation dependency.
 * A producer parses and submits a line into the bounded queue.  The native game thread drains a
 * snapshot of that queue at the documented hook in getv/patches/0011-netplay-tick-integration.patch.
 * Handlers therefore run before the accepted simulation tick, never from an overlay callback.
 *
 * Raw command lines are not retained.  A queued request contains only its registered command ID
 * and typed, bounded arguments.  Future diagnostic export must additionally require the command's
 * GE_CONSOLE_CMD_DIAGNOSTIC_SAFE flag and an allowlisted message_id/payload schema; it must not
 * infer safety from read-only status or export the UI-oriented message text automatically.
 *
 * The current SDL event/render producer and this pump are serialized on the native main thread.
 * The queue enforces execution ownership and bounds, not cross-thread synchronization.  A future
 * producer moved to another thread must marshal submissions or add synchronization explicitly.
 */
#ifndef GE_CONSOLE_H
#define GE_CONSOLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GE_CONSOLE_SCHEMA_VERSION       1u

#define GE_CONSOLE_MAX_COMMANDS         32
#define GE_CONSOLE_MAX_ALIASES          4
#define GE_CONSOLE_MAX_ARGS             8
#define GE_CONSOLE_MAX_ENUM_VALUES      8
#define GE_CONSOLE_MAX_PAYLOAD_VALUES   4
#define GE_CONSOLE_QUEUE_CAPACITY       16
#define GE_CONSOLE_RESULT_CAPACITY      64

#define GE_CONSOLE_MAX_COMMAND_PATH     48
#define GE_CONSOLE_MAX_ARG_NAME         24
#define GE_CONSOLE_MAX_SUMMARY          128
#define GE_CONSOLE_MAX_VALUE_TEXT       64
#define GE_CONSOLE_MAX_MESSAGE          160
#define GE_CONSOLE_MAX_LINE             256
#define GE_CONSOLE_MAX_TOKENS           16

/* Explicit values are part of the console schema.  Do not renumber them: diagnostics and future
 * automation will record the numeric code rather than locale-dependent display text. */
typedef enum GeConsoleStatus {
    GE_CONSOLE_STATUS_OK                    = 0,

    GE_CONSOLE_STATUS_EMPTY_INPUT           = 100,
    GE_CONSOLE_STATUS_UNKNOWN_COMMAND       = 101,
    GE_CONSOLE_STATUS_INVALID_SYNTAX        = 102,
    GE_CONSOLE_STATUS_LINE_TOO_LONG         = 103,
    GE_CONSOLE_STATUS_TOO_MANY_TOKENS       = 104,
    GE_CONSOLE_STATUS_TOKEN_TOO_LONG        = 105,
    GE_CONSOLE_STATUS_ARGUMENT_COUNT        = 106,
    GE_CONSOLE_STATUS_ARGUMENT_TYPE         = 107,
    GE_CONSOLE_STATUS_ARGUMENT_RANGE        = 108,
    GE_CONSOLE_STATUS_ARGUMENT_CHOICE       = 109,

    GE_CONSOLE_STATUS_QUEUE_FULL            = 200,
    GE_CONSOLE_STATUS_REGISTRY_FULL         = 201,
    GE_CONSOLE_STATUS_DUPLICATE_COMMAND     = 202,
    GE_CONSOLE_STATUS_INVALID_DEFINITION    = 203,

    GE_CONSOLE_STATUS_REFUSED_MISSION       = 300,
    GE_CONSOLE_STATUS_REFUSED_PLAYER        = 301,
    GE_CONSOLE_STATUS_REFUSED_SOLO          = 302,
    GE_CONSOLE_STATUS_REFUSED_NETPLAY       = 303,
    GE_CONSOLE_STATUS_REFUSED_DETERMINISM   = 304,

    GE_CONSOLE_STATUS_HANDLER_ERROR         = 500,
    GE_CONSOLE_STATUS_RESULT_OVERFLOW       = 600
} GeConsoleStatus;

typedef enum GeConsoleSeverity {
    GE_CONSOLE_SEVERITY_INFO    = 0,
    GE_CONSOLE_SEVERITY_WARNING = 1,
    GE_CONSOLE_SEVERITY_ERROR   = 2
} GeConsoleSeverity;

typedef enum GeConsoleArgType {
    GE_CONSOLE_ARG_INTEGER     = 1,
    GE_CONSOLE_ARG_BOOLEAN     = 2,
    GE_CONSOLE_ARG_ENUM        = 3,
    GE_CONSOLE_ARG_SYMBOL      = 4,
    GE_CONSOLE_ARG_TEXT        = 5,
    GE_CONSOLE_ARG_PLAYER_SLOT = 6
} GeConsoleArgType;

enum {
    GE_CONSOLE_CMD_READ_ONLY          = 1u << 0,
    GE_CONSOLE_CMD_MUTATES_GAME       = 1u << 1,
    GE_CONSOLE_CMD_REQUIRES_MISSION   = 1u << 2,
    GE_CONSOLE_CMD_REQUIRES_PLAYER    = 1u << 3,
    GE_CONSOLE_CMD_SOLO_ONLY          = 1u << 4,
    GE_CONSOLE_CMD_DETERMINISTIC_ONLY = 1u << 5,
    GE_CONSOLE_CMD_RECORDABLE         = 1u << 6,
    GE_CONSOLE_CMD_DIAGNOSTIC_SAFE    = 1u << 7
};

enum {
    GE_CONSOLE_CONTEXT_MISSION_ACTIVE = 1u << 0,
    GE_CONSOLE_CONTEXT_SOLO           = 1u << 1,
    GE_CONSOLE_CONTEXT_NETPLAY        = 1u << 2,
    GE_CONSOLE_CONTEXT_DETERMINISTIC  = 1u << 3,
    GE_CONSOLE_CONTEXT_HAS_STAGE      = 1u << 4
};

enum {
    GE_CONSOLE_TARGET_PLAYER = 1u << 0,
    GE_CONSOLE_TARGET_STAGE  = 1u << 1
};

typedef struct GeConsoleArgSchema {
    char name[GE_CONSOLE_MAX_ARG_NAME];
    GeConsoleArgType type;
    int required;
    int64_t minimum;
    int64_t maximum;
    unsigned int enum_count;
    char enum_values[GE_CONSOLE_MAX_ENUM_VALUES][GE_CONSOLE_MAX_VALUE_TEXT];
} GeConsoleArgSchema;

typedef struct GeConsoleCommandSpec {
    uint32_t command_id;
    uint16_t schema_version;
    uint16_t handler_id;
    uint32_t flags;
    char name[GE_CONSOLE_MAX_COMMAND_PATH];
    unsigned int alias_count;
    char aliases[GE_CONSOLE_MAX_ALIASES][GE_CONSOLE_MAX_COMMAND_PATH];
    char summary[GE_CONSOLE_MAX_SUMMARY];
    unsigned int argument_count;
    GeConsoleArgSchema arguments[GE_CONSOLE_MAX_ARGS];
} GeConsoleCommandSpec;

typedef struct GeConsoleValue {
    GeConsoleArgType type;
    int present;
    int64_t integer;
    int boolean;
    int choice_index;
    char text[GE_CONSOLE_MAX_VALUE_TEXT];
} GeConsoleValue;

typedef struct GeConsoleRequest {
    uint64_t sequence;
    uint64_t submission_tick;
    uint64_t submission_frame;
    uint32_t command_id;
    uint16_t command_version;
    uint16_t handler_id;
    uint32_t command_flags;
    unsigned int argument_count;
    GeConsoleValue arguments[GE_CONSOLE_MAX_ARGS];

    /* Registry slot is an internal dispatch token, never diagnostic data. */
    unsigned int registry_index;
} GeConsoleRequest;

typedef struct GeConsoleExecutionContext {
    uint64_t game_tick;
    uint64_t render_frame;
    uint32_t flags;
    uint32_t player_mask;
    int stage_id;
} GeConsoleExecutionContext;

typedef struct GeConsolePayloadValue {
    uint32_t field_id;
    GeConsoleValue value;
} GeConsolePayloadValue;

typedef struct GeConsoleReply {
    GeConsoleStatus status;
    GeConsoleSeverity severity;
    uint32_t message_id;
    char message[GE_CONSOLE_MAX_MESSAGE]; /* UI-only until message_id is export-allowlisted */
    uint32_t target_fields;
    int player_slot;
    int stage_id;
    unsigned int payload_count;
    GeConsolePayloadValue payload[GE_CONSOLE_MAX_PAYLOAD_VALUES];
} GeConsoleReply;

typedef struct GeConsoleResult {
    uint64_t sequence;         /* monotonically increasing result-history sequence */
    uint64_t request_sequence; /* submission identity, which may complete out of order */
    uint32_t command_id;
    uint16_t command_version;
    uint16_t handler_id;
    GeConsoleStatus status;
    GeConsoleSeverity severity;
    uint64_t submission_tick;
    uint64_t submission_frame;
    uint64_t execution_tick;
    uint64_t execution_frame;
    uint32_t message_id;
    char message[GE_CONSOLE_MAX_MESSAGE];
    uint32_t target_fields;
    int player_slot;
    int stage_id;
    unsigned int payload_count;
    GeConsolePayloadValue payload[GE_CONSOLE_MAX_PAYLOAD_VALUES];
    uint64_t history_dropped_before;
} GeConsoleResult;

typedef struct GeConsoleHistoryInfo {
    unsigned int count;
    unsigned int capacity;
    uint64_t dropped;
    GeConsoleStatus status;
} GeConsoleHistoryInfo;

typedef struct GeConsoleCompletion {
    uint32_t command_id;
    uint16_t schema_version;
    uint32_t flags;
    char name[GE_CONSOLE_MAX_COMMAND_PATH];
    char summary[GE_CONSOLE_MAX_SUMMARY];
} GeConsoleCompletion;

typedef void (*GeConsoleHandler)(const GeConsoleRequest *request,
                                 const GeConsoleExecutionContext *context,
                                 GeConsoleReply *reply,
                                 void *user);
typedef void (*GeConsoleContextProvider)(GeConsoleExecutionContext *context, void *user);

/* Initialization and registry.  Static zero initialization is also a valid empty console; reset
 * exists for tests and explicit process lifecycle boundaries.  Registration deep-copies metadata. */
void geConsoleReset(void);
GeConsoleStatus geConsoleRegister(const GeConsoleCommandSpec *spec,
                                  GeConsoleHandler handler,
                                  void *user);
unsigned int geConsoleCommandCount(void);
int geConsoleCommandAt(unsigned int index, GeConsoleCommandSpec *out);
int geConsoleCommandById(uint32_t command_id, GeConsoleCommandSpec *out);

/* Returns the total number of matching commands.  At most `max` canonical entries are copied;
 * aliases participate in matching but never create duplicate completion rows. */
unsigned int geConsoleComplete(const char *prefix, GeConsoleCompletion *out,
                               unsigned int max, int *truncated);

/* Parsing is side-effect free and retains no raw line.  Submit assigns a sequence and either
 * queues the typed request or records one immediate structured refusal/error result. */
GeConsoleStatus geConsoleParse(const char *line, GeConsoleRequest *out);
GeConsoleStatus geConsoleSubmit(const char *line, uint64_t submission_tick,
                                uint64_t submission_frame, uint64_t *sequence_out);

/* Pump pops before dispatch and drains only the requests present at entry.  A handler may submit
 * follow-up work, but it executes on the next simulation tick.  This makes re-entry and follow-up
 * submission unable to execute one request twice. */
unsigned int geConsolePump(const GeConsoleExecutionContext *context);
unsigned int geConsoleQueueCount(void);

void geConsoleSetContextProvider(GeConsoleContextProvider provider, void *user);

/* The native game-thread hook.  It obtains the authoritative input/simulation tick and the frame
 * about to be rendered, then calls geConsolePump. */
void gePortConsoleGameTick(void);

/* Preserve the existing gePortNetTick() return contract while draining only an admitted
 * simulation iteration.  This is the established boss-loop boundary after netplay approval and
 * before game simulation; render-only GETV_SIMDIV iterations do not drain the queue. */
int gePortConsoleAdmitGameTick(int admitted);

unsigned int geConsoleResultCount(void);
int geConsoleResultAt(unsigned int index, GeConsoleResult *out);
void geConsoleHistoryInfo(GeConsoleHistoryInfo *out);
void geConsoleClearHistory(void);

const char *geConsoleStatusName(GeConsoleStatus status);

#ifdef __cplusplus
}
#endif
#endif /* GE_CONSOLE_H */
