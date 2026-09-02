/* Mission transition commands without a ROM, game, window, SDL or renderer. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned long fake_tick = 2300;
static unsigned long fake_frame = 2400;
static int pause_calls;

unsigned long gePlayerTick(void) { return fake_tick; }
unsigned long gePortRenderedFrame(void) { return fake_frame; }
int gePortSimShouldTick(void) { return 1; }
void gePortConsolePauseGameTick(void) { pause_calls++; }

#include "ge_console.c"
#include "ge_console_transitions.c"
#include "ge_setup_fixups.c"

static int failures;
static int context_calls;
static int transition_calls;
static int transition_accept = 1;
static int transition_applied;
static int last_stage = -1;
static int fake_stage = 34;
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
    else { printf("  FAIL  %s: '%s' lacks '%s'\n", what,
                  text ? text : "(null)", needle); failures++; }
}

static int provide_transition(int stage)
{
    transition_calls++;
    last_stage = stage;
    if (!transition_accept) { return 0; }
    transition_applied++;
    return 1;
}

static GeConsoleTransitionProvider provider(void)
{
    GeConsoleTransitionProvider p;
    memset(&p, 0, sizeof p);
    p.request_stage = provide_transition;
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
    context->stage_id = fake_stage;
    context->player_mask = fake_mission ? 1u : 0u;
}

static void reset_fakes(void)
{
    context_calls = 0;
    transition_calls = 0;
    transition_accept = 1;
    transition_applied = 0;
    last_stage = -1;
    fake_stage = 34;
    fake_netplay = 0;
    fake_mission = 1;
    fake_solo = 1;
    pause_calls = 0;
}

static GeConsoleResult run(const char *line)
{
    GeConsoleResult result;
    unsigned int before = geConsoleResultCount();
    GeConsoleStatus status = geConsoleSubmit(line, 211, 222, NULL);
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
    GeConsoleTransitionProvider p = provider();
    GeConsoleCommandSpec spec;

    geConsoleReset();
    check_i("null transition provider refused", geConsoleTransitionInstall(NULL),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    check_i("null provider registers nothing", geConsoleCommandCount(), 0);
    p.request_stage = NULL;
    check_i("partial transition provider refused", geConsoleTransitionInstall(&p),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    check_i("partial provider registers nothing", geConsoleCommandCount(), 0);

    p = provider();
    check_i("transition provider installs", geConsoleTransitionInstall(&p),
            GE_CONSOLE_STATUS_OK);
    /* Installation copies the provider rather than retaining this stack object. */
    p.request_stage = NULL;
    check_i("two transition commands registered", geConsoleCommandCount(), 2);

    check_i("restart metadata found", geConsoleCommandById(GE_CONSOLE_COMMAND_RESTART, &spec), 1);
    check_i("restart mutates game", (spec.flags & GE_CONSOLE_CMD_MUTATES_GAME) != 0, 1);
    check_i("restart requires a mission", (spec.flags & GE_CONSOLE_CMD_REQUIRES_MISSION) != 0, 1);
    check_i("restart is solo only", (spec.flags & GE_CONSOLE_CMD_SOLO_ONLY) != 0, 1);
    check_i("restart is recordable", (spec.flags & GE_CONSOLE_CMD_RECORDABLE) != 0, 1);
    check_i("restart typed evidence is diagnostic safe",
            (spec.flags & GE_CONSOLE_CMD_DIAGNOSTIC_SAFE) != 0, 1);
    check_i("restart has no arguments", spec.argument_count, 0);

    check_i("level metadata found", geConsoleCommandById(GE_CONSOLE_COMMAND_LEVEL, &spec), 1);
    check_i("level has one argument", spec.argument_count, 1);
    check_i("level stage is integer", spec.arguments[0].type, GE_CONSOLE_ARG_INTEGER);
    check_i("level lower parse bound", spec.arguments[0].minimum, 0);
    check_i("level upper parse bound", spec.arguments[0].maximum, 99);

    geConsoleSetContextProvider(provide_context, NULL);
}

