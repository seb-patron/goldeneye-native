/* Does a per-field integrator hold its rate when the simulation divider changes?
 *
 * chrobjApplySpeed (propobj.c) drives autogun yaw and pitch tracking, truck turning and the
 * door motion that shares the path. Its accel and decel constants are named PER_FRAME and are
 * applied without any time factor, which looks frame-quantised at a glance. It is not: the whole
 * body sits inside `for (i = 0; i < g_ClockTimer; i++)`, so it steps once per elapsed video
 * FIELD rather than once per simulation tick, and a tick worth n fields does n steps.
 *
 * That is the property this asserts, because it is the kind of thing a later refactor removes by
 * accident while tidying a loop away.
 *
 *   clang -o /tmp/t getv/port/tests/test_field_integrator.c -lm && /tmp/t
 */
#include <stdio.h>
#include <math.h>

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

/* The shape of chrobjApplySpeed: accelerate toward a target, capped, stepping once per field. */
static float travel(int total_fields, int divider, float accel, float max_speed)
{
    float pos = 0.0f;
    float speed = 0.0f;
    int fields = 0;

    while (fields < total_fields) {
        int i;
        /* One simulation tick. g_ClockTimer is how many fields it was worth. */
        for (i = 0; i < divider; i++) {
            speed += accel;
            if (speed > max_speed) { speed = max_speed; }
            pos += speed;
        }
        fields += divider;
    }
    return pos;
}

/* The same integrator with the field loop removed, which is the mistake being guarded against. */
static float travel_per_tick(int total_fields, int divider, float accel, float max_speed)
{
    float pos = 0.0f;
    float speed = 0.0f;
    int fields = 0;

    while (fields < total_fields) {
        speed += accel;
        if (speed > max_speed) { speed = max_speed; }
        pos += speed;
        fields += divider;
    }
    return pos;
}

int main(void)
{
    const int span = 3600;                 /* one minute of video at 60Hz */
    const float accel = 0.0000139626345f;  /* AUTOGUN_YAW_ACCEL_PER_FRAME */
    const float maxsp = 0.0008377581f;     /* AUTOGUN_YAW_MAX_SPEED */
    float base, d1, d2;
    int d;

    printf("per-field integrator across simulation dividers\n");

    base = travel(span, 1, accel, maxsp);
    printf("  per field: divider 1 = %.5f\n", base);

    for (d = 2; d <= 8; d *= 2) {
        float t = travel(span, d, accel, maxsp);
        float err = fabsf(t - base) / base;
        printf("  per field: divider %d = %.5f (%.2f%% from base)\n", d, t, err * 100.0f);
        check(err < 0.02f, "a per-field integrator holds its rate across the divider");
    }

    /* And the failure mode, so the test says what it is protecting against rather than only
     * that something passed. */
    d1 = travel_per_tick(span, 1, accel, maxsp);
    d2 = travel_per_tick(span, 4, accel, maxsp);
    printf("  per tick:  divider 1 = %.5f, divider 4 = %.5f\n", d1, d2);
    check(d2 < d1 * 0.6f, "dropping the field loop would slow the turret with the divider");

    if (fails == 0) { printf("all checks passed\n"); }
    return fails != 0;
}
