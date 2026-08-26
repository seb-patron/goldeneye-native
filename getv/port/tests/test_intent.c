/* Exercises ge_intent_to_buttons across all eight control styles.
 *
 * It includes the .c rather than linking it, because the function under test is static and making
 * it non-static purely to test it would change the shipping surface to suit the test.
 *
 * What this is actually for: the two-controller mapping bug was invisible from outside. Input was
 * accepted, the bot moved, and the only symptom was the DIRECTION it moved. A test that asserts on
 * the returned bits is the cheapest thing that could have caught it, and it needs no game running.
 */

#include <stdio.h>

static unsigned int g_test_style;

/* Stubs for everything ge_player_api.c reaches for. Only the style matters here; the rest exist so
 * the translation unit links. */
unsigned int get_player_control_style(int playernum) { (void) playernum; return g_test_style; }

/* The port accessor the gePlayerControlType calls. It returns -1 for a slot with no style,
 * which is what makes gePlayerSlotIsDrivable's `>= 0` meaningful; the fake population here always
 * has a style, so slot 0 answers and everything else does not. */
int gePortPlayerControlStyle(int idx) { return (idx == 0) ? (int) g_test_style : -1; }
int  getPlayerCount(void) { return 1; }
unsigned short joyGetButtons(signed char p, unsigned short m) { (void) p; (void) m; return 0; }
signed char joyGetStickX(signed char p) { (void) p; return 0; }
signed char joyGetStickY(signed char p) { (void) p; return 0; }
void joySetContDataIndex(int i) { (void) i; }
int  gePortPlayerPos(int idx, float *out) { (void) idx; (void) out; return 0; }

#define GE_TEST_BUILD 1
#include "ge_player_api.c"

/* These two need types that only exist once the unit above is included, so they come after it
 * rather than with the other stubs. */
void joySetPlaybackFunc(ge_contplaybackfunc func, s32 count) { (void) func; (void) count; }
u64  g_randomSeed = 0xAB8D9F7781280783ULL;   /* matches port_random.c's real initialiser */

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %-42s 0x%04x\n", what, got);
    } else {
        printf("  FAIL  %-42s got 0x%04x want 0x%04x\n", what, got, want);
        failures++;
    }
}

struct Case {
    unsigned int style;
    const char  *name;
    int          fire;      /* expected bits for GE_IN_FIRE */
    int          aim;       /* expected bits for GE_IN_AIM  */
    int          drivable;
};

int main(void)
{
    /* Expectations come from bondview2.c, not from the implementation:
     *   1.x  5546-5558 : Kissy/Goodnight swap shoot and aim; everyone else shoot = Z_TRIG
     *   2.x  5345-5359 : Plenty/Galore put SHOOT on pad 1, Domino/Goodhead put AIM on pad 1,
     *                    and whichever is on pad 2 is unreachable through this one-pad API. */
    struct Case cases[] = {
        { GE_STYLE_HONEY,     "1.1 Honey",     Z_TRIG,          L_TRIG | R_TRIG, 1 },
        { GE_STYLE_SOLITARE,  "1.2 Solitare",  Z_TRIG,          L_TRIG | R_TRIG, 1 },
        { GE_STYLE_KISSY,     "1.3 Kissy",     A_BUTTON,        Z_TRIG,          1 },
        { GE_STYLE_GOODNIGHT, "1.4 Goodnight", A_BUTTON,        Z_TRIG,          1 },
        /* Every style IS drivable, including the two-PAD ones.
         *
         * This column said 0 for 2.x when the file was written, on the theory that a slot whose
         * movement axis lives on a second pad cannot be driven. That was wrong and it was the
         * expensive kind of wrong: this port DEFAULTS to 2.2 Galore, so "2.x is undrivable"
         * disabled every bot on every level, and ge_bot_route printed a confident explanation for
         * why it was giving up.
         *
         * Two-pad is a ROUTING fact, not a disqualification -- the answer is to write the pad the
         * engine actually reads for movement, which ge_playback now does in a second pass. The
         * button half below is still real and still per-style; the two are independent. */
        { GE_STYLE_PLENTY,    "2.1 Plenty",    Z_TRIG,          0,               1 },
        { GE_STYLE_GALORE,    "2.2 Galore",    Z_TRIG,          0,               1 },
        { GE_STYLE_DOMINO,    "2.3 Domino",    0,               Z_TRIG,          1 },
        { GE_STYLE_GOODHEAD,  "2.4 Goodhead",  0,               Z_TRIG,          1 },
    };
    unsigned i;
    char buf[96];

    printf("ge_intent_to_buttons across all eight control styles\n\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_test_style = cases[i].style;
        ge_unreach_warned[0] = 0;   /* each style re-earns its warning */

        sprintf(buf, "%s FIRE", cases[i].name);
        check(buf, ge_intent_to_buttons(0, GE_IN_FIRE), cases[i].fire);

        sprintf(buf, "%s AIM", cases[i].name);
        check(buf, ge_intent_to_buttons(0, GE_IN_AIM), cases[i].aim);

        sprintf(buf, "%s drivable", cases[i].name);
        check(buf, gePlayerSlotIsDrivable(0), cases[i].drivable);
    }

    /* The regression this file exists for. On Domino, fire must not become Z_TRIG: the game reads
     * pad 1's Z_TRIG as AIM there (5365), insightaimmode goes true, and canNaturalTurn goes false
     * at 5385 -- which switches off yaw at 6394 while strafe at 6069 keeps running. The old code
     * returned Z_TRIG here and that is precisely how a firing bot lost its steering. */
    g_test_style = GE_STYLE_DOMINO;
    ge_unreach_warned[0] = 0;
    check("Domino FIRE is not Z_TRIG (the bug)",
          (ge_intent_to_buttons(0, GE_IN_FIRE) & Z_TRIG) == 0, 1);

    /* And the warning must fire once, not every frame: a bot posting FIRE at 60Hz would otherwise
     * bury the log it is trying to produce. */
    g_test_style = GE_STYLE_DOMINO;
    ge_unreach_warned[0] = 0;
    printf("\n  (one warning line should appear immediately below)\n");
    ge_intent_to_buttons(0, GE_IN_FIRE);
    ge_intent_to_buttons(0, GE_IN_FIRE);
    ge_intent_to_buttons(0, GE_IN_FIRE);

    printf("\n%s -- %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
