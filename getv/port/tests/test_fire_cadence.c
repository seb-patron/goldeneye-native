/* Does an automatic weapon fire at the same rate in real time whatever the simulation divider?
 *
 * Retail asks whether a TICK counter is a multiple of the weapon's rate. Under a simulation
 * divider a tick is worth n fields of real time, so that test keeps the rate constant per tick
 * and divides it per second: at divider 2 the gun fires half as often as it should.
 *
 * gePortAutoFireDue replaces the modulo with a crossing test on the FIELD counter, which is
 * the same quantity measured in real time. This models both and asserts the difference, so a
 * regression shows up here rather than as a gun that sounds wrong.
 *
 * Built and run by tests/run_tests.ps1 and by hand:
 *   clang -o /tmp/t getv/port/tests/test_fire_cadence.c && /tmp/t
 */
#include <stdio.h>

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

/* The retail test: fire when the tick counter is a multiple of the rate. */
static int fire_by_tick(int ticks, int rate)
{
    return rate > 0 && (ticks % rate) == 0;
}

/* The ported test: fire when the FIELD counter crosses a multiple of the rate. `delta` is how
 * many fields this tick was worth, which is the divider. */
static int fire_by_field(int fields, int delta, int rate)
{
    int prev;
    if (rate <= 0) { return 1; }
    if (fields == 0) { return 1; }
    prev = fields - delta;
    if (prev < 0) { prev = 0; }
    return (prev / rate) != (fields / rate);
}

/* Count shots over a fixed span of REAL TIME, expressed in video fields. */
static int shots_over(int total_fields, int divider, int rate, int time_based)
{
    int fields = 0;
    int ticks = 0;
    int shots = 0;

    while (fields < total_fields) {
        fields += divider;          /* one simulation tick is `divider` fields of real time */
        ticks  += 1;
        if (time_based ? fire_by_field(fields, divider, rate)
                       : fire_by_tick(ticks, rate)) {
            shots++;
        }
    }
    return shots;
}

int main(void)
{
    /* 3600 fields is one minute of video at 60Hz. Rate 4 is a typical automatic. */
    const int span = 3600;
    const int rate = 4;
    int base, d;

    printf("fire cadence over %d fields, rate %d\n", span, rate);

    base = shots_over(span, 1, rate, 1);
    printf("  time-based:  divider 1 = %d shots\n", base);

    /* THE CONTRACT. The rate holds in real time for as long as the simulation ticks at least as
     * often as the weapon fires. Past that the gun is clipped to one shot per tick, because a
     * weapon cannot fire between simulation steps that do not happen. That is a property of a
     * fixed timestep rather than a defect, and it is why the automatic divider targets 30Hz: the
     * fastest weapon in the game wants 29.2 rounds a second. */
    for (d = 2; d <= 8; d *= 2) {
        int n = shots_over(span, d, rate, 1);
        if (d <= rate) {
            int drift = n > base ? n - base : base - n;
            printf("  time-based:  divider %d = %d shots (drift %d)\n", d, n, drift);
            check(drift <= 1, "time-based rate holds while the simulation outpaces the weapon");
        } else {
            int ticks = span / d;
            printf("  time-based:  divider %d = %d shots, clipped to the tick rate %d\n",
                   d, n, ticks);
            check(n <= ticks + 1, "past the floor the gun clips to one shot per tick");
            check(n >= ticks - 1, "and it does not fall further than that");
        }
    }

    for (d = 2; d <= 8; d *= 2) {
        int n = shots_over(span, d, rate, 0);
        printf("  tick-based:  divider %d = %d shots\n", d, n);
        check(n < base / 2 + 1, "tick-based rate falls with the divider, as retail does");
    }

    /* A rate the divider does not divide evenly is the case most likely to drift. */
    base = shots_over(span, 1, 3, 1);
    for (d = 2; d <= 3; d++) {
        int n = shots_over(span, d, 3, 1);
        int drift = n > base ? n - base : base - n;
        printf("  rate 3, divider %d = %d shots against %d (drift %d)\n", d, n, base, drift);
        check(drift <= 1, "an uneven rate still holds while the simulation outpaces it");
    }

    /* The configuration the port ships. `divider` here counts VIDEO FIELDS per simulation tick,
     * not rendered frames, so the 30Hz simulation the auto divider targets is two fields per
     * tick whatever the display is doing. The fastest weapon in the game wants 29.2 rounds a
     * second, which is one shot every two fields, and that is exactly what a 30Hz simulation can
     * still deliver. This is the check that says the floor was put in the right place. */
    {
        int fast = 2;                      /* two fields between shots is 30 rounds a second */
        int at60 = shots_over(span, 1, fast, 1);   /* 60Hz simulation */
        int at30 = shots_over(span, 2, fast, 1);   /* 30Hz simulation, what ships */
        int drift = at30 > at60 ? at30 - at60 : at60 - at30;
        printf("  fastest weapon: 60Hz sim = %d shots, 30Hz sim = %d (drift %d)\n",
               at60, at30, drift);
        check(drift <= 1, "a 30Hz simulation still delivers the fastest weapon's full rate");
    }

    if (fails == 0) { printf("all checks passed\n"); }
    return fails != 0;
}
