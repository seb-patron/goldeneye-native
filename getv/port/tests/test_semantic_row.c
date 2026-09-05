#include <stdio.h>
#include <string.h>

static unsigned long fake_sample;
static unsigned long fake_presentation;
static int fake_paused;
static int fake_position_available;
static float fake_position[3];

unsigned long gePlayerTick(void) { return fake_sample; }
unsigned long gePortRenderedFrame(void) { return fake_presentation; }
int checkGamePaused(void) { return fake_paused; }
int g_GlobalTimer;
int gePortPlayerPos(int index, float *out)
{
    (void)index;
    if (!fake_position_available) { return 0; }
    out[0] = fake_position[0];
    out[1] = fake_position[1];
    out[2] = fake_position[2];
    return 1;
}

#include "ge_semantic_row.c"

static int failures;

static void check_i(const char *what, long long got, long long want)
{
    if (got == want) {
        printf("  ok    %-58s %lld\n", what, got);
    } else {
        printf("  FAIL  %-58s got %lld want %lld\n", what, got, want);
        failures++;
    }
}

static void check_s(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) == 0) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n        got:  %s        want: %s", what, got, want);
        failures++;
    }
}

static void reset_exporter(unsigned int limit, unsigned int delay)
{
    memset(&ge_semantic, 0, sizeof ge_semantic);
    ge_semantic.configured = 1;
    ge_semantic.row_limit = limit;
    ge_semantic.row_delay = delay;
    fake_sample = 10;
    fake_presentation = 20;
    fake_paused = 0;
    g_GlobalTimer = 30;
    fake_position_available = 1;
    fake_position[0] = 1.0f;
    fake_position[1] = 2.0f;
    fake_position[2] = 3.0f;
}

static void test_synchronized_admitted_row(void)
{
    char output[384];

    reset_exporter(1, 0);
    check_i("admission hook preserves an admitted return", geSemanticRowAdmit(1), 1);
    fake_sample = 11;
    fake_presentation = 21;
    fake_paused = 1;
    g_GlobalTimer = 34;
    fake_position[2] = 4.0f;
    geSemanticRowEnd();

    check_i("one admitted row emitted", ge_semantic.emitted, 1);
    check_i("logical sample is captured at the admission boundary",
            (long long)ge_semantic.row.logical_sample_ordinal, 10);
    check_i("admission result is discrete", ge_semantic.row.admission_result, 1);
    check_i("pause state is observed after the iteration settles", ge_semantic.row.pause_state, 1);
    check_i("timer before is synchronized", ge_semantic.row.integer_timer_before, 30);
    check_i("timer after is synchronized", ge_semantic.row.integer_timer_after, 34);
    check_i("motion reduces to one discrete changed marker",
            ge_semantic.row.semantic_marker, GE_SEMANTIC_MARKER_CHANGED);
    check_i("presentation before is synchronized",
            (long long)ge_semantic.row.presentation_ordinal_before, 20);
    check_i("presentation after is synchronized",
            (long long)ge_semantic.row.presentation_ordinal_after, 21);

    check_i("fixed schema formats within its bound",
            ge_semantic_row_format(output, sizeof output, &ge_semantic.row) > 0, 1);
    check_s("output contains exactly the REF-007 allowlisted numeric fields", output,
            "{\"logical_sample_ordinal\":10,\"admission_result\":1,\"pause_state\":1,"
            "\"integer_timer_before\":30,\"integer_timer_after\":34,"
            "\"semantic_marker\":1,\"presentation_ordinal_before\":20,"
            "\"presentation_ordinal_after\":21}\n");
}

static void test_refusal_and_marker_domains(void)
{
    reset_exporter(3, 0);
    check_i("admission hook preserves a refused return", geSemanticRowAdmit(0), 0);
    check_i("refused row completes without a render callback", ge_semantic.emitted, 1);
    check_i("refusal stays explicit", ge_semantic.row.admission_result, 0);
    check_i("refusal preserves timer identity", ge_semantic.row.integer_timer_after, 30);
    check_i("refusal preserves presentation identity",
            (long long)ge_semantic.row.presentation_ordinal_after, 20);
    check_i("unchanged available motion is stable", ge_semantic.row.semantic_marker,
            GE_SEMANTIC_MARKER_STABLE);

    fake_position_available = 0;
    geSemanticRowAdmit(1);
    geSemanticRowEnd();
    check_i("an absent motion source is unavailable, never plausible zero",
            ge_semantic.row.semantic_marker, GE_SEMANTIC_MARKER_UNAVAILABLE);
}

static void test_bounded_controls(void)
{
    unsigned int parsed = 0;

    check_i("row limit parser accepts its maximum",
            ge_semantic_parse_unsigned("4096", 1u, GE_SEMANTIC_ROW_LIMIT_MAX, &parsed), 1);
    check_i("row limit parser preserves its maximum", parsed, 4096);
    check_i("row limit parser rejects zero",
            ge_semantic_parse_unsigned("0", 1u, GE_SEMANTIC_ROW_LIMIT_MAX, &parsed), 0);
    check_i("row limit parser rejects a sign",
            ge_semantic_parse_unsigned("+3", 1u, GE_SEMANTIC_ROW_LIMIT_MAX, &parsed), 0);
    check_i("row limit parser rejects whitespace",
            ge_semantic_parse_unsigned(" 3", 1u, GE_SEMANTIC_ROW_LIMIT_MAX, &parsed), 0);
    check_i("row limit parser rejects a suffix",
            ge_semantic_parse_unsigned("3x", 1u, GE_SEMANTIC_ROW_LIMIT_MAX, &parsed), 0);
    check_i("row limit parser rejects overflow of the policy bound",
            ge_semantic_parse_unsigned("4097", 1u, GE_SEMANTIC_ROW_LIMIT_MAX, &parsed), 0);

    reset_exporter(1, 2);
    geSemanticRowAdmit(0);
    geSemanticRowAdmit(0);
    check_i("bounded pre-row delay emits nothing early", ge_semantic.emitted, 0);
    geSemanticRowAdmit(0);
    check_i("first post-delay attempt emits", ge_semantic.emitted, 1);
    geSemanticRowAdmit(0);
    check_i("bounded row limit stops further output", ge_semantic.emitted, 1);
}

int main(void)
{
    printf("content-free synchronized semantic-row exporter\n\n");
    test_synchronized_admitted_row();
    test_refusal_and_marker_domains();
    test_bounded_controls();
    printf("\n%s -- %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
