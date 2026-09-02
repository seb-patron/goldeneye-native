#include <stdio.h>

#include "ge_console_pause.h"

/* The title stage is not a mission.  Keep the constant local to the port adapter instead of
 * importing the decomp's broad game headers into this renderer-independent policy module. */
#define GE_CONSOLE_TITLE_STAGE 90

static struct {
    int requested_open;
    int active;
    GeConsolePauseState state;
} ge_console_pause;

void geConsolePauseReset(void)
{
    ge_console_pause.requested_open = 0;
    ge_console_pause.active = 0;
    ge_console_pause.state = GE_CONSOLE_PAUSE_CLOSED;
}

void geConsolePauseRequest(int open)
{
    ge_console_pause.requested_open = open ? 1 : 0;
}

int geConsolePauseUpdate(const GeConsolePauseContext *context)
{
    GeConsolePauseState next;
    int was_active = ge_console_pause.active;
    GeConsolePauseState was_state = ge_console_pause.state;

    if (!ge_console_pause.requested_open) {
        next = GE_CONSOLE_PAUSE_CLOSED;
    } else if (context != NULL && context->netplay_active) {
        next = GE_CONSOLE_PAUSE_NETPLAY;
    } else if (context == NULL || !context->mission_active || context->player_count <= 0) {
        next = GE_CONSOLE_PAUSE_NO_MISSION;
    } else if (context->player_count != 1) {
        next = GE_CONSOLE_PAUSE_MULTIPLAYER;
    } else {
        next = GE_CONSOLE_PAUSE_SOLO_OWNED;
    }

    ge_console_pause.state = next;
    ge_console_pause.active = next == GE_CONSOLE_PAUSE_SOLO_OWNED;
    return was_active != ge_console_pause.active || was_state != ge_console_pause.state;
}

int geConsolePauseRequested(void)
{
    return ge_console_pause.requested_open;
}

int gePortConsolePauseActive(void)
{
    return ge_console_pause.active;
}

GeConsolePauseState geConsolePauseState(void)
{
    return ge_console_pause.state;
}

const char *geConsolePauseStateName(GeConsolePauseState state)
{
    switch (state) {
    case GE_CONSOLE_PAUSE_CLOSED:       return "closed";
    case GE_CONSOLE_PAUSE_SOLO_OWNED:   return "solo paused (developer-owned)";
    case GE_CONSOLE_PAUSE_NO_MISSION:   return "no mission: live";
    case GE_CONSOLE_PAUSE_MULTIPLAYER:  return "multiplayer: live";
    case GE_CONSOLE_PAUSE_NETPLAY:      return "netplay: live";
    default:                            return "unknown";
    }
}

void gePortConsolePauseGameTick(void)
{
    extern int bossGetStageNum(void);
    extern int getPlayerCount(void);
    extern int gePortNetActive(void);
    GeConsolePauseContext context;
    int stage = bossGetStageNum();

    context.player_count = getPlayerCount();
    context.mission_active = stage != GE_CONSOLE_TITLE_STAGE && context.player_count > 0;
    context.netplay_active = gePortNetActive();

    if (geConsolePauseUpdate(&context)) {
        printf("[getv][console] pause policy: %s\n",
               geConsolePauseStateName(geConsolePauseState()));
        fflush(stdout);
    }
}
