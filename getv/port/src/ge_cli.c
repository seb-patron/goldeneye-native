/* Play GoldenEye from a terminal, through the API alone.
 *
 * Why this IS the real test OF the API
 *
 * A bot that walks into a wall tells you very little: the policy might be poor, or the policy
 * might be fine and unable to perceive the wall. Those need different fixes and the bot cannot
 * tell you which. Put a PERSON at the same interface and the ambiguity disappears -- if someone
 * can read the situation report and play the level, the API is complete and the bot is wiring;
 * if they cannot, whatever they are missing is exactly what the bot is missing too.
 *
 * So this exposes everything a bot sees and nothing it does not: position, heading, room, health,
 * weapon, what is ahead and how far, the clearest way out, doors and keys and objectives with
 * bearings, and every enemy with range, bearing and whether it can see you. No screen.
 *
 *   GETV_CLI=1              turn it on; reads commands from stdin
 *   GETV_CLI_SLOT=<n>       which player, default 0
 *   GETV_CLI_EVERY=<n>      frames between reports, default 60
 *
 * COMMANDS   w <n>  walk n ticks        a/d <n>  turn left/right n ticks
 *            s <n>  back                use      open a door / act
 *            fire   shoot               look     report now
 *            stop   neutral             quit     end the run
 *
 * stdin is read NON-BLOCKING. Blocking would stall the game loop, and a frozen game with a
 * prompt looks exactly like a crash -- which is how the first version presented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#else
/* Declared locally rather than #include <windows.h>, matching port_save.c's MoveFileExA --
 * windows.h's macros collide with names elsewhere in the port layer, and the three functions
 * this needs are a small, stable, unchanging slice of kernel32. */
#define GE_CLI_STD_INPUT_HANDLE ((unsigned long) -10)
#define GE_CLI_FILE_TYPE_PIPE   3
__declspec(dllimport) void  *__stdcall GetStdHandle(unsigned long);
__declspec(dllimport) unsigned long __stdcall GetFileType(void *);
__declspec(dllimport) int   __stdcall PeekNamedPipe(void *, void *, unsigned long, unsigned long *,
                                                    unsigned long *, unsigned long *);
__declspec(dllimport) int   __stdcall ReadFile(void *, void *, unsigned long, unsigned long *, void *);
#include <conio.h>
#endif

#include "ge_player_api.h"
#include "ge_world_api.h"
#include "ge_sense_api.h"
#include "ge_enemy_api.h"

#define GE_CLI_WALK 60

static int   ge_cli_on = -1;
static int   ge_cli_slot;
static int   ge_cli_every;
#ifdef _WIN32
static int   ge_cli_win_is_pipe;   /* resolved once in ge_cli_setup; see the note there */
#endif
static int   ge_cli_hold;          /* ticks left on the current command */
static int   ge_cli_sx, ge_cli_sy;
static unsigned int ge_cli_buttons;
static int   ge_cli_report_now;
static int   ge_cli_map_now;
static float ge_cli_map_cell;
static int   ge_cli_path_now;
static float ge_cli_path_cell;

static float ge_cli_norm180(float a)
{
    while (a > 180.0f)  { a -= 360.0f; }
    while (a < -180.0f) { a += 360.0f; }
    return a;
}

/* Bearing RELATIVE to where the player is facing, which is the only form a person can act on.
 * An absolute bearing forces the reader to do the subtraction the game already knows. */
static float ge_cli_rel(float from_x, float from_z, float to_x, float to_z, float facing)
{
    float b = (float) (atan2((double) (to_x - from_x), (double) (to_z - from_z))
                       * 180.0 / 3.14159265358979);
    return ge_cli_norm180(b - facing);
}

