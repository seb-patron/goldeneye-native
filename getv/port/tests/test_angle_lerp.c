/* Shortest-path angle interpolation, as used for character yaw between simulation ticks.
 *
 * The failure this guards against: a guard turning from 350 degrees to 10 should sweep 20
 * degrees forward through zero, not 340 degrees backward. Interpolating the raw numbers does the
 * second one, and the result is a model spinning most of a full turn in a single frame, which is
 * far worse than the step that interpolation was added to remove.
 *
 *   clang -o /tmp/t getv/port/tests/test_angle_lerp.c -lm && /tmp/t
 */
#include <stdio.h>
#include <math.h>

static int fails;

/* Reports on the way through rather than only on failure. Silence is not evidence: the runner
 * counts result lines, and a test that prints nothing when it is happy scores zero checks, which
 * looks exactly like a test whose body has been deleted. */
static void check(int cond, const char *what)
{
    if (cond) { printf("  ok    %s\n", what); return; }
    printf("  FAIL  %s\n", what);
    fails++;
}

/* The function under test, mirroring frametiming.c. */
static float lerp_angle(float from, float to, float a)
{
    float d = to - from;
    while (d >  (float) M_PI) { d -= (float) (2.0 * M_PI); }
    while (d < -(float) M_PI) { d += (float) (2.0 * M_PI); }
    return from + d * a;
}

static float deg(float r) { return r * 180.0f / (float) M_PI; }
static float rad(float d) { return d * (float) M_PI / 180.0f; }

/* How far apart two angles are, ignoring which way round. */
static float apart(float a, float b)
{
    float d = fmodf(fabsf(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

int main(void)
{
    float mid;

    printf("shortest-path angle interpolation\n");

    /* The wrap case. Halfway from 350 to 10 is 0, not 180. */
    mid = deg(lerp_angle(rad(350.0f), rad(10.0f), 0.5f));
    printf("  350 -> 10 at 0.5 = %.1f\n", mid);
    check(apart(mid, 0.0f) < 0.5f, "wrapping forward through zero takes the short way");

    /* And the same backwards. */
    mid = deg(lerp_angle(rad(10.0f), rad(350.0f), 0.5f));
    printf("  10 -> 350 at 0.5 = %.1f\n", mid);
    check(apart(mid, 0.0f) < 0.5f, "wrapping backward through zero takes the short way");

    /* An ordinary turn with no wrap involved. */
    mid = deg(lerp_angle(rad(80.0f), rad(100.0f), 0.5f));
    printf("  80 -> 100 at 0.5 = %.1f\n", mid);
    check(apart(mid, 90.0f) < 0.5f, "an ordinary turn interpolates linearly");

    /* The ends are exact, because a tick boundary must land on the simulated value. */
    check(apart(deg(lerp_angle(rad(350.0f), rad(10.0f), 0.0f)), 350.0f) < 0.01f,
          "alpha 0 is the previous state exactly");
    check(apart(deg(lerp_angle(rad(350.0f), rad(10.0f), 1.0f)), 10.0f) < 0.01f,
          "alpha 1 is the current state exactly");

    /* Exactly opposite is the ambiguous case: either way round is 180 degrees. It must not spin
     * further than that, whichever it picks. */
    mid = deg(lerp_angle(rad(0.0f), rad(180.0f), 0.5f));
    printf("  0 -> 180 at 0.5 = %.1f\n", mid);
    check(apart(mid, 90.0f) < 0.5f || apart(mid, 270.0f) < 0.5f,
          "the opposite case turns 180 degrees, not more");

    /* No interpolation should ever travel more than half a turn. */
    {
        int a, b;
        int worst_ok = 1;
        for (a = 0; a < 360; a += 7) {
            for (b = 0; b < 360; b += 7) {
                float m = deg(lerp_angle(rad((float) a), rad((float) b), 0.5f));
                if (apart(m, (float) a) > 90.5f) { worst_ok = 0; }
            }
        }
        check(worst_ok, "no pair travels more than a quarter turn by the halfway point");
    }

    if (fails == 0) { printf("all checks passed\n"); }
    return fails != 0;
}
