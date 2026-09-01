/* Developer-console parser, registry, queue, result history and execution boundary.
 *
 * This includes the production C unit directly and supplies only the two established port clocks
 * used by the boss-loop wrapper.  It needs no ROM, game state, window, renderer or ImGui.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static unsigned long fake_player_tick;
static unsigned long fake_render_frame;
static int fake_sim_should_tick;
unsigned long gePlayerTick(void) { return fake_player_tick; }
unsigned long gePortRenderedFrame(void) { return fake_render_frame; }
int gePortSimShouldTick(void) { return fake_sim_should_tick; }

#include "ge_console.c"

static int failures;
static int checks;

static void check_i(const char *what, long long got, long long want)
{
    checks++;
    if (got == want) {
        printf("  ok    %-58s %lld\n", what, got);
    } else {
        printf("  FAIL  %-58s got %lld want %lld\n", what, got, want);
        failures++;
    }
}

static void check_s(const char *what, const char *got, const char *want)
{
    checks++;
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        printf("  ok    %-58s %s\n", what, got);
    } else {
        printf("  FAIL  %-58s got %s want %s\n", what,
               got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
}

static GeConsoleCommandSpec command_spec(const char *name, uint32_t id, uint32_t flags)
{
    GeConsoleCommandSpec spec;
    memset(&spec, 0, sizeof spec);
    spec.command_id = id;
    spec.schema_version = GE_CONSOLE_SCHEMA_VERSION;
    spec.handler_id = (uint16_t)(100u + id);
    spec.flags = flags;
    snprintf(spec.name, sizeof spec.name, "%s", name);
    snprintf(spec.summary, sizeof spec.summary, "test metadata for %s", name);
    return spec;
}

static int handler_calls;
static int handler_order[256];
static int handler_order_count;
static uint64_t handler_tick;
static uint64_t handler_frame;
static unsigned int reentrant_pump_count;
static int submit_follow_up;

static void record_handler(const GeConsoleRequest *request,
                           const GeConsoleExecutionContext *context,
                           GeConsoleReply *reply,
                           void *user)
{
    int marker = (int)(intptr_t)user;
    handler_calls++;
    handler_tick = context->game_tick;
    handler_frame = context->render_frame;
    if (request->argument_count > 0 && request->arguments[0].present &&
        (request->arguments[0].type == GE_CONSOLE_ARG_INTEGER ||
         request->arguments[0].type == GE_CONSOLE_ARG_PLAYER_SLOT)) {
        marker = (int)request->arguments[0].integer;
    }
    if (handler_order_count < (int)(sizeof handler_order / sizeof handler_order[0])) {
        handler_order[handler_order_count++] = marker;
    }
    reply->status = GE_CONSOLE_STATUS_OK;
    reply->severity = GE_CONSOLE_SEVERITY_INFO;
    reply->message_id = 42;
    snprintf(reply->message, sizeof reply->message, "handled");
    reply->payload_count = 1;
    reply->payload[0].field_id = 7;
    reply->payload[0].value.type = GE_CONSOLE_ARG_INTEGER;
    reply->payload[0].value.present = 1;
    reply->payload[0].value.integer = marker;

    if (submit_follow_up && request->command_id == 9) {
        uint64_t ignored;
        submit_follow_up = 0;
        geConsoleSubmit("status", 700, 800, &ignored);
        reentrant_pump_count = geConsolePump(context);
    }
}

static void bad_reply_handler(const GeConsoleRequest *request,
                              const GeConsoleExecutionContext *context,
                              GeConsoleReply *reply,
                              void *user)
{
    (void)request; (void)context; (void)user;
    handler_calls++;
    reply->status = (GeConsoleStatus)9999;
    reply->severity = GE_CONSOLE_SEVERITY_INFO;
}

static void add_integer_arg(GeConsoleCommandSpec *spec, const char *name,
                            int required, int64_t minimum, int64_t maximum)
{
    GeConsoleArgSchema *arg = &spec->arguments[spec->argument_count++];
    memset(arg, 0, sizeof *arg);
    snprintf(arg->name, sizeof arg->name, "%s", name);
    arg->type = GE_CONSOLE_ARG_INTEGER;
    arg->required = required;
    arg->minimum = minimum;
    arg->maximum = maximum;
}

static void add_simple_arg(GeConsoleCommandSpec *spec, const char *name,
                           GeConsoleArgType type, int required)
{
    GeConsoleArgSchema *arg = &spec->arguments[spec->argument_count++];
    memset(arg, 0, sizeof *arg);
    snprintf(arg->name, sizeof arg->name, "%s", name);
    arg->type = type;
    arg->required = required;
}

static void add_enum_arg(GeConsoleCommandSpec *spec, const char *name,
                         const char *a, const char *b, const char *c)
{
    GeConsoleArgSchema *arg = &spec->arguments[spec->argument_count++];
    memset(arg, 0, sizeof *arg);
    snprintf(arg->name, sizeof arg->name, "%s", name);
    arg->type = GE_CONSOLE_ARG_ENUM;
    arg->required = 1;
    if (a) { snprintf(arg->enum_values[arg->enum_count++], GE_CONSOLE_MAX_VALUE_TEXT, "%s", a); }
    if (b) { snprintf(arg->enum_values[arg->enum_count++], GE_CONSOLE_MAX_VALUE_TEXT, "%s", b); }
    if (c) { snprintf(arg->enum_values[arg->enum_count++], GE_CONSOLE_MAX_VALUE_TEXT, "%s", c); }
}

static void register_core_commands(void)
{
    GeConsoleCommandSpec spec;

    spec = command_spec("status", 1,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE);
    spec.alias_count = 1;
    snprintf(spec.aliases[0], sizeof spec.aliases[0], "st");
    check_i("register status", geConsoleRegister(&spec, record_handler, (void *)(intptr_t)1),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("player show", 2,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_REQUIRES_MISSION |
                        GE_CONSOLE_CMD_REQUIRES_PLAYER | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE);
    add_simple_arg(&spec, "slot", GE_CONSOLE_ARG_PLAYER_SLOT, 1);
    check_i("register player show", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("gibs", 3,
                        GE_CONSOLE_CMD_MUTATES_GAME | GE_CONSOLE_CMD_REQUIRES_MISSION |
                        GE_CONSOLE_CMD_SOLO_ONLY | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE);
    add_enum_arg(&spec, "policy", "off", "explosions", "always");
    check_i("register gibs", geConsoleRegister(&spec, record_handler, (void *)(intptr_t)3),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("say", 4, GE_CONSOLE_CMD_READ_ONLY);
    add_simple_arg(&spec, "message", GE_CONSOLE_ARG_TEXT, 1);
    check_i("register non-diagnostic text", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("range", 5,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE);
    add_integer_arg(&spec, "amount", 1, -2, 10);
    check_i("register bounded integer", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("toggle", 6,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE);
    add_simple_arg(&spec, "state", GE_CONSOLE_ARG_BOOLEAN, 1);
    check_i("register boolean", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("mode", 7,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE);
    add_enum_arg(&spec, "name", "low", "high damage", NULL);
    check_i("register quoted enum", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("weapon", 8,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE);
    add_simple_arg(&spec, "name", GE_CONSOLE_ARG_SYMBOL, 1);
    check_i("register symbol", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("follow", 9, GE_CONSOLE_CMD_READ_ONLY);
    check_i("register follow-up harness", geConsoleRegister(&spec, record_handler,
                                                             (void *)(intptr_t)9),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("solo", 10,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_SOLO_ONLY);
    check_i("register solo guard", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("deterministic", 11,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_DETERMINISTIC_ONLY);
    check_i("register determinism guard", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_OK);

    spec = command_spec("badreply", 12, GE_CONSOLE_CMD_READ_ONLY);
    check_i("register invalid-reply harness", geConsoleRegister(&spec, bad_reply_handler, NULL),
            GE_CONSOLE_STATUS_OK);
}

static void test_registry(void)
{
    GeConsoleCommandSpec spec, copy;
    GeConsoleCompletion completion[2];
    int truncated = 0;

    printf("\nregistry and completion metadata\n\n");
    geConsoleReset();
    register_core_commands();
    check_i("registry count", geConsoleCommandCount(), 12);
    check_i("command lookup by index", geConsoleCommandAt(0, &copy), 1);
    check_s("canonical metadata survives registration", copy.name, "status");
    check_s("summary metadata survives registration", copy.summary, "test metadata for status");
    check_i("stable command schema version", copy.schema_version, GE_CONSOLE_SCHEMA_VERSION);
    check_i("command lookup by id", geConsoleCommandById(2, &copy), 1);
    check_s("multi-word canonical metadata", copy.name, "player show");
    check_i("missing command id", geConsoleCommandById(999, &copy), 0);

    memset(completion, 0, sizeof completion);
    check_i("completion matches canonical prefix", geConsoleComplete("pla", completion, 2,
                                                                      &truncated), 1);
    check_s("completion returns canonical name", completion[0].name, "player show");
    check_i("completion preserves flags", completion[0].flags & GE_CONSOLE_CMD_REQUIRES_PLAYER,
            GE_CONSOLE_CMD_REQUIRES_PLAYER);
    check_i("completion matches alias prefix", geConsoleComplete("st", completion, 2,
                                                                  &truncated), 1);
    check_s("alias completion still returns canonical", completion[0].name, "status");
    check_i("bounded completion reports all matches", geConsoleComplete("", completion, 2,
                                                                         &truncated), 12);
    check_i("bounded completion reports truncation", truncated, 1);

    spec = command_spec("another", 1, GE_CONSOLE_CMD_READ_ONLY);
    check_i("duplicate stable id refused", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_DUPLICATE_COMMAND);
    spec = command_spec("st", 50, GE_CONSOLE_CMD_READ_ONLY);
    check_i("alias/canonical collision refused", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_DUPLICATE_COMMAND);
    spec = command_spec("unsafe", 51,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE);
    add_simple_arg(&spec, "text", GE_CONSOLE_ARG_TEXT, 1);
    check_i("diagnostic-safe free text refused", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    spec = command_spec("targetless", 52,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_REQUIRES_PLAYER);
    check_i("requires-player without slot schema refused",
            geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    spec = command_spec("twomodes", 53,
                        GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_MUTATES_GAME);
    check_i("read-only plus mutation refused", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    spec = command_spec("unknownflag", 55, GE_CONSOLE_CMD_READ_ONLY | (1u << 31));
    check_i("unknown command flag refused", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    spec = command_spec("optionalorder", 54, GE_CONSOLE_CMD_READ_ONLY);
    add_integer_arg(&spec, "optional", 0, 0, 5);
    add_integer_arg(&spec, "required", 1, 0, 5);
    check_i("required argument after optional refused",
            geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
}

static void test_parser(void)
{
    GeConsoleRequest request;
    char line[GE_CONSOLE_MAX_LINE + 8];
    unsigned int i;

    printf("\nparser and typed arguments\n\n");
    geConsoleReset();
    register_core_commands();

    check_i("empty input has stable code", geConsoleParse("   \t", &request),
            GE_CONSOLE_STATUS_EMPTY_INPUT);
    check_i("NULL input is invalid syntax", geConsoleParse(NULL, &request),
            GE_CONSOLE_STATUS_INVALID_SYNTAX);
    check_i("unknown command has stable code", geConsoleParse("missing", &request),
            GE_CONSOLE_STATUS_UNKNOWN_COMMAND);
    check_i("case-insensitive multi-word command", geConsoleParse("PLAYER SHOW 2", &request),
            GE_CONSOLE_STATUS_OK);
    check_i("multi-word command id", request.command_id, 2);
    check_i("player slot typed", request.arguments[0].type, GE_CONSOLE_ARG_PLAYER_SLOT);
    check_i("player slot value", request.arguments[0].integer, 2);
    check_i("player slot below range", geConsoleParse("player show -1", &request),
            GE_CONSOLE_STATUS_ARGUMENT_RANGE);
    check_i("player slot above range", geConsoleParse("player show 4", &request),
            GE_CONSOLE_STATUS_ARGUMENT_RANGE);
    check_i("missing required argument", geConsoleParse("player show", &request),
            GE_CONSOLE_STATUS_ARGUMENT_COUNT);
    check_i("extra argument", geConsoleParse("status now", &request),
            GE_CONSOLE_STATUS_ARGUMENT_COUNT);
    check_i("alias parses", geConsoleParse("st", &request), GE_CONSOLE_STATUS_OK);
    check_i("alias resolves canonical id", request.command_id, 1);

    check_i("quoted text parses", geConsoleParse("say \"two words\"", &request),
            GE_CONSOLE_STATUS_OK);
    check_s("quoted text value", request.arguments[0].text, "two words");
    check_i("escaped quote parses", geConsoleParse("say \"a \\\"quote\\\"\"", &request),
            GE_CONSOLE_STATUS_OK);
    check_s("escaped quote value", request.arguments[0].text, "a \"quote\"");
    check_i("unterminated quote refused", geConsoleParse("say \"open", &request),
            GE_CONSOLE_STATUS_INVALID_SYNTAX);
    check_i("dangling escape refused", geConsoleParse("say bad\\", &request),
            GE_CONSOLE_STATUS_INVALID_SYNTAX);
    check_i("unknown escape refused", geConsoleParse("say bad\\q", &request),
            GE_CONSOLE_STATUS_INVALID_SYNTAX);

    check_i("bounded integer minimum", geConsoleParse("range -2", &request),
            GE_CONSOLE_STATUS_OK);
    check_i("bounded integer maximum", geConsoleParse("range 10", &request),
            GE_CONSOLE_STATUS_OK);
    check_i("integer below bound refused", geConsoleParse("range -3", &request),
            GE_CONSOLE_STATUS_ARGUMENT_RANGE);
    check_i("integer above bound refused", geConsoleParse("range 11", &request),
            GE_CONSOLE_STATUS_ARGUMENT_RANGE);
    check_i("integer shape refused", geConsoleParse("range 1x", &request),
            GE_CONSOLE_STATUS_ARGUMENT_TYPE);

    check_i("boolean on parses", geConsoleParse("toggle on", &request),
            GE_CONSOLE_STATUS_OK);
    check_i("boolean normalized true", request.arguments[0].boolean, 1);
    check_s("boolean canonical text", request.arguments[0].text, "on");
    check_i("boolean false parses", geConsoleParse("toggle false", &request),
            GE_CONSOLE_STATUS_OK);
    check_i("boolean normalized false", request.arguments[0].boolean, 0);
    check_i("bad boolean refused", geConsoleParse("toggle maybe", &request),
            GE_CONSOLE_STATUS_ARGUMENT_TYPE);

    check_i("quoted registered enum parses", geConsoleParse("mode \"high damage\"", &request),
            GE_CONSOLE_STATUS_OK);
    check_i("enum index stable", request.arguments[0].choice_index, 1);
    check_s("enum stores registered spelling", request.arguments[0].text, "high damage");
    check_i("unknown enum refused", geConsoleParse("mode medium", &request),
            GE_CONSOLE_STATUS_ARGUMENT_CHOICE);
    check_i("safe symbol parses", geConsoleParse("weapon rcp-90.v2", &request),
            GE_CONSOLE_STATUS_OK);
    check_i("path-like symbol refused", geConsoleParse("weapon /tmp/secret", &request),
            GE_CONSOLE_STATUS_ARGUMENT_TYPE);

    memset(line, 'x', GE_CONSOLE_MAX_LINE + 1);
    line[GE_CONSOLE_MAX_LINE + 1] = '\0';
    check_i("line bound enforced", geConsoleParse(line, &request),
            GE_CONSOLE_STATUS_LINE_TOO_LONG);
    snprintf(line, sizeof line, "say ");
    for (i = 4; i < 4 + GE_CONSOLE_MAX_VALUE_TEXT; i++) { line[i] = 'x'; }
    line[4 + GE_CONSOLE_MAX_VALUE_TEXT] = '\0';
    check_i("token bound enforced", geConsoleParse(line, &request),
            GE_CONSOLE_STATUS_TOKEN_TOO_LONG);
    snprintf(line, sizeof line, "status");
    for (i = 0; i < GE_CONSOLE_MAX_TOKENS; i++) { strcat(line, " x"); }
    check_i("token-count bound enforced", geConsoleParse(line, &request),
            GE_CONSOLE_STATUS_TOO_MANY_TOKENS);

    check_i("status code 101 stays unknown-command", GE_CONSOLE_STATUS_UNKNOWN_COMMAND, 101);
    check_i("status code 200 stays queue-full", GE_CONSOLE_STATUS_QUEUE_FULL, 200);
    check_i("status code 303 stays netplay refusal", GE_CONSOLE_STATUS_REFUSED_NETPLAY, 303);
    check_s("stable status name", geConsoleStatusName(GE_CONSOLE_STATUS_ARGUMENT_CHOICE),
            "argument_choice");
}

static void clear_handler_capture(void)
{
    handler_calls = 0;
    handler_order_count = 0;
    handler_tick = 0;
    handler_frame = 0;
    reentrant_pump_count = 0;
    submit_follow_up = 0;
    memset(handler_order, 0, sizeof handler_order);
}

static GeConsoleExecutionContext full_context(uint64_t tick, uint64_t frame)
{
    GeConsoleExecutionContext context;
    memset(&context, 0, sizeof context);
    context.game_tick = tick;
    context.render_frame = frame;
    context.flags = GE_CONSOLE_CONTEXT_MISSION_ACTIVE | GE_CONSOLE_CONTEXT_SOLO |
                    GE_CONSOLE_CONTEXT_DETERMINISTIC | GE_CONSOLE_CONTEXT_HAS_STAGE;
    context.player_mask = 0xfu;
    context.stage_id = 33;
    return context;
}

static void test_queue_and_results(void)
{
    GeConsoleExecutionContext context;
    GeConsoleResult result;
    GeConsoleHistoryInfo history;
    uint64_t sequence;
    int i;

    printf("\nbounded queue, exactly-once pump and structured results\n\n");
    geConsoleReset();
    register_core_commands();
    clear_handler_capture();
    context = full_context(50, 70);

    check_i("submit first typed request", geConsoleSubmit("range 1", 10, 20, &sequence),
            GE_CONSOLE_STATUS_OK);
    check_i("first sequence starts at one", sequence, 1);
    check_i("submit second typed request", geConsoleSubmit("range 2", 11, 21, &sequence),
            GE_CONSOLE_STATUS_OK);
    check_i("submit third typed request", geConsoleSubmit("range 3", 12, 22, &sequence),
            GE_CONSOLE_STATUS_OK);
    check_i("queue exposes bounded count", geConsoleQueueCount(), 3);
    check_i("pump drains entry snapshot", geConsolePump(&context), 3);
    check_i("all three handlers called", handler_calls, 3);
    check_i("in-order execution 1", handler_order[0], 1);
    check_i("in-order execution 2", handler_order[1], 2);
    check_i("in-order execution 3", handler_order[2], 3);
    check_i("second pump executes nothing", geConsolePump(&context), 0);
    check_i("second pump cannot duplicate handlers", handler_calls, 3);
    check_i("queue empty after pump", geConsoleQueueCount(), 0);
    check_i("one structured result per request", geConsoleResultCount(), 3);
    check_i("oldest result readable", geConsoleResultAt(0, &result), 1);
    check_i("result sequence", result.sequence, 1);
    check_i("result links request sequence", result.request_sequence, 1);
    check_i("result command id", result.command_id, 5);
    check_i("result submission tick", result.submission_tick, 10);
    check_i("result submission frame", result.submission_frame, 20);
    check_i("result execution tick", result.execution_tick, 50);
    check_i("result execution frame", result.execution_frame, 70);
    check_i("result stage target present", result.target_fields & GE_CONSOLE_TARGET_STAGE,
            GE_CONSOLE_TARGET_STAGE);
    check_i("result stage target", result.stage_id, 33);
    check_i("handler message id", result.message_id, 42);
    check_s("handler message", result.message, "handled");
    check_i("typed result payload count", result.payload_count, 1);
    check_i("typed result payload field", result.payload[0].field_id, 7);
    check_i("typed result payload value", result.payload[0].value.integer, 1);

    check_i("invalid line is refused immediately",
            geConsoleSubmit("missing secret", 30, 40, &sequence),
            GE_CONSOLE_STATUS_UNKNOWN_COMMAND);
    check_i("invalid line is never queued", geConsoleQueueCount(), 0);
    check_i("invalid line has one structured result", geConsoleResultCount(), 4);
    check_i("invalid result readable", geConsoleResultAt(3, &result), 1);
    check_i("invalid result keeps no command id", result.command_id, 0);
    check_i("invalid result stable status", result.status, GE_CONSOLE_STATUS_UNKNOWN_COMMAND);
    check_s("invalid result contains allowlisted status text", result.message, "unknown_command");
    check_i("invalid result has no execution tick", result.execution_tick, 0);

    geConsoleReset();
    register_core_commands();
    clear_handler_capture();
    for (i = 0; i < GE_CONSOLE_QUEUE_CAPACITY; i++) {
        check_i("queue capacity accepts request", geConsoleSubmit("status", 1, 2, &sequence),
                GE_CONSOLE_STATUS_OK);
    }
    check_i("queue reaches exact fixed capacity", geConsoleQueueCount(),
            GE_CONSOLE_QUEUE_CAPACITY);
    check_i("full queue refuses without overwrite", geConsoleSubmit("status", 1, 2, &sequence),
            GE_CONSOLE_STATUS_QUEUE_FULL);
    check_i("full queue count unchanged", geConsoleQueueCount(), GE_CONSOLE_QUEUE_CAPACITY);
    check_i("full queue refusal recorded", geConsoleResultAt(0, &result), 1);
    check_i("full queue result status", result.status, GE_CONSOLE_STATUS_QUEUE_FULL);
    check_i("full queue result has first completion sequence", result.sequence, 1);
    check_i("full queue result links refused request", result.request_sequence,
            GE_CONSOLE_QUEUE_CAPACITY + 1);
    check_i("accepted queue drains exactly once", geConsolePump(&context),
            GE_CONSOLE_QUEUE_CAPACITY);
    check_i("accepted handlers all ran", handler_calls, GE_CONSOLE_QUEUE_CAPACITY);
    check_i("no overwrite-created extra execution", geConsolePump(&context), 0);
    check_i("completion history stays monotonic", geConsoleResultAt(1, &result), 1);
    check_i("first executed request completed second", result.sequence, 2);
    check_i("first executed result links request one", result.request_sequence, 1);

    geConsoleReset();
    register_core_commands();
    clear_handler_capture();
    submit_follow_up = 1;
    check_i("submit handler that follows up", geConsoleSubmit("follow", 1, 2, &sequence),
            GE_CONSOLE_STATUS_OK);
    check_i("first pump executes original only", geConsolePump(&context), 1);
    check_i("re-entrant pump is inert", reentrant_pump_count, 0);
    check_i("follow-up stays queued for next tick", geConsoleQueueCount(), 1);
    check_i("original called once", handler_calls, 1);
    context.game_tick++;
    context.render_frame++;
    check_i("next pump executes follow-up", geConsolePump(&context), 1);
    check_i("follow-up called once", handler_calls, 2);
    check_i("follow-up result has next tick", geConsoleResultAt(1, &result), 1);
    check_i("follow-up execution tick", result.execution_tick, 51);

    geConsoleReset();
    register_core_commands();
    clear_handler_capture();
    for (i = 0; i < GE_CONSOLE_RESULT_CAPACITY + 16; i++) {
        geConsoleSubmit("status", (uint64_t)i, (uint64_t)i, &sequence);
        geConsolePump(&context);
    }
    geConsoleHistoryInfo(&history);
    check_i("result ring remains bounded", history.count, GE_CONSOLE_RESULT_CAPACITY);
    check_i("result ring reports dropped total", history.dropped, 16);
    check_i("result ring exposes overflow status", history.status,
            GE_CONSOLE_STATUS_RESULT_OVERFLOW);
    check_i("oldest surviving result readable", geConsoleResultAt(0, &result), 1);
    check_i("sequence gap exposes overwritten history", result.sequence, 17);
    check_i("newest surviving result readable",
            geConsoleResultAt(GE_CONSOLE_RESULT_CAPACITY - 1, &result), 1);
    check_i("newest sequence retained", result.sequence, GE_CONSOLE_RESULT_CAPACITY + 16);
    check_i("each result records cumulative loss", result.history_dropped_before, 16);
    geConsoleClearHistory();
    geConsoleHistoryInfo(&history);
    check_i("clear empties history", history.count, 0);
    check_i("clear resets overflow report", history.status, GE_CONSOLE_STATUS_OK);
}

static void test_context_refusals(void)
{
    GeConsoleExecutionContext context;
    GeConsoleResult result;
    uint64_t sequence;

    printf("\ncontext validation and refusal paths\n\n");
    geConsoleReset();
    register_core_commands();
    clear_handler_capture();

    memset(&context, 0, sizeof context);
    context.game_tick = 10;
    context.render_frame = 20;
    geConsoleSubmit("player show 2", 1, 2, &sequence);
    geConsolePump(&context);
    geConsoleResultAt(0, &result);
    check_i("mission requirement refuses before handler", result.status,
            GE_CONSOLE_STATUS_REFUSED_MISSION);
    check_i("mission refusal does not call handler", handler_calls, 0);

    context.flags = GE_CONSOLE_CONTEXT_MISSION_ACTIVE;
    geConsoleSubmit("player show 2", 1, 2, &sequence);
    geConsolePump(&context);
    geConsoleResultAt(1, &result);
    check_i("absent explicit slot refused", result.status, GE_CONSOLE_STATUS_REFUSED_PLAYER);
    check_i("absent player still records requested target", result.player_slot, 2);
    check_i("absent player target marked present", result.target_fields & GE_CONSOLE_TARGET_PLAYER,
            GE_CONSOLE_TARGET_PLAYER);
    check_i("player refusal does not call handler", handler_calls, 0);

    context.player_mask = 1u << 2;
    geConsoleSubmit("player show 2", 1, 2, &sequence);
    geConsolePump(&context);
    geConsoleResultAt(2, &result);
    check_i("present explicit slot executes", result.status, GE_CONSOLE_STATUS_OK);
    check_i("resolved slot recorded", result.player_slot, 2);
    check_i("present slot handler called", handler_calls, 1);

    context.flags = GE_CONSOLE_CONTEXT_NETPLAY;
    geConsoleSubmit("gibs always", 1, 2, &sequence);
    geConsolePump(&context);
    geConsoleResultAt(3, &result);
    check_i("mutation refuses netplay before other context", result.status,
            GE_CONSOLE_STATUS_REFUSED_NETPLAY);
    check_i("netplay refusal has warning severity", result.severity,
            GE_CONSOLE_SEVERITY_WARNING);
    check_i("netplay refusal does not partially call handler", handler_calls, 1);

    context.flags = 0;
    geConsoleSubmit("solo", 1, 2, &sequence);
    geConsolePump(&context);
    geConsoleResultAt(4, &result);
    check_i("solo-only command refuses non-solo", result.status,
            GE_CONSOLE_STATUS_REFUSED_SOLO);
    context.flags = GE_CONSOLE_CONTEXT_SOLO;
    geConsoleSubmit("solo", 1, 2, &sequence);
    geConsolePump(&context);
    geConsoleResultAt(5, &result);
    check_i("solo-only command executes in solo", result.status, GE_CONSOLE_STATUS_OK);

    context.flags = 0;
    geConsoleSubmit("deterministic", 1, 2, &sequence);
    geConsolePump(&context);
    geConsoleResultAt(6, &result);
    check_i("determinism requirement refuses unknown context", result.status,
            GE_CONSOLE_STATUS_REFUSED_DETERMINISM);
    context.flags = GE_CONSOLE_CONTEXT_DETERMINISTIC;
    geConsoleSubmit("deterministic", 1, 2, &sequence);
    geConsolePump(&context);
    geConsoleResultAt(7, &result);
    check_i("deterministic command executes when declared", result.status,
            GE_CONSOLE_STATUS_OK);

    geConsoleSubmit("badreply", 1, 2, &sequence);
    geConsolePump(&context);
    geConsoleResultAt(8, &result);
    check_i("invalid handler reply becomes internal error", result.status,
            GE_CONSOLE_STATUS_HANDLER_ERROR);
    check_i("invalid handler reply becomes error severity", result.severity,
            GE_CONSOLE_SEVERITY_ERROR);
}

static void provide_context(GeConsoleExecutionContext *context, void *user)
{
    (void)user;
    context->flags = GE_CONSOLE_CONTEXT_MISSION_ACTIVE | GE_CONSOLE_CONTEXT_SOLO;
    context->player_mask = 1;
    context->game_tick = 1;      /* wrapper must replace both stale provider identities */
    context->render_frame = 2;
}