static void test_success(void)
{
    GeConsoleResult result;
    int found;

    result = run("restart");
    check_i("restart succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("restart provider called once", transition_calls, 1);
    check_i("restart schedules current stage", last_stage, 34);
    check_i("restart provider applied once", transition_applied, 1);
    check_i("restart result records command", result.command_id, GE_CONSOLE_COMMAND_RESTART);
    check_i("restart result records submission tick", result.submission_tick, 211);
    check_i("restart result records execution tick", result.execution_tick, fake_tick);
    check_i("restart result records execution frame", result.execution_frame, fake_frame);
    check_i("restart result targets stage", result.stage_id, 34);
    check_i("restart previous-stage payload",
            payload_integer(&result, GE_CONSOLE_FIELD_STAGE_PREVIOUS, &found), 34);
    check_i("restart previous-stage field found", found, 1);
    check_i("restart requested-stage payload",
            payload_integer(&result, GE_CONSOLE_FIELD_STAGE_REQUESTED, &found), 34);
    check_has("restart result says scheduled", result.message, "scheduled");

    gePortConsoleGameTick();
    check_i("second tick does not repeat restart", transition_calls, 1);
    check_i("second tick creates no duplicate result", geConsoleResultCount(), 1);

    result = run("level 33");
    check_i("level succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("level provider called once", transition_calls, 2);
    check_i("level schedules requested stage", last_stage, 33);
    check_i("level result targets requested stage", result.stage_id, 33);
    check_i("level previous-stage payload",
            payload_integer(&result, GE_CONSOLE_FIELD_STAGE_PREVIOUS, &found), 34);
    check_i("level requested-stage payload",
            payload_integer(&result, GE_CONSOLE_FIELD_STAGE_REQUESTED, &found), 33);
    check_has("level reports prior stage", result.message, "was 34");
}

static void test_validation(void)
{
    GeConsoleResult result;
    int before_context;
    int before_transition;

    before_context = context_calls;
    before_transition = transition_calls;
    result = run("level nope");
    check_i("non-numeric stage is refused", result.status, GE_CONSOLE_STATUS_ARGUMENT_TYPE);
    check_i("parse refusal does not sample context", context_calls, before_context);
    check_i("parse refusal does not call transition", transition_calls, before_transition);

    result = run("level -1");
    check_i("negative stage is refused", result.status, GE_CONSOLE_STATUS_ARGUMENT_RANGE);
    result = run("level 100");
    check_i("stage above parse bound is refused", result.status, GE_CONSOLE_STATUS_ARGUMENT_RANGE);
    result = run("level");
    check_i("missing stage is refused", result.status, GE_CONSOLE_STATUS_ARGUMENT_COUNT);
    result = run("level 33 extra");
    check_i("extra stage argument is refused", result.status,
            GE_CONSOLE_STATUS_ARGUMENT_COUNT);
    check_i("syntax and range refusals never call transition", transition_calls,
            before_transition);

    result = run("level 5");
    check_i("unavailable in-range stage is refused", result.status,
            GE_CONSOLE_STATUS_ARGUMENT_CHOICE);
    check_i("unavailable stage reports attempted target", result.stage_id, 5);
    check_i("unavailable stage does not call transition", transition_calls,
            before_transition);

    result = run("level 31");
    check_i("multiplayer-only target is context refusal", result.status,
            GE_CONSOLE_STATUS_REFUSED_CONTEXT);
    check_i("multiplayer-only target is a warning", result.severity,
            GE_CONSOLE_SEVERITY_WARNING);
    check_i("multiplayer-only target does not call transition", transition_calls,
            before_transition);
}

static void test_context_refusals(void)
{
    GeConsoleResult result;
    int before;

    before = transition_calls;
    fake_netplay = 1;
    fake_mission = 0;
    fake_solo = 0;
    result = run("restart");
    check_i("netplay refusal wins before other context", result.status,
            GE_CONSOLE_STATUS_REFUSED_NETPLAY);
    check_i("netplay refusal does not call transition", transition_calls, before);

    fake_netplay = 0;
    result = run("level 33");
    check_i("inactive mission is refused", result.status,
            GE_CONSOLE_STATUS_REFUSED_MISSION);
    check_i("inactive mission does not call transition", transition_calls, before);

    fake_mission = 1;
    result = run("restart");
    check_i("local multiplayer is refused", result.status, GE_CONSOLE_STATUS_REFUSED_SOLO);
    check_i("multiplayer refusal does not call transition", transition_calls, before);

    fake_solo = 1;
    fake_stage = 31;
    result = run("restart");
    check_i("unsupported current stage is refused", result.status,
            GE_CONSOLE_STATUS_REFUSED_CONTEXT);
    check_i("unsupported current stage does not call transition", transition_calls, before);

    fake_stage = 5;
    result = run("level 33");
    check_i("unknown current stage is refused", result.status,
            GE_CONSOLE_STATUS_REFUSED_CONTEXT);
    check_i("unknown current stage does not call transition", transition_calls, before);

    fake_stage = 34;
}

static void test_failure(void)
{
    GeConsoleResult result;
    int found;

    transition_accept = 0;
    result = run("level 33");
    check_i("provider failure becomes handler error", result.status,
            GE_CONSOLE_STATUS_HANDLER_ERROR);
    check_i("provider failure is an error", result.severity, GE_CONSOLE_SEVERITY_ERROR);
    check_i("failed transition is attempted once", transition_calls, 1);
    check_i("failed transition applies nothing", transition_applied, 0);
    check_i("failed result targets requested stage", result.stage_id, 33);
    check_i("failed result retains requested-stage payload",
            payload_integer(&result, GE_CONSOLE_FIELD_STAGE_REQUESTED, &found), 33);
    check_has("provider failure is visible", result.message, "could not schedule");

    gePortConsoleGameTick();
    check_i("failed request is not retried", transition_calls, 1);
    check_i("failed request creates one result", geConsoleResultCount(), 1);
}

static void test_reload_safe_setup_fixups(void)
{
    static int pad_section;
    static int bound_pad_section;

    check_i("null setup section never needs scaling",
            gePortSetupSectionNeedsScale(NULL), 0);
    check_i("pad section needs scaling on first load",
            gePortSetupSectionNeedsScale(&pad_section), 1);
    check_i("pad section skips scaling on same-stage reload",
            gePortSetupSectionNeedsScale(&pad_section), 0);
    check_i("bound-pad section has independent first load",
            gePortSetupSectionNeedsScale(&bound_pad_section), 1);
    check_i("bound-pad section skips scaling on later reload",
            gePortSetupSectionNeedsScale(&bound_pad_section), 0);
}

int main(void)
{
    printf("test_console_transitions\n");
    reset_fakes();
    test_install();
    test_success();

    geConsoleClearHistory();
    transition_calls = 0;
    transition_applied = 0;
    test_validation();

    geConsoleClearHistory();
    transition_calls = 0;
    transition_applied = 0;
    test_context_refusals();

    geConsoleClearHistory();
    transition_calls = 0;
    transition_applied = 0;
    test_failure();

    test_reload_safe_setup_fixups();
    return failures ? 1 : 0;
}
