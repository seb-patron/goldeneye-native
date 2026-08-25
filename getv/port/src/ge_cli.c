/* Play GoldenEye from a terminal, through the API alone.
 *
 * WHY THIS IS THE REAL TEST OF THE API
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
 * ⚠️ stdin is read NON-BLOCKING. Blocking would stall the game loop, and a frozen game with a
 * prompt looks exactly like a crash -- which is how the first version presented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "ge_player_api.h"
#include "ge_world_api.h"
#include "ge_sense_api.h"
#include "ge_enemy_api.h"

#define GE_CLI_WALK 60

static int   ge_cli_on = -1;
static int   ge_cli_slot;
static int   ge_cli_every;
static int   ge_cli_hold;          /* ticks left on the current command */
static int   ge_cli_sx, ge_cli_sy;
static unsigned int ge_cli_buttons;
static int   ge_cli_report_now;

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
#endif
    gePlayerClaim(ge_cli_slot, GE_SLOT_INJECTED);
    printf("[cli] playing slot %d. commands: w/s/a/d <ticks>, use, fire, look, stop, quit\n",
           ge_cli_slot);
    fflush(stdout);
}

static void ge_cli_command(const char *line)
{
    char verb[32];
    int n = 0;

    if (sscanf(line, "%31s %d", verb, &n) < 1) { return; }
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

static void ge_cli_poll_stdin(void)
{
#ifndef _WIN32
    static char buf[256];
    static int len;
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
        }
    }

    /* Landmarks, as bearings a person can steer by. */
    {
        static const int kinds[3] = { GE_PROP_DOOR, GE_PROP_KEY, GE_PROP_COLLECTABLE };
        unsigned int k;
        for (k = 0; k < 3; k++) {
            if (!geWorldNearestProp(kinds[k], st.x, st.y, st.z, &pr)) { continue; }
            printf("%-6s %4.0f away, turn %+.0f, room %d\n",
                   geWorldPropKindName(kinds[k]),
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
        struct { float d, b; int kind; } near[10];
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
            k = count < 10 ? count : 9;
            while (k > 0 && near[k - 1].d > d) {
                if (k < 10) { near[k] = near[k - 1]; }
                k--;
            }
            if (k < 10) {
                for (j = (count < 10 ? count : 9); j > k; j--) { near[j] = near[j - 1]; }
                near[k].d = d;
                near[k].b = ge_cli_rel(st.x, st.z, p2.x, p2.z, st.angle);
                near[k].kind = p2.kind;
                if (count < 10) { count++; }
            }
        }
        for (i = 0; i < count; i++) {
            printf("near   %-13s %4.0f away, turn %+.0f\n",
                   geWorldPropKindName(near[i].kind), (double) near[i].d, (double) near[i].b);
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

    /* Enemies: only the ones close enough to matter, nearest first would need a sort and this is
     * a report, not a tactical display. Range and bearing are what a person acts on. */
    n = geEnemyCount();
    for (i = 0; i < n && shown < 6; i++) {
        GeEnemy e;
        float d;
        if (!geEnemy(i, &e) || !e.alive) { continue; }
        d = (float) sqrt((double) (((e.x - st.x) * (e.x - st.x)) + ((e.z - st.z) * (e.z - st.z))));
        if (d > 2500.0f) { continue; }
        printf("enemy  %4.0f away, turn %+.0f%s\n", (double) d,
               (double) ge_cli_rel(st.x, st.z, e.x, e.z, st.angle),
               geSenseVisibleTo(i, ge_cli_slot) ? "  SEES YOU" : "");
        shown++;
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