static void ge_cli_setup(void)
{
    const char *on = getenv("GETV_CLI");
    const char *slot = getenv("GETV_CLI_SLOT");
    const char *every = getenv("GETV_CLI_EVERY");

    ge_cli_on = (on != NULL && *on != '\0' && *on != '0');
    ge_cli_slot = (slot != NULL && *slot != '\0') ? atoi(slot) : 0;
    ge_cli_every = (every != NULL && *every != '\0') ? atoi(every) : 60;
    if (ge_cli_every < 1) { ge_cli_every = 1; }
    if (!ge_cli_on) { return; }

#ifndef _WIN32
    {
        int fl = fcntl(0, F_GETFL, 0);
        if (fl != -1) { fcntl(0, F_SETFL, fl | O_NONBLOCK); }
    }
#else
    /* Resolved once rather than on every poll, matching how ge_cli_on/slot/every are cached
     * above: GetFileType is a real kernel call, not a local variable read, and this runs once a
     * frame for the life of the process. play_cli.py's redirected pipe is FILE_TYPE_PIPE, so
     * PeekNamedPipe + ReadFile applies; a human running the .exe directly in a terminal has a
     * console handle instead, where PeekNamedPipe always fails, so _kbhit/_getch is used there.
     * Both paths existed already in spirit -- POSIX read() with O_NONBLOCK works the same way
     * against a real terminal or a pipe without the caller needing to know which; Windows has no
     * single call that does both, so the two are picked apart once here instead. */
    ge_cli_win_is_pipe = (GetFileType(GetStdHandle(GE_CLI_STD_INPUT_HANDLE))
                          == GE_CLI_FILE_TYPE_PIPE);
#endif
    gePlayerClaim(ge_cli_slot, GE_SLOT_INJECTED);
    printf("[cli] playing slot %d. commands: w/s/a/d <ticks>, use, fire, look, map, stop, quit\n",
           ge_cli_slot);
    fflush(stdout);
}

static void ge_cli_command(const char *line)
{
    char verb[32];
    int n = 0;
    int given;

    /* Whether a number was TYPED, not just what it ended up as. The default of 30 is a sensible
     * tick count for a movement command and a bad cell size for a search, and conflating "no
     * argument" with "30" made a bare "path" search a third of its intended range -- which
     * looked exactly like the pathfinder getting worse. */
    given = sscanf(line, "%31s %d", verb, &n);
    if (given < 1) { return; }
    given = (given >= 2 && n > 0);
    if (n <= 0) { n = 30; }

    ge_cli_sx = ge_cli_sy = 0;
    ge_cli_buttons = 0;
    ge_cli_hold = n;

    if      (strcmp(verb, "w") == 0)    { ge_cli_sy =  GE_CLI_WALK; }
    else if (strcmp(verb, "s") == 0)    { ge_cli_sy = -GE_CLI_WALK; }
    else if (strcmp(verb, "a") == 0)    { ge_cli_sx = -80; }
    else if (strcmp(verb, "d") == 0)    { ge_cli_sx =  80; }
    else if (strcmp(verb, "use") == 0)  { ge_cli_buttons = GE_IN_USE; ge_cli_sy = GE_CLI_WALK; }
    else if (strcmp(verb, "fire") == 0) { ge_cli_buttons = GE_IN_FIRE; ge_cli_hold = 6; }
    else if (strcmp(verb, "look") == 0) { ge_cli_hold = 0; ge_cli_report_now = 1; }
    else if (strcmp(verb, "map") == 0)  { ge_cli_hold = 0; ge_cli_map_now = 1;
                                          ge_cli_report_now = 1;
                                          ge_cli_map_cell = given ? (float) n : 0.0f; }
    else if (strcmp(verb, "path") == 0) { ge_cli_hold = 0; ge_cli_path_now = 1;
                                          ge_cli_report_now = 1;
                                          ge_cli_path_cell = given ? (float) n : 0.0f; }
    else if (strcmp(verb, "stop") == 0) { ge_cli_hold = 0; }
    else if (strcmp(verb, "quit") == 0) { printf("[cli] bye\n"); fflush(stdout); exit(0); }
    else {
        /* Say so rather than silently doing nothing: a typo that reads as a stuck game is the
         * worst outcome for an interface whose whole job is being legible. */
        printf("[cli] ? %s\n", verb);
        ge_cli_hold = 0;
    }
    fflush(stdout);
}

