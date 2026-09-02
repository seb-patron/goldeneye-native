/* Mutation command registration and refusal behavior without a ROM, game, SDL or renderer. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned long fake_tick = 1700;
static unsigned long fake_frame = 1900;
static int pause_calls;

unsigned long gePlayerTick(void) { return fake_tick; }
unsigned long gePortRenderedFrame(void) { return fake_frame; }
int gePortSimShouldTick(void) { return 1; }
void gePortConsolePauseGameTick(void) { pause_calls++; }

#include "ge_console.c"
#include "ge_console_mutations.c"

static int failures;
static int fake_mode;
static int fake_setter_accept = 1;
static int fake_setter_apply = 1;
static int getter_calls;
static int setter_calls;
static int context_calls;
static int fake_netplay;
static int fake_mission = 1;
static int fake_solo = 1;

static void check_i(const char *what, long long got, long long want)
{
    if (got == want) { printf("  ok    %s\n", what); }
    else { printf("  FAIL  %s: got %lld want %lld\n", what, got, want); failures++; }
}

static void check_has(const char *what, const char *text, const char *needle)
{
    if (text != NULL && strstr(text, needle) != NULL) { printf("  ok    %s\n", what); }
    else { printf("  FAIL  %s: '%s' lacks '%s'\n", what, text ? text : "(null)", needle); failures++; }
}

static int provide_mode(void)
{
    getter_calls++;
    return fake_mode;
}

static int provide_set_mode(int mode)
{
    setter_calls++;
    if (!fake_setter_accept) { return 0; }
    if (fake_setter_apply) { fake_mode = mode; }
    return 1;
}

static GeConsoleMutationProvider provider(void)
{
    GeConsoleMutationProvider p;
    p.gibs_mode = provide_mode;
    p.set_gibs_mode = provide_set_mode;
    return p;
}

static void provide_context(GeConsoleExecutionContext *context, void *user)
{
    (void)user;
    context_calls++;
    memset(context, 0, sizeof *context);
    context->flags = GE_CONSOLE_CONTEXT_HAS_STAGE;
    if (fake_mission) { context->flags |= GE_CONSOLE_CONTEXT_MISSION_ACTIVE; }
    if (fake_solo) { context->flags |= GE_CONSOLE_CONTEXT_SOLO; }
    if (fake_netplay) { context->flags |= GE_CONSOLE_CONTEXT_NETPLAY; }
    context->stage_id = 34;
    context->player_mask = 1u;
}

static void reset_fakes(void)
{
    fake_mode = GE_GIBS_OFF;
    fake_setter_accept = 1;
    fake_setter_apply = 1;
    getter_calls = 0;
    setter_calls = 0;
    context_calls = 0;
    fake_netplay = 0;
    fake_mission = 1;
    fake_solo = 1;
    pause_calls = 0;
}

static GeConsoleResult run(const char *line)
{
    GeConsoleResult result;
    unsigned int before = geConsoleResultCount();
    GeConsoleStatus status = geConsoleSubmit(line, 111, 222, NULL);
    if (status == GE_CONSOLE_STATUS_OK) { gePortConsoleGameTick(); }
    memset(&result, 0, sizeof result);
    if (geConsoleResultCount() != before + 1u ||
        !geConsoleResultAt(geConsoleResultCount() - 1u, &result)) {
        printf("  FAIL  run '%s' did not produce exactly one result\n", line);
        failures++;
    }
    return result;
}

static int64_t payload_integer(const GeConsoleResult *result, uint32_t field_id, int *found)
{
    unsigned int i;
    if (found != NULL) { *found = 0; }
    for (i = 0; i < result->payload_count; i++) {
        if (result->payload[i].field_id == field_id) {
            if (found != NULL) { *found = 1; }
            return result->payload[i].value.integer;
        }
    }
    return 0;
}

static void test_install(void)
{
    GeConsoleMutationProvider p = provider();
    GeConsoleCommandSpec spec;

    geConsoleReset();
    check_i("null mutation provider refused", geConsoleMutationInstall(NULL),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    check_i("null provider registers nothing", geConsoleCommandCount(), 0);
    p.set_gibs_mode = NULL;
    check_i("partial mutation provider refused", geConsoleMutationInstall(&p),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    check_i("partial provider still registers nothing", geConsoleCommandCount(), 0);

    p = provider();
    check_i("mutation provider installs", geConsoleMutationInstall(&p), GE_CONSOLE_STATUS_OK);
    check_i("one mutation command registered", geConsoleCommandCount(), 1);
    check_i("gibs metadata found", geConsoleCommandById(GE_CONSOLE_COMMAND_GIBS, &spec), 1);
    check_i("gibs mutates game", (spec.flags & GE_CONSOLE_CMD_MUTATES_GAME) != 0, 1);
    check_i("gibs is not marked read-only", (spec.flags & GE_CONSOLE_CMD_READ_ONLY) != 0, 0);
    check_i("gibs mutation is recordable", (spec.flags & GE_CONSOLE_CMD_RECORDABLE) != 0, 1);
    check_i("gibs typed evidence is diagnostic safe",
            (spec.flags & GE_CONSOLE_CMD_DIAGNOSTIC_SAFE) != 0, 1);
    check_i("gibs may set pre-mission policy",
            (spec.flags & GE_CONSOLE_CMD_REQUIRES_MISSION) != 0, 0);
    check_i("gibs may set local multiplayer policy",
            (spec.flags & GE_CONSOLE_CMD_SOLO_ONLY) != 0, 0);
    check_i("gibs has one argument", spec.argument_count, 1);
    check_i("gibs mode is a required enum", spec.arguments[0].type, GE_CONSOLE_ARG_ENUM);
    check_i("gibs exposes four bounded choices", spec.arguments[0].enum_count, 4);
    check_has("gibs exposes high_damage spelling", spec.arguments[0].enum_values[2],
              "high_damage");

    geConsoleSetContextProvider(provide_context, NULL);
}

static void test_modes(void)
{
    GeConsoleResult result;
    int found;

    result = run("gibs explosions");
    check_i("explosions mutation succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("setter runs exactly once", setter_calls, 1);
    check_i("explosions mode applied", fake_mode, GE_GIBS_EXPLOSIONS);
    check_i("result records command id", result.command_id, GE_CONSOLE_COMMAND_GIBS);
    check_i("result records submission tick", result.submission_tick, 111);
    check_i("result records execution tick", result.execution_tick, fake_tick);
    check_i("result records execution frame", result.execution_frame, fake_frame);
    check_i("result carries stage target", result.stage_id, 34);
    check_has("result names applied policy", result.message, "mode=explosions");
    check_i("previous mode payload is present",
            payload_integer(&result, GE_CONSOLE_FIELD_GIBS_PREVIOUS_MODE, &found), GE_GIBS_OFF);
    check_i("previous mode field found", found, 1);
    check_i("current mode payload is present",
            payload_integer(&result, GE_CONSOLE_FIELD_GIBS_CURRENT_MODE, &found),
            GE_GIBS_EXPLOSIONS);

    result = run("gibs high_damage");
    check_i("high_damage mutation succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("high_damage mode applied", fake_mode, GE_GIBS_HIGH_DAMAGE);

    result = run("gibs ALWAYS");
    check_i("enum matching is case insensitive", result.status, GE_CONSOLE_STATUS_OK);
    check_i("always mode applied by canonical choice index", fake_mode, GE_GIBS_ALWAYS);

    result = run("gibs off");
    check_i("off mutation succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("off mode applied", fake_mode, GE_GIBS_OFF);
    check_i("each accepted request invokes one setter", setter_calls, 4);
}

static void test_refusals(void)
{
    GeConsoleResult result;
    int before_context;
    int before_setter;

    before_context = context_calls;
    before_setter = setter_calls;
    result = run("gibs high-damage");
    check_i("unknown mode is rejected by parser", result.status,
            GE_CONSOLE_STATUS_ARGUMENT_CHOICE);
    check_i("parse refusal does not sample context", context_calls, before_context);
    check_i("parse refusal does not call setter", setter_calls, before_setter);

    result = run("gibs");
    check_i("missing mode is rejected", result.status, GE_CONSOLE_STATUS_ARGUMENT_COUNT);
    result = run("gibs off extra");
    check_i("extra mode is rejected", result.status, GE_CONSOLE_STATUS_ARGUMENT_COUNT);

    fake_mode = GE_GIBS_EXPLOSIONS;
    fake_netplay = 1;
    before_setter = setter_calls;
    result = run("gibs always");
    check_i("netplay mutation is refused", result.status, GE_CONSOLE_STATUS_REFUSED_NETPLAY);
    check_i("netplay refusal does not call setter", setter_calls, before_setter);
    check_i("netplay refusal leaves mode unchanged", fake_mode, GE_GIBS_EXPLOSIONS);
    check_i("netplay refusal still reports stage", result.stage_id, 34);

    fake_netplay = 0;
    fake_setter_accept = 0;
    result = run("gibs always");
    check_i("provider refusal becomes handler error", result.status,
            GE_CONSOLE_STATUS_HANDLER_ERROR);
    check_i("provider refusal is an error", result.severity, GE_CONSOLE_SEVERITY_ERROR);
    check_i("provider refusal leaves mode unchanged", fake_mode, GE_GIBS_EXPLOSIONS);
    check_has("provider refusal is visible", result.message, "refused always");

    fake_setter_accept = 1;
    fake_setter_apply = 0;
    result = run("gibs high_damage");
    check_i("dishonest provider is detected", result.status, GE_CONSOLE_STATUS_HANDLER_ERROR);
    check_i("dishonest provider leaves observed mode unchanged", fake_mode, GE_GIBS_EXPLOSIONS);
    check_has("failed application is visible", result.message, "did not apply high_damage");

    fake_setter_apply = 1;
    fake_mode = 99;
    before_setter = setter_calls;
    result = run("gibs off");
    check_i("invalid provider state is detected", result.status,
            GE_CONSOLE_STATUS_HANDLER_ERROR);
    check_i("invalid prior state blocks setter", setter_calls, before_setter);
    check_has("invalid provider state is visible", result.message, "invalid state");
}

static void test_global_context(void)
{
    GeConsoleResult result;

    fake_mode = GE_GIBS_OFF;
    fake_mission = 0;
    fake_solo = 0;
    result = run("gibs always");
    check_i("global policy may change before a mission", result.status, GE_CONSOLE_STATUS_OK);
    check_i("global policy may change outside solo", fake_mode, GE_GIBS_ALWAYS);
    fake_mission = 1;
    fake_solo = 1;
}

int main(void)
{
    printf("test_console_mutations\n");
    reset_fakes();
    test_install();
    test_modes();
    test_global_context();
    test_refusals();
    return failures ? 1 : 0;
}
