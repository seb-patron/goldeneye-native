/* Renderer-independent developer-console pause policy.  No ROM, game, window, SDL or ImGui. */
#include <stdio.h>
#include <string.h>

static int fake_stage;
static int fake_players;
static int fake_netplay;
int bossGetStageNum(void) { return fake_stage; }
int getPlayerCount(void) { return fake_players; }
int gePortNetActive(void) { return fake_netplay; }

#include "ge_console_pause.c"

static int checks;
static int failures;

static void check_i(const char *what, int got, int want)
{
    checks++;
    if (got == want) {
        printf("  ok    %-58s %d\n", what, got);
    } else {
        printf("  FAIL  %-58s got %d want %d\n", what, got, want);
        failures++;
    }
}

static void check_s(const char *what, const char *got, const char *want)
{
    checks++;
    if (got != NULL && strcmp(got, want) == 0) {
        printf("  ok    %-58s %s\n", what, got);
    } else {
        printf("  FAIL  %-58s got %s want %s\n", what, got ? got : "(null)", want);
        failures++;
    }
}

static void set_context(int stage, int players, int netplay)
{
    fake_stage = stage;
    fake_players = players;
    fake_netplay = netplay;
    gePortConsolePauseGameTick();
}

/* Mirrors lv.c's three independently owned clock-stop reasons.  The important property is
 * composition: the console module never receives a setter for either original game reason. */
static int clock_stopped(int controls_locked, int watch_paused)
{
    return controls_locked || watch_paused || gePortConsolePauseActive();
}

int main(void)
{
    int preexisting_watch_pause = 1;

    printf("developer console pause policy\n\n");
    geConsolePauseReset();
    check_i("reset leaves console request closed", geConsolePauseRequested(), 0);
    check_i("reset owns no pause reason", gePortConsolePauseActive(), 0);
    check_i("reset state is closed", geConsolePauseState(), GE_CONSOLE_PAUSE_CLOSED);

    geConsolePauseRequest(1);
    check_i("renderer request is recorded", geConsolePauseRequested(), 1);
    check_i("renderer request cannot pause before game tick", gePortConsolePauseActive(), 0);
    set_context(34, 1, 0);
    check_i("solo mission game tick owns pause reason", gePortConsolePauseActive(), 1);
    check_i("developer reason stops an otherwise-live clock", clock_stopped(0, 0), 1);
    check_i("solo mission state is explicit", geConsolePauseState(),
            GE_CONSOLE_PAUSE_SOLO_OWNED);
    check_s("solo mission status names developer ownership",
            geConsolePauseStateName(geConsolePauseState()),
            "solo paused (developer-owned)");

    set_context(34, 1, 0);
    check_i("repeated game tick is idempotent", gePortConsolePauseActive(), 1);
    geConsolePauseRequest(1);
    set_context(34, 1, 0);
    check_i("repeated open request is idempotent", gePortConsolePauseActive(), 1);

    geConsolePauseRequest(0);
    check_i("close request waits for game-thread reconciliation", gePortConsolePauseActive(), 1);
    set_context(34, 1, 0);
    check_i("close releases only developer-owned reason", gePortConsolePauseActive(), 0);
    check_i("pre-existing watch pause remains untouched", preexisting_watch_pause, 1);
    check_i("pre-existing watch pause still stops clock after close",
            clock_stopped(0, preexisting_watch_pause), 1);
    preexisting_watch_pause = 0;
    check_i("clock resumes once its remaining owner releases", clock_stopped(0, 0), 0);
    check_i("controls lock also remains independently owned", clock_stopped(1, 0), 1);
    check_i("close state is explicit", geConsolePauseState(), GE_CONSOLE_PAUSE_CLOSED);

    geConsolePauseRequest(1);
    set_context(34, 2, 0);
    check_i("local multiplayer remains live", gePortConsolePauseActive(), 0);
    check_i("multiplayer refusal reason is explicit", geConsolePauseState(),
            GE_CONSOLE_PAUSE_MULTIPLAYER);

    set_context(34, 1, 1);
    check_i("netplay remains live", gePortConsolePauseActive(), 0);
    check_i("netplay wins over otherwise-solo context", geConsolePauseState(),
            GE_CONSOLE_PAUSE_NETPLAY);

    set_context(90, 1, 0);
    check_i("title screen remains live", gePortConsolePauseActive(), 0);
    check_i("title screen has no-mission reason", geConsolePauseState(),
            GE_CONSOLE_PAUSE_NO_MISSION);
    set_context(34, 0, 0);
    check_i("missing player remains live", gePortConsolePauseActive(), 0);
    set_context(34, -1, 0);
    check_i("invalid player count remains live", gePortConsolePauseActive(), 0);

    set_context(34, 1, 0);
    check_i("valid solo context can reacquire reason", gePortConsolePauseActive(), 1);
    set_context(34, 1, 1);
    check_i("context change to netplay releases reason", gePortConsolePauseActive(), 0);
    set_context(34, 3, 0);
    check_i("context change to multiplayer stays live", gePortConsolePauseActive(), 0);

    printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASSED",
           checks, failures);
    return failures ? 1 : 0;
}