/* Where we are trying to get to, if anywhere.
 *
 * The objective line already computes this; the detour probe needs the same point, and two
 * places deriving "the objective" separately is how they end up disagreeing. */
static int ge_cli_objective_point(float *out_x, float *out_z)
{
    GeWorldObjective ob;

    if (!geWorldObjective(0, &ob)) { return 0; }
    if (out_x != NULL) { *out_x = ob.tx; }
    if (out_z != NULL) { *out_z = ob.tz; }
    return 1;
}


/* The floor around you, as a picture.
 *
 * Every other line in this report is a bearing and a range, which tells you about one thing at a
 * time. None of them answers "which way can I actually go", and that is the question a player
 * stuck against a wall is really asking. A body either fits somewhere or it does not, and the
 * engine will answer that for any point, so ask it for a few hundred points and draw the answer.
 *
 * North is up, matching the compass line: -z is up, +x is right. The cells are 60 units, about a
 * third of a metre, which is fine enough to show a doorway and coarse enough to fit on a screen.
 */
static void ge_cli_print_map(const GePlayerState *st, float cell)
{
    extern int gePortCanStandAt(float x, float z);
    extern int gePortTileAt(float x, float z, int *out_id, int *out_room);
    const int half = 10;

    /* A cell is a body-ish 60 units by default, fine enough to show a doorway. Pass a bigger one
     * to see a whole carriage at once: the answer to "how do I get round this" is often outside
     * the twelve metres a body-scale map covers. */
    if (cell < 10.0f) { cell = 60.0f; }
    int gz, gx;

    printf("map    %d cells of %.0f units, north up, @ is you\n", half * 2 + 1, (double) cell);
    for (gz = -half; gz <= half; gz++) {
        printf("       ");
        for (gx = -half; gx <= half; gx++) {
            float x = st->x + (float) gx * cell;
            float z = st->z + (float) gz * cell;
            char c;

            if (gx == 0 && gz == 0)            { c = '@'; }
            else if (gePortCanStandAt(x, z))   { c = '.'; }
            else if (gePortTileAt(x, z, NULL, NULL)) { c = ':'; }   /* floor, but no room to stand */
            else                               { c = '#'; }
            putchar(c);
        }
        putchar('\n');
    }
    printf("       . you fit   : floor but too tight   # no floor\n");
}


