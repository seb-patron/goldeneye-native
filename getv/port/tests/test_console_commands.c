/* Read-only console handlers and provider composition, without a ROM, game, SDL or renderer. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned long fake_tick = 700;
static unsigned long fake_frame = 900;
static int pause_calls;

unsigned long gePlayerTick(void) { return fake_tick; }
unsigned long gePortRenderedFrame(void) { return fake_frame; }
int gePortSimShouldTick(void) { return 1; }
void gePortConsolePauseGameTick(void) { pause_calls++; }

#include "ge_console.c"
#include "ge_console_commands.c"

static int failures;

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

static int fake_stage = 34;
static int fake_difficulty = 2;
static int fake_netplay;
static GePlayerState fake_players[GE_MAX_SLOTS];
static int fake_objective_total;
static int fake_objective_present[32];
static int fake_objective_statuses[32];
static int stage_calls;
static int player_calls;
static int objective_count_calls;
static int objective_status_calls;
static int objective_max_index;

static int provide_stage(void) { stage_calls++; return fake_stage; }
static int provide_difficulty(void) { return fake_difficulty; }
static int provide_netplay(void) { return fake_netplay; }

static int provide_player(int slot, GePlayerState *out)
{
    player_calls++;
    if (slot < 0 || slot >= GE_MAX_SLOTS || out == NULL || !fake_players[slot].present) {
        return 0;
    }
    *out = fake_players[slot];
    return 1;
}

static int provide_objective_count(void)
{
    objective_count_calls++;
    return fake_objective_total;
}

static int provide_objective_status(int index, int *out)
{
    objective_status_calls++;
    if (index > objective_max_index) { objective_max_index = index; }
    if (index < 0 || index >= 32 || out == NULL || !fake_objective_present[index]) { return 0; }
    *out = fake_objective_statuses[index];
    return 1;
}

static GeConsoleReadProvider provider(void)
{
    GeConsoleReadProvider p;
    p.stage_id = provide_stage;
    p.difficulty = provide_difficulty;
    p.netplay_active = provide_netplay;
    p.player_state = provide_player;
    p.objective_count = provide_objective_count;
    p.objective_status = provide_objective_status;
    return p;
}

static void reset_fakes(void)
{
    int i;
    memset(fake_players, 0, sizeof fake_players);
    memset(fake_objective_present, 0, sizeof fake_objective_present);
    memset(fake_objective_statuses, 0, sizeof fake_objective_statuses);
    fake_stage = 34;
    fake_difficulty = 2;
    fake_netplay = 0;
    fake_objective_total = 0;
    stage_calls = 0;
    player_calls = 0;
    objective_count_calls = 0;
    objective_status_calls = 0;
    objective_max_index = -1;
    pause_calls = 0;
    for (i = 0; i < GE_MAX_SLOTS; i++) { fake_players[i].room = -1; }
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

static void test_install_and_idle(void)
{
    GeConsoleReadProvider p = provider();
    GeConsoleCommandSpec spec;

    geConsoleReset();
    check_i("null provider refused", geConsoleReadInstall(NULL),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    check_i("invalid provider registers nothing", geConsoleCommandCount(), 0);
    p.objective_status = NULL;
    check_i("partial provider refused", geConsoleReadInstall(&p),
            GE_CONSOLE_STATUS_INVALID_DEFINITION);
    check_i("partial provider still registers nothing", geConsoleCommandCount(), 0);

    p = provider();
    check_i("read-only provider installs", geConsoleReadInstall(&p), GE_CONSOLE_STATUS_OK);
    check_i("eight initial commands registered", geConsoleCommandCount(), 8);
    check_i("player show metadata found",
            geConsoleCommandById(GE_CONSOLE_COMMAND_PLAYER_SHOW, &spec), 1);
    check_i("player show requires explicit player",
            (spec.flags & GE_CONSOLE_CMD_REQUIRES_PLAYER) != 0, 1);
    check_i("player show is read-only",
            (spec.flags & GE_CONSOLE_CMD_READ_ONLY) != 0, 1);
    check_i("player show has one argument", spec.argument_count, 1);
    check_i("player show argument is a slot", spec.arguments[0].type,
            GE_CONSOLE_ARG_PLAYER_SLOT);

    gePortConsoleGameTick();
    check_i("idle tick still reconciles pause", pause_calls, 1);
    check_i("idle tick does not sample stage", stage_calls, 0);
    check_i("idle tick does not walk player slots", player_calls, 0);
}

static void test_registry_commands(void)
{
    GeConsoleResult result;
    int found;

    result = run("help player show");
    check_i("help succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("help resolves stable command id",
            payload_integer(&result, GE_CONSOLE_FIELD_COMMAND_ID, &found),
            GE_CONSOLE_COMMAND_PLAYER_SHOW);
    check_i("help command id payload present", found, 1);
    check_has("help uses registered summary", result.message, "explicit player slot");

    result = run("help missing");
    check_i("unknown help target is a choice error", result.status,
            GE_CONSOLE_STATUS_ARGUMENT_CHOICE);
    check_i("unknown help target is warning", result.severity,
            GE_CONSOLE_SEVERITY_WARNING);

    result = run("cmds");
    check_i("commands alias succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("commands reports registry total",
            payload_integer(&result, GE_CONSOLE_FIELD_TOTAL, &found), 8);
    check_has("commands includes objective list", result.message, "objective list");

    result = run("version");
    check_i("build alias succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("build reports OpenGL test renderer",
            payload_integer(&result, GE_CONSOLE_FIELD_RENDERER, &found),
            GE_CONSOLE_RENDERER_OPENGL);
    check_has("build reports schema", result.message, "console_schema=1");
}

static void populate_players(void)
{
    fake_players[0].present = 1;
    fake_players[0].fields = GE_ST_POSITION;
    fake_players[0].x = 1.0f;
    fake_players[0].y = 2.0f;
    fake_players[0].z = 3.0f;

    fake_players[2].present = 1;
    fake_players[2].fields = GE_ST_POSITION | GE_ST_ROOM | GE_ST_ANGLE |
                             GE_ST_HEALTH | GE_ST_WEAPON | GE_ST_SCORE;
    fake_players[2].x = 12.25f;
    fake_players[2].y = -3.5f;
    fake_players[2].z = 99.75f;
    fake_players[2].room = 17;
    fake_players[2].angle = -45.5f;
    fake_players[2].health = 0.875f;
    fake_players[2].armour = 0.25f;
    fake_players[2].dead = 0;
    fake_players[2].weapon = 7;
    fake_players[2].ammo_clip = 6;
    fake_players[2].ammo_reserve = 30;
    fake_players[2].kills = 4;
    fake_players[2].deaths = 1;
    fake_players[2].shots = 40;
}

static void test_session_and_players(void)
{
    GeConsoleResult result;
    int found;

    populate_players();
    fake_netplay = 1;
    result = run("status");
    check_i("status succeeds during netplay", result.status, GE_CONSOLE_STATUS_OK);
    check_i("status target carries stage", result.stage_id, 34);
    check_i("status reports sparse explicit player mask",
            payload_integer(&result, GE_CONSOLE_FIELD_PLAYER_MASK, &found), 5);
    check_i("status reports netplay", result.payload[3].value.boolean, 1);
    check_has("status reports multiplayer honestly", result.message, "mode=multiplayer");
    check_has("status reports execution tick", result.message, "tick=700");

    result = run("players");
    check_i("player list succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("player list counts occupied slots",
            payload_integer(&result, GE_CONSOLE_FIELD_TOTAL, &found), 2);
    check_has("player list names sparse slots", result.message, "slots=0,2");

    result = run("player 2");
    check_i("player show alias succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("player show result resolves slot", result.player_slot, 2);
    check_i("player show carries target flag",
            (result.target_fields & GE_CONSOLE_TARGET_PLAYER) != 0, 1);
    check_i("player show reports field mask",
            payload_integer(&result, GE_CONSOLE_FIELD_PLAYER_FIELDS, &found),
            fake_players[2].fields);
    check_i("player show scales health deterministically",
            payload_integer(&result, GE_CONSOLE_FIELD_HEALTH_MILLI, &found), 875);
    check_has("player show reports weapon", result.message, "weapon=7 ammo=6/30");

    result = run("where 2");
    check_i("where succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("where scales x to hundredths",
            payload_integer(&result, GE_CONSOLE_FIELD_POSITION_X_CENTI, &found), 1225);
    check_i("where scales negative y to hundredths",
            payload_integer(&result, GE_CONSOLE_FIELD_POSITION_Y_CENTI, &found), -350);
    check_i("where scales z to hundredths",
            payload_integer(&result, GE_CONSOLE_FIELD_POSITION_Z_CENTI, &found), 9975);
    check_has("where reports room and heading", result.message, "room=17 heading=-45.50");

    result = run("where 1");
    check_i("empty explicit slot refused before handler", result.status,
            GE_CONSOLE_STATUS_REFUSED_PLAYER);
    check_i("empty slot remains the resolved target", result.player_slot, 1);

    fake_players[0].fields = GE_ST_HEALTH;
    result = run("where 0");
    check_i("absent position is an honest successful observation", result.status,
            GE_CONSOLE_STATUS_OK);
    check_i("absent position raises warning severity", result.severity,
            GE_CONSOLE_SEVERITY_WARNING);
    check_has("absent position is visible", result.message, "position unavailable");

    stage_calls = 0;
    result = run("where 4");
    check_i("out-of-range slot rejected by parser", result.status,
            GE_CONSOLE_STATUS_ARGUMENT_RANGE);
    check_i("parse refusal does not sample game context", stage_calls, 0);
}

static void test_objectives(void)
{
    GeConsoleResult result;
    int i, found;
    uint32_t expected_present = 0;
    uint32_t expected_status = 0;

    fake_stage = GE_CONSOLE_TITLE_STAGE;
    objective_count_calls = 0;
    result = run("objectives");
    check_i("title-screen objectives refused", result.status,
            GE_CONSOLE_STATUS_REFUSED_MISSION);
    check_i("mission refusal does not call objective provider", objective_count_calls, 0);

    fake_stage = -1;
    result = run("objectives");
    check_i("unavailable stage is not treated as an active mission", result.status,
            GE_CONSOLE_STATUS_REFUSED_MISSION);
    check_i("unavailable stage does not call objective provider", objective_count_calls, 0);

    fake_stage = 34;
    fake_objective_total = 12;
    for (i = 0; i < 12; i++) {
        fake_objective_present[i] = i != 5;
        fake_objective_statuses[i] = i % 3;
    }
    objective_status_calls = 0;
    objective_max_index = -1;
    result = run("objective list");
    check_i("objective list succeeds", result.status, GE_CONSOLE_STATUS_OK);
    check_i("objective list exposes truncation as warning", result.severity,
            GE_CONSOLE_SEVERITY_WARNING);
    check_i("objective list reports full total",
            payload_integer(&result, GE_CONSOLE_FIELD_TOTAL, &found), 12);
    check_i("objective list captures fixed capacity",
            payload_integer(&result, GE_CONSOLE_FIELD_CAPTURED, &found),
            GE_CONSOLE_OBJECTIVE_CAPACITY);
    check_i("objective provider called only to capacity", objective_status_calls,
            GE_CONSOLE_OBJECTIVE_CAPACITY);
    check_i("objective provider never receives index past capacity", objective_max_index, 9);
    check_has("objective truncation is visible", result.message, "showing 10/12");
    check_has("missing objective status is visible", result.message, "5=unavailable");
    for (i = 0; i < GE_CONSOLE_OBJECTIVE_CAPACITY; i++) {
        unsigned int encoded = i == 5 ? 3u : (unsigned int)(i % 3);
        if (i != 5) { expected_present |= 1u << (unsigned int)i; }
        expected_status |= encoded << ((unsigned int)i * 2u);
    }
    check_i("objective presence is structured",
            payload_integer(&result, GE_CONSOLE_FIELD_OBJECTIVE_PRESENT, &found),
            expected_present);
    check_i("objective statuses are bounded structured data",
            payload_integer(&result, GE_CONSOLE_FIELD_OBJECTIVE_STATUS, &found),
            expected_status);

    fake_objective_total = 1000;
    objective_status_calls = 0;
    objective_max_index = -1;
    result = run("objectives");
    check_i("corrupt large count remains bounded", objective_status_calls,
            GE_CONSOLE_OBJECTIVE_CAPACITY);
    check_i("corrupt count is reported, not clamped silently",
            payload_integer(&result, GE_CONSOLE_FIELD_TOTAL, &found), 1000);

    fake_objective_total = -1;
    result = run("objectives");
    check_i("unavailable objective provider is not a fabricated failure", result.status,
            GE_CONSOLE_STATUS_OK);
    check_i("unavailable objective provider warns", result.severity,
            GE_CONSOLE_SEVERITY_WARNING);
    check_has("unavailable objective state is explicit", result.message,
              "objectives unavailable");
}

int main(void)
{
    printf("test_console_commands\n");
    reset_fakes();
    test_install_and_idle();
    test_registry_commands();
    test_session_and_players();
    test_objectives();
    return failures ? 1 : 0;
}
