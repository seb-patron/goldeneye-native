/* Content-free synchronized semantic rows for bounded reference observation.
 *
 * The exporter is deliberately numeric and closed-schema. It has no path, string, pointer,
 * memory, screenshot, save, trace, or arbitrary-field input. GETV_SEMANTIC_ROWS=<1..4096>
 * enables that many JSON Lines on stdout; GETV_SEMANTIC_ROW_DELAY=<0..1000000> skips a bounded
 * number of attempted rows first. Repeating an identical run, selecting an independently
 * invented motion control, and varying presentation cadence remain existing harness controls;
 * none can add fields to the row.
 */
#ifndef GE_SEMANTIC_ROW_H
#define GE_SEMANTIC_ROW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Begin at the established post-input, pre-simulation admission boundary. A refusal completes
 * immediately because the caller intentionally performs neither simulation nor presentation. */
int geSemanticRowAdmit(int admission_result);

/* Complete after the corresponding presentation has settled. */
void geSemanticRowEnd(void);

#ifdef __cplusplus
}
#endif
#endif /* GE_SEMANTIC_ROW_H */