static void ge_cli_poll_stdin(void)
{
    static char buf[256];
    static int len;
#ifndef _WIN32
    char c;

    while (read(0, &c, 1) == 1) {
        if (c == '\n') {
            buf[len] = '\0';
            if (len > 0) { ge_cli_command(buf); }
            len = 0;
        } else if (len < (int) sizeof buf - 1) {
            buf[len++] = c;
        }
    }
#else
    /* this was the whole reason GETV_CLI did nothing ON Windows: the function existed, was
     * called every frame (once gePortCliFrame itself is wired in -- see the note at its call
     * site), but its body was `#ifndef _WIN32` around empty, so no byte typed or piped ever
     * reached ge_cli_command. quit never fired, and neither did anything else -- confirmed by
     * driving it from play_cli.py and finding the child process alive, producing no report
     * lines, and never exiting even after "quit" was written and flushed to its stdin pipe.
     *
     * Two Windows sources need two different calls, because there is no single Win32 API that
     * behaves like POSIX non-blocking read() against both a pipe and a real console. Which one
     * applies is resolved once in ge_cli_setup and cached in ge_cli_win_is_pipe, not re-detected
     * every frame -- GetFileType is a real kernel call, and this runs once per rendered frame for
     * the life of the process. */
    if (ge_cli_win_is_pipe) {
        /* play_cli.py's case: a redirected pipe, exactly like a script piping into any other CLI
         * tool. PeekNamedPipe asks how many bytes are waiting WITHOUT consuming them and without
         * blocking if there are none -- that non-blocking check is the whole point, matching what
         * O_NONBLOCK gives the POSIX side for free. Only then does ReadFile consume one byte,
         * which cannot block either because PeekNamedPipe already proved a byte is there. */
        void *h = GetStdHandle(GE_CLI_STD_INPUT_HANDLE);
        unsigned long avail = 0;
        while (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            char c;
            unsigned long got = 0;
            if (!ReadFile(h, &c, 1, &got, NULL) || got != 1) { break; }
            if (c == '\n') {
                buf[len] = '\0';
                if (len > 0) { ge_cli_command(buf); }
                len = 0;
            } else if (c != '\r' && len < (int) sizeof buf - 1) {
                /* '\r' dropped here rather than left in the buffer: Windows text-mode pipes and a
                 * person's terminal both commonly send CRLF, and the POSIX side never sees a
                 * stray '\r' because pipes there are binary by default -- without this, "quit"
                 * arrives as "quit\r", sscanf's %s still reads "quit\r" as one token, and it
                 * matches none of the known verbs. */
                buf[len++] = c;
            }
        }
    } else {
        /* A human running the .exe directly in a terminal: PeekNamedPipe always fails against a
         * real console handle, so this is the conio.h pair that DOES work against one. _kbhit
         * is the non-blocking check; _getch reads one key without echoing it or waiting for
         * Enter, so it is called once per key exactly like the pipe branch above, not once per
         * line -- echoing and this file's own newline-triggered dispatch stay identical either
         * way, so a person typing behaves the same as play_cli.py sending one line at a time. */
        while (_kbhit()) {
            int c = _getch();
            if (c == '\r' || c == '\n') {
                putchar('\n');
                buf[len] = '\0';
                if (len > 0) { ge_cli_command(buf); }
                len = 0;
            } else if (len < (int) sizeof buf - 1) {
                putchar(c);          /* conio does not echo; a typed command should be visible */
                buf[len++] = (char) c;
            }
        }
        fflush(stdout);
    }
#endif
}

