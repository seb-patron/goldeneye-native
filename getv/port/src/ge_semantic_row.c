/* OWNER-030 / REF-007 synchronized semantic-row exporter.
 *
 * Only the fixed numeric schema below crosses the observation boundary. Player coordinates are
 * sampled solely to reduce them to one independently defined motion marker: unavailable (-1),
 * stable (0), or changed (1). They are never formatted or retained after the row completes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ge_semantic_row.h"

#define GE_SEMANTIC_ROW_LIMIT_MAX 4096u
#define GE_SEMANTIC_ROW_DELAY_MAX 1000000u
#define GE_SEMANTIC_MARKER_UNAVAILABLE (-1)
#define GE_SEMANTIC_MARKER_STABLE       0
#define GE_SEMANTIC_MARKER_CHANGED      1

typedef struct GeSemanticRow {
    uint64_t logical_sample_ordinal;
    int admission_result;
    int pause_state;
    int64_t integer_timer_before;
    int64_t integer_timer_after;
    int semantic_marker;
    uint64_t presentation_ordinal_before;
    uint64_t presentation_ordinal_after;
} GeSemanticRow;

typedef struct GeSemanticRowState {
    int configured;
    unsigned int row_limit;
    unsigned int row_delay;
    unsigned int attempts;
    unsigned int emitted;
    int pending;
    int marker_before_available;
    float marker_before[3];
    GeSemanticRow row;
} GeSemanticRowState;

static GeSemanticRowState ge_semantic;

extern unsigned long gePlayerTick(void);
extern unsigned long gePortRenderedFrame(void);
extern int checkGamePaused(void);
extern int g_GlobalTimer;
extern int gePortPlayerPos(int index, float *out);

static int ge_semantic_parse_unsigned(const char *text, unsigned int minimum,
                                      unsigned int maximum, unsigned int *out)
{
    const char *cursor;
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text < '0' || *text > '9') { return 0; }
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') { return 0; }
    }
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value < minimum || value > maximum) { return 0; }
    *out = (unsigned int)value;
    return 1;
}

static void ge_semantic_configure(void)
{
    const char *limit;
    const char *delay;

    if (ge_semantic.configured) { return; }
    ge_semantic.configured = 1;
    limit = getenv("GETV_SEMANTIC_ROWS");
    if (limit == NULL || *limit == '\0') { return; }
    if (!ge_semantic_parse_unsigned(limit, 1u, GE_SEMANTIC_ROW_LIMIT_MAX,
                                    &ge_semantic.row_limit)) {
        fputs("[getv][semantic-row] disabled: GETV_SEMANTIC_ROWS must be 1..4096\n", stderr);
        return;
    }

    delay = getenv("GETV_SEMANTIC_ROW_DELAY");
    if (delay != NULL && *delay != '\0' &&
        !ge_semantic_parse_unsigned(delay, 0u, GE_SEMANTIC_ROW_DELAY_MAX,
                                    &ge_semantic.row_delay)) {
        ge_semantic.row_limit = 0;
        fputs("[getv][semantic-row] disabled: GETV_SEMANTIC_ROW_DELAY must be 0..1000000\n",
              stderr);
    }
}

static int ge_semantic_row_format(char *buffer, size_t size, const GeSemanticRow *row)
{
    int written = snprintf(
        buffer, size,
        "{\"logical_sample_ordinal\":%llu,\"admission_result\":%d,"
        "\"pause_state\":%d,\"integer_timer_before\":%lld,"
        "\"integer_timer_after\":%lld,\"semantic_marker\":%d,"
        "\"presentation_ordinal_before\":%llu,"
        "\"presentation_ordinal_after\":%llu}\n",
        (unsigned long long)row->logical_sample_ordinal,
        row->admission_result,
        row->pause_state,
        (long long)row->integer_timer_before,
        (long long)row->integer_timer_after,
        row->semantic_marker,
        (unsigned long long)row->presentation_ordinal_before,
        (unsigned long long)row->presentation_ordinal_after);
    return written >= 0 && (size_t)written < size ? written : -1;
}

static void ge_semantic_complete(void)
{
    float marker_after[3];
    int marker_after_available;
    char output[384];

    ge_semantic.row.pause_state = checkGamePaused() ? 1 : 0;
    ge_semantic.row.integer_timer_after = (int64_t)g_GlobalTimer;
    ge_semantic.row.presentation_ordinal_after = (uint64_t)gePortRenderedFrame();
    marker_after_available = gePortPlayerPos(0, marker_after);

    if (!ge_semantic.marker_before_available || !marker_after_available) {
        ge_semantic.row.semantic_marker = GE_SEMANTIC_MARKER_UNAVAILABLE;
    } else if (ge_semantic.marker_before[0] != marker_after[0] ||
               ge_semantic.marker_before[1] != marker_after[1] ||
               ge_semantic.marker_before[2] != marker_after[2]) {
        ge_semantic.row.semantic_marker = GE_SEMANTIC_MARKER_CHANGED;
    } else {
        ge_semantic.row.semantic_marker = GE_SEMANTIC_MARKER_STABLE;
    }

    ge_semantic.pending = 0;
    ge_semantic.marker_before[0] = 0.0f;
    ge_semantic.marker_before[1] = 0.0f;
    ge_semantic.marker_before[2] = 0.0f;
    ge_semantic.marker_before_available = 0;

    if (ge_semantic_row_format(output, sizeof output, &ge_semantic.row) < 0) {
        fputs("[getv][semantic-row] fixed row exceeded its internal bound\n", stderr);
        ge_semantic.row_limit = 0;
        return;
    }
    fputs(output, stdout);
    fflush(stdout);
    ge_semantic.emitted++;
}

int geSemanticRowAdmit(int admission_result)
{
    ge_semantic_configure();
    if (ge_semantic.row_limit == 0 || ge_semantic.emitted >= ge_semantic.row_limit) {
        return admission_result;
    }

    /* One begin must pair with one settled presentation. Refuse to splice two iterations into
     * one row if a caller violates that boundary. */
    if (ge_semantic.pending) {
        ge_semantic.pending = 0;
        ge_semantic.row_limit = 0;
        fputs("[getv][semantic-row] disabled: observation boundary overlap\n", stderr);
        return admission_result;
    }

    if (ge_semantic.attempts++ < ge_semantic.row_delay) { return admission_result; }

    ge_semantic.row.logical_sample_ordinal = (uint64_t)gePlayerTick();
    ge_semantic.row.admission_result = admission_result ? 1 : 0;
    ge_semantic.row.pause_state = 0;
    ge_semantic.row.integer_timer_before = (int64_t)g_GlobalTimer;
    ge_semantic.row.integer_timer_after = 0;
    ge_semantic.row.semantic_marker = GE_SEMANTIC_MARKER_UNAVAILABLE;
    ge_semantic.row.presentation_ordinal_before = (uint64_t)gePortRenderedFrame();
    ge_semantic.row.presentation_ordinal_after = 0;
    ge_semantic.marker_before_available = gePortPlayerPos(0, ge_semantic.marker_before);
    ge_semantic.pending = 1;

    if (!admission_result) { ge_semantic_complete(); }
    return admission_result;
}

void geSemanticRowEnd(void)
{
    if (!ge_semantic.pending) { return; }
    ge_semantic_complete();
}
