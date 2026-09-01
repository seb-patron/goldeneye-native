#include "ge_console_input.h"

static int ge_console_open;
static int ge_console_release_quarantine;

void geConsoleInputReset(void)
{
    ge_console_open = 0;
    ge_console_release_quarantine = 0;
}

void geConsoleInputSetOpen(int open)
{
    open = open ? 1 : 0;
    if (open == ge_console_open) return;

    ge_console_open = open;
    /* Opening owns input immediately.  Closing retains ownership until the physical devices
     * are idle, so a held gameplay key cannot become a new edge when polling resumes. */
    ge_console_release_quarantine = open ? 0 : 1;
}

int geConsoleInputOpen(void)
{
    return ge_console_open;
}

int geConsoleInputCaptureActive(void)
{
    return ge_console_open || ge_console_release_quarantine;
}

int geConsoleInputGameplayAllowed(int any_keyboard_key, int any_mouse_button)
{
    if (ge_console_open) return 0;
    if (!ge_console_release_quarantine) return 1;

    if (any_keyboard_key || any_mouse_button) return 0;
    ge_console_release_quarantine = 0;
    return 1;
}