static void ge_cli_report(int frame)
{
    GePlayerState st;
    GeSenseContact ahead;
    GeWorldProp pr;
    int i, n, shown = 0;

    if (!gePlayerStateGet(ge_cli_slot, &st) || !st.present) { return; }

    printf("\n--- f%d ---\n", frame);
    {
        /* The level's own terms. A coordinate says where the player is; this says whether that
         * is progress. See ge_places.c. */
        extern int gePortPlaceName(const char *level, float x, float z, char *out, int n);
        const char *lv = getenv("GETV_BOT_ROUTE_LEVEL");
        char place[64];
        if (lv != NULL && gePortPlaceName(lv, st.x, st.z, place, (int) sizeof place)) {
            printf("place  %s\n", place);
        }
    }
    /* where IN the map, not just what is in front.
     *
     * Every other line here is relative to facing, which is the right form for acting on one
     * thing and the wrong form for holding a picture. A reader given only bearings has to
     * re-derive the world every time it turns, and cannot form a belief like "the objective is
     * at the west end and I have been working west" -- which is the belief that would stop the
     * player doubling back, as it currently does.
     *
     * Cardinals are a convenience OVER the axes, not a second coordinate system: north is -z,
     * east is +x, stated once here so nothing downstream has to infer it and disagree. */
    {
        static const char *card[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
        float a = st.angle;
        int idx;

        while (a < 0.0f)      { a += 360.0f; }
        while (a >= 360.0f)   { a -= 360.0f; }
        idx = ((int) ((a + 22.5f) / 45.0f)) & 7;

        printf("compass facing %s (%.0f deg)   north is -z, east is +x\n", card[idx], (double) a);
    }

    {
        /* THE TILE YOU ARE ON, AND THE ONE YOU ARE WALKING INTO.
         *
         * "wall 96 away" is the same sentence at every wall in the game. The floor is a mesh of
         * numbered tiles and the engine knows which one is under you, so say it: a repeated tile
         * number is the plainest possible statement that you are stuck, and a tile number ahead
         * names the specific wall rather than the fact of one. */
        extern int gePortTileAt(float x, float z, int *out_id, int *out_room);
        int here = -1, here_room = -1, there = -1, there_room = -1;
        float ra = (float) (st.angle * 3.14159265358979 / 180.0);
        float fx = st.x + (float) sin((double) ra) * 150.0f;
        float fz = st.z + (float) cos((double) ra) * 150.0f;

        if (gePortTileAt(st.x, st.z, &here, &here_room)) {
            printf("tile   standing on %d (room %d)", here, here_room);
            if (gePortTileAt(fx, fz, &there, &there_room) && there != here) {
                printf(", facing %d (room %d)", there, there_room);
            } else if (there == here) {
                printf(", same tile ahead");
            } else {
                printf(", NO FLOOR ahead");
            }
            printf("\n");
        }
    }
    printf("you    (%.0f %.0f %.0f) facing %.0f  room %d  hp %.0f%%  weapon %d  ammo %d/%d\n",
           (double) st.x, (double) st.y, (double) st.z, (double) st.angle, st.room,
           (double) (st.health * 100.0f), st.weapon, st.ammo_clip, st.ammo_reserve);

    if (st.fields & GE_ST_ANGLE) {
        char what[48];
        /* BODY WIDTH, not a point ray. A single line fits through a gap narrower than the player
         * and reports clear, which is how the CLI player walked confidently into the corner of a
         * crate the report said it could pass. Three parallel lines a body-radius apart, nearest
         * obstruction wins, because a body stops at whichever shoulder meets something first. */
        geSenseAheadForBody(st.x, st.z, st.angle, 400.0f, &ahead);
        what[0] = '\0';
        if (ahead.what == GE_SENSE_CLEAR) { strcpy(what, "clear"); }
        else {
            if (ahead.what & GE_SENSE_WALL)   { strcat(what, "wall "); }
            if (ahead.what & GE_SENSE_DOOR)   { strcat(what, "door "); }
            if (ahead.what & GE_SENSE_OBJECT) { strcat(what, "object "); }
        }
        {
            /* Clearest heading judged with the SAME body probe the report uses. Asking a
             * point-ray sweep for the way out while reporting body-width obstruction sends the
             * reader down gaps the report has just said are blocked -- two different questions
             * answered as though they were one. */
            static const float sweep[9] = { 0.0f, 20.0f, 40.0f, 60.0f, 90.0f,
                                            120.0f, 145.0f, 165.0f, 180.0f };
            float best_turn = 0.0f, best_room = -1.0f;
            int k, sgn;

            for (k = 0; k < 9; k++) {
                for (sgn = 1; sgn >= -1; sgn -= 2) {
                    GeSenseContact c2;
                    float h = st.angle + (sweep[k] * (float) sgn);
                    geSenseAheadForBody(st.x, st.z, h, 400.0f, &c2);
                    if ((c2.what & GE_SENSE_SOLID) == 0) {
                        best_turn = sweep[k] * (float) sgn;
                        best_room = 400.0f;
                        k = 9;
                        break;
                    }
                    /* Nothing fully clear yet: remember the roomiest, so a boxed-in reader is
                     * told the least-bad way rather than "no way out". */
                    if (c2.distance > best_room) {
                        best_room = c2.distance;
                        best_turn = sweep[k] * (float) sgn;
                    }
                    if (sweep[k] == 0.0f) { break; }
                }
            }
            printf("ahead  %-16s %4.0f away   clearest turn %+.0f (%.0f room)\n",
                   what, (double) ahead.distance, (double) best_turn, (double) best_room);

            /* AND THE WAY PAST IT, as a sideways step rather than a new heading.
             *
             * "clearest turn" answers which way is open, which is not the same question as how
             * to get where you were going. On Train the carriage is blocked by a row of crates
             * with a gap between them: the clearest turn points at the open half of the room,
             * and following it walks you into the corner of the room instead of through the
             * gap. A step of sixty units to one side threads it and leaves you still pointed
             * down the carriage.
             *
             * Probed along the route rather than along the facing, because the question is how
             * to reach the objective, not what happens to be in front of the body.
             */
            if (ahead.distance < 500.0f) {
                extern float gePortLaneOffset(float px, float pz, float *tx, float *tz);
                float ob_x, ob_z, ax, az, off;

                if (ge_cli_objective_point(&ob_x, &ob_z)) {
                    float ddx = ob_x - st.x, ddz = ob_z - st.z;
                    float dl = (float) sqrt((double) (ddx * ddx + ddz * ddz));
                    if (dl > 1.0f) {
                        ax = st.x + ddx / dl * 600.0f;
                        az = st.z + ddz / dl * 600.0f;
                        off = gePortLaneOffset(st.x, st.z, &ax, &az);
                        if (off != 0.0f) {
                            printf("detour step %.0f to the %s, then straight on\n",
                                   (double) (off < 0.0f ? -off : off),
                                   (off > 0.0f) ? "left" : "right");
                        } else {
                            printf("detour none within 240 units either side\n");
                        }
                    }
                }
            }
        }
    }

    /* Landmarks, as bearings a person can steer by. */
    {
        static const int kinds[3] = { GE_PROP_DOOR, GE_PROP_KEY, GE_PROP_COLLECTABLE };
        unsigned int k;
        for (k = 0; k < 3; k++) {
            if (!geWorldNearestProp(kinds[k], st.x, st.y, st.z, &pr)) { continue; }
            /* Named, not just located. "Door 4935 away" is a fact about geometry; "door #212"
             * is a fact about a specific door, which is what lets a reader notice it is the same
             * one it failed to open a minute ago. */
            printf("%-6s tag %-4d %4.0f away, turn %+.0f, room %d\n",
                   geWorldPropKindName(kinds[k]), pr.tag,
                   (double) sqrt((double) (((pr.x - st.x) * (pr.x - st.x))
                                         + ((pr.z - st.z) * (pr.z - st.z)))),
                   (double) ge_cli_rel(st.x, st.z, pr.x, pr.z, st.angle), pr.room);
        }
    }

    /* EVERYTHING NEARBY, not just the landmarks worth walking to.
     *
     * The nearest-of-each-kind lines answer "where should I go". They do not answer "what am I
     * about to walk into", and those are different questions: a crate two metres ahead never
     * appears as the nearest collectable, so a player steering by landmarks walks straight into
     * furniture the level knowledge knew about all along.
     *
     * So: every prop within reach, whatever its kind, with range and relative bearing. Sorted by
     * distance because the near ones are the ones that decide the next step. The level already
     * carries all 4,871 of them; withholding the ones with no navigational purpose was the
     * mistake. */
    {
        struct { float d, b, x, z, r; int kind, index; } near[10];
        int count = 0;

        n = geWorldPropCount();
        for (i = 0; i < n; i++) {
            GeWorldProp p2;
            float dx, dz, d;
            int j, k;

            if (!geWorldProp(i, &p2)) { continue; }
            dx = p2.x - st.x;
            dz = p2.z - st.z;
            d = (float) sqrt((double) ((dx * dx) + (dz * dz)));
            if (d > 900.0f) { continue; }

            /* Insertion sort into a fixed ten: a report longer than that stops being readable,
             * and the tenth-nearest crate has never changed anyone's next move. */
            /* Find the slot only. The shift belongs to the loop below and doing it here as
             * well copied every displaced entry twice, so the list reported the same prop at the
             * same distance in two consecutive rows -- visible the moment the props carried
             * their pack index, and invisible for as long as they did not. */
            k = count < 10 ? count : 9;
            while (k > 0 && near[k - 1].d > d) { k--; }
            if (k < 10) {
                for (j = (count < 10 ? count : 9); j > k; j--) { near[j] = near[j - 1]; }
                near[k].d = d;
                near[k].b = ge_cli_rel(st.x, st.z, p2.x, p2.z, st.angle);
                near[k].kind = p2.kind;
                near[k].index = i;
                near[k].x = p2.x;
                near[k].z = p2.z;
                near[k].r = p2.radius;
                if (count < 10) { count++; }
            }
        }
        for (i = 0; i < count; i++) {
            /* the radius turns A distance into A Surface. "crate 278 away" is 278 to its centre,
             * so a reader with room to spare has already walked into the corner of it; with
             * "radius 120" the same line says the surface is at 158.
             *
             * Printed as "radius ?" when the pack has none rather than as "radius 0". Guards
             * have no model box -- 664 props across the twenty levels -- and a reader shown 0
             * would treat a person as point-sized and walk straight through them. Unknown and
             * zero-sized are opposite claims and only one of them is true here.
             *
             * It is the CIRCUMRADIUS, so it is orientation-safe but generous: it over-reserves
             * for a square footprint by about 41%. That is the correct direction to be wrong in
             * for clearance, and the half-extents are in the pack for a caller that knows the
             * prop's rotation and wants the tighter number. */
            if (near[i].r > 0.0f) {
                printf("near   %-13s #%-4d %4.0f away, turn %+.0f, at (%.0f %.0f), radius %.0f\n",
                       geWorldPropKindName(near[i].kind), near[i].index,
                       (double) near[i].d, (double) near[i].b,
                       (double) near[i].x, (double) near[i].z, (double) near[i].r);
            } else {
                printf("near   %-13s #%-4d %4.0f away, turn %+.0f, at (%.0f %.0f), radius ?\n",
                       geWorldPropKindName(near[i].kind), near[i].index,
                       (double) near[i].d, (double) near[i].b,
                       (double) near[i].x, (double) near[i].z);
            }
        }
    }

    /* Threats before objectives, deliberately.
     *
     * The objective line is the last thing in a report, which makes it the natural signal that a
     * report is complete -- tools/play_cli.py acts on it. With the enemies printed after it, every
     * threat decision was taken against the PREVIOUS report's enemy list, one report stale, which
     * on a moving guard is the difference between shooting at it and shooting where it was.
     */
    /* Enemies: only the ones close enough to matter, nearest first would need a sort and this is
     * a report, not a tactical display. Range and bearing are what a person acts on. */
    n = geEnemyCount();
    for (i = 0; i < n && shown < 6; i++) {
        GeEnemy e;
        float d;
        if (!geEnemy(i, &e) || !e.alive) { continue; }
        d = (float) sqrt((double) (((e.x - st.x) * (e.x - st.x)) + ((e.z - st.z) * (e.z - st.z))));
        if (d > 2500.0f) { continue; }
        /* Health and alertness alongside range and bearing, because the three questions a
         * player actually asks are "can it see me", "is it still a threat" and "is it already
         * dying". Firing into a corpse is the commonest way an automated player wastes a magazine
         * and its attention: the death animation runs for a while after health reaches zero, and
         * the character stays in the world the whole time. */
        {
            const char *state = "";

            if ((e.fields & GE_EN_HEALTH) && e.health <= 0.0f) { state = "  DYING"; }
            else if (geSenseVisibleTo(i, ge_cli_slot))         { state = "  SEES YOU"; }

            printf("enemy  #%-3d %4.0f away, turn %+.0f", e.id, (double) d,
                   (double) ge_cli_rel(st.x, st.z, e.x, e.z, st.angle));
            /* Remaining over threshold, not a percentage. Guards on Train report more remaining
             * than their own threshold, so a percentage here reads as 150% and looks like a bug
             * in the report rather than a fact about the character. Showing both numbers says
             * what is actually known without having to explain it. */
            if (e.fields & GE_EN_HEALTH) { printf(", hp %.0f/%.0f", (double) e.health,
                                                  (double) e.max_health); }
            if (e.fields & GE_EN_ALERT)  { printf(", alert %d", e.alertness); }
            printf("%s\n", state);
        }
        shown++;
    }
    /* Objective STATE, not just where it is. Distance says how far; this says whether it still
     * matters -- and a player told only distance keeps walking at something already done. */
    {
        extern int gePortObjectiveCount(void);
        extern int gePortObjectiveStatus(int index, int *out_status);
        static const char *names[3] = { "incomplete", "COMPLETE", "FAILED" };
        int oi, on = gePortObjectiveCount();

        for (oi = 0; oi < on && oi < 8; oi++) {
            int stt;
            if (!gePortObjectiveStatus(oi, &stt)) { continue; }
            printf("objectv %d  %s\n", oi, (stt >= 0 && stt < 3) ? names[stt] : "?");
        }
    }

    {
        GeWorldObjective ob;
        if (geWorldObjective(0, &ob)) {
            printf("obj    %4.0f away, turn %+.0f%s\n",
                   (double) sqrt((double) (((ob.tx - st.x) * (ob.tx - st.x))
                                         + ((ob.tz - st.z) * (ob.tz - st.z)))),
                   (double) ge_cli_rel(st.x, st.z, ob.tx, ob.tz, st.angle),
                   ob.steps ? "" : "  (no route solved)");
        }
    }

    if (ge_cli_path_now) {
        /* A route over ground the body fits on, rather than over the level's pad graph. See
         * ge_local_path.c -- this is the answer to "how do I get round the thing in front of
         * me", which no bearing-and-range line in this report can give. */
        extern int gePortLocalPath(float px, float pz, float tx, float tz, float cell,
                                   float *out_x, float *out_z, int max);
        float ob_x, ob_z, wx[12], wz[12];
        int k, got;

        ge_cli_path_now = 0;
        if (ge_cli_objective_point(&ob_x, &ob_z)) {
            /* A finer grid finds a way through a pinch that a coarse one steps over: the
             * search only knows a cell is passable if its centre is, and Train's carriages have
             * places where the body fits on one line and not on the line sixty units beside it. */
            got = gePortLocalPath(st.x, st.z, ob_x, ob_z,
                                  (ge_cli_path_cell >= 10.0f) ? ge_cli_path_cell : 60.0f,
                                  wx, wz, 12);
            if (got <= 0) {
                printf("path   nothing reachable from here gets any nearer\n");
            } else {
                for (k = 0; k < got; k++) {
                    printf("path   %d: (%.0f %.0f)  %.0f away, turn %+.0f\n", k + 1,
                           (double) wx[k], (double) wz[k],
                           (double) sqrt((double) ((wx[k] - st.x) * (wx[k] - st.x)
                                                 + (wz[k] - st.z) * (wz[k] - st.z))),
                           (double) ge_cli_rel(st.x, st.z, wx[k], wz[k], st.angle));
                }
            }
        }
    }

    if (ge_cli_map_now) {
        ge_cli_map_now = 0;
        ge_cli_print_map(&st, ge_cli_map_cell);
    }

    printf("> ");
    fflush(stdout);
}

void gePortCliFrame(int frame)
{
    GePlayerInput in;

    if (ge_cli_on < 0) { ge_cli_setup(); }
    if (!ge_cli_on) { return; }

    ge_cli_poll_stdin();


    if (ge_cli_report_now || (frame % ge_cli_every) == 0) {
        ge_cli_report_now = 0;
        ge_cli_report(frame);
    }

    memset(&in, 0, sizeof in);
    if (ge_cli_hold > 0) {
        ge_cli_hold--;
        in.stick_x = (signed char) ge_cli_sx;
        in.stick_y = (signed char) ge_cli_sy;
        in.buttons = ge_cli_buttons;
    }
    /* Posted every tick even when neutral: a slot that goes quiet falls back to whatever was
     * held, and the player would keep walking after the command ended. */
    gePlayerPost(ge_cli_slot, gePlayerTick() + 1, &in, 1);
}
