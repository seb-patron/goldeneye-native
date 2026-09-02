/* Developer-console keyboard/mouse ownership, without SDL, a renderer, or a ROM. */
#include <stdio.h>

#include "ge_console_input.c"

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: got %d want %d\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    printf("test_console_input\n");

    geConsoleInputReset();
    check("reset is closed", geConsoleInputOpen(), 0);
    check("reset does not capture", geConsoleInputCaptureActive(), 0);
    check("gameplay starts enabled", geConsoleInputGameplayAllowed(1, 1), 1);

    geConsoleInputSetOpen(1);
    check("open state is visible", geConsoleInputOpen(), 1);
    check("open captures events", geConsoleInputCaptureActive(), 1);
    check("open blocks an idle poll too", geConsoleInputGameplayAllowed(0, 0), 0);
    check("open blocks keyboard", geConsoleInputGameplayAllowed(1, 0), 0);
    check("open blocks mouse", geConsoleInputGameplayAllowed(0, 1), 0);

    geConsoleInputSetOpen(0);
    check("close state is visible", geConsoleInputOpen(), 0);
    check("close starts release quarantine", geConsoleInputCaptureActive(), 1);
    check("held close key remains blocked", geConsoleInputGameplayAllowed(1, 0), 0);
    check("held mouse button also blocks release", geConsoleInputGameplayAllowed(0, 1), 0);
    check("all released ends quarantine", geConsoleInputGameplayAllowed(0, 0), 1);
    check("capture ends after release", geConsoleInputCaptureActive(), 0);
    check("later keys reach gameplay normally", geConsoleInputGameplayAllowed(1, 0), 1);

    /* Reopening during quarantine must own the device again, not inherit a half-released
     * closed state. */
    geConsoleInputSetOpen(1);
    geConsoleInputSetOpen(0);
    check("second close quarantines", geConsoleInputGameplayAllowed(1, 1), 0);
    geConsoleInputSetOpen(1);
    check("reopen remains blocking", geConsoleInputGameplayAllowed(0, 0), 0);
    geConsoleInputReset();
    check("shutdown reset clears capture", geConsoleInputCaptureActive(), 0);

    return failures ? 1 : 0;
}