static void test_native_hook_wrapper(void)
{
    GeConsoleResult result;
    uint64_t sequence;

    printf("\nnative game-thread wrapper identities\n\n");
    geConsoleReset();
    register_core_commands();
    clear_handler_capture();
    fake_player_tick = 900;
    fake_render_frame = 45;
    geConsoleSetContextProvider(provide_context, NULL);
    check_i("request queued before native hook", geConsoleSubmit("status", 899, 44, &sequence),
            GE_CONSOLE_STATUS_OK);
    gePortConsoleGameTick();
    check_i("native hook drains queued request", handler_calls, 1);
    check_i("handler sees authoritative input/simulation tick", handler_tick, 900);
    check_i("handler sees frame about to render", handler_frame, 45);
    check_i("native result readable", geConsoleResultAt(0, &result), 1);
    check_i("native result execution tick", result.execution_tick, 900);
    check_i("native result execution frame", result.execution_frame, 45);
    gePortConsoleGameTick();
    check_i("second native hook cannot repeat drained request", handler_calls, 1);

    check_i("stalled netplay iteration preserves refusal", gePortConsoleAdmitGameTick(0), 0);
    check_i("stalled netplay iteration does not drain", handler_calls, 1);
    check_i("second request queued", geConsoleSubmit("status", 900, 45, &sequence),
            GE_CONSOLE_STATUS_OK);
    fake_sim_should_tick = 0;
    check_i("render-only iteration remains admitted", gePortConsoleAdmitGameTick(1), 1);
    check_i("render-only iteration does not drain", handler_calls, 1);
    fake_sim_should_tick = 1;
    check_i("simulation iteration remains admitted", gePortConsoleAdmitGameTick(1), 1);
    check_i("admitted simulation iteration drains once", handler_calls, 2);
    check_i("next admitted iteration cannot repeat request", gePortConsoleAdmitGameTick(1), 1);
    check_i("admitted hook remains exactly once", handler_calls, 2);
}

static void test_registry_bound(void)
{
    GeConsoleCommandSpec spec;
    char name[GE_CONSOLE_MAX_COMMAND_PATH];
    unsigned int i;

    printf("\nregistry bound\n\n");
    geConsoleReset();
    for (i = 0; i < GE_CONSOLE_MAX_COMMANDS; i++) {
        snprintf(name, sizeof name, "command%u", i);
        spec = command_spec(name, 1000u + i, GE_CONSOLE_CMD_READ_ONLY);
        check_i("registry accepts fixed-capacity entry",
                geConsoleRegister(&spec, record_handler, NULL), GE_CONSOLE_STATUS_OK);
    }
    spec = command_spec("one_more", 2000, GE_CONSOLE_CMD_READ_ONLY);
    check_i("registry refuses beyond fixed capacity", geConsoleRegister(&spec, record_handler, NULL),
            GE_CONSOLE_STATUS_REGISTRY_FULL);
    check_i("registry count remains bounded", geConsoleCommandCount(), GE_CONSOLE_MAX_COMMANDS);
}

int main(void)
{
    printf("developer console core\n");
    test_registry();
    test_parser();
    test_queue_and_results();
    test_context_refusals();
    test_native_hook_wrapper();
    test_registry_bound();
    printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
