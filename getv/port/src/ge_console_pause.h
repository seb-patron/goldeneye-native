/* Game-thread-owned pause policy for the native developer console.
 *
 * The renderer may only request that the console is open or closed.  The admitted simulation
 * tick supplies authoritative game context and owns the active pause reason.  This reason is
 * deliberately independent of GoldenEye's watch/menu pause and controls lock: lv.c ORs it into
 * the clock-stop decision, so releasing it cannot clear a pause that another subsystem owns.
 */
#ifndef GE_CONSOLE_PAUSE_H
#define GE_CONSOLE_PAUSE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GeConsolePauseState {
    GE_CONSOLE_PAUSE_CLOSED = 0,
    GE_CONSOLE_PAUSE_SOLO_OWNED,
    GE_CONSOLE_PAUSE_NO_MISSION,
    GE_CONSOLE_PAUSE_MULTIPLAYER,
    GE_CONSOLE_PAUSE_NETPLAY
} GeConsolePauseState;

typedef struct GeConsolePauseContext {
    int mission_active;
    int player_count;
    int netplay_active;
} GeConsolePauseContext;

/* Pure policy/lifecycle surface.  Request records renderer intent only; Update is the sole
 * transition into or out of the developer-owned active reason and must run on the game thread. */
void geConsolePauseReset(void);
void geConsolePauseRequest(int open);
int geConsolePauseUpdate(const GeConsolePauseContext *context);

int geConsolePauseRequested(void);
int gePortConsolePauseActive(void);
GeConsolePauseState geConsolePauseState(void);
const char *geConsolePauseStateName(GeConsolePauseState state);

/* Production adapter, called from gePortConsoleGameTick() at the established admitted-tick
 * boundary.  It samples mission/player/netplay state and applies the pure policy above. */
void gePortConsolePauseGameTick(void);

#ifdef __cplusplus
}
#endif
#endif /* GE_CONSOLE_PAUSE_H */
