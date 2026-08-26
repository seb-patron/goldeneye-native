/* Stable body sense: same standing position, same answer.
 *
 * Wraps geSenseAheadForBody. Two changes and nothing else -- the walk starts clear of the asker's
 * own body, and the query is quantised so nearby positions are the same query. See the .c for why
 * those are one fix rather than two.
 *
 * Separate header rather than an addition to ge_sense_api.h so the swap is a single call site and
 * reverting it is that edit backwards.
 */
#ifndef GE_SENSE_STABLE_H
#define GE_SENSE_STABLE_H

#include "ge_sense_api.h"

/* Drop-in replacement for geSenseAheadForBody with the same signature and the same contact
 * semantics. Distances are reported in the CALLER's frame, so a caller comparing distance to its
 * own reach needs to know nothing about the self-clearance offset. */
int geSenseAheadForBodyStable(float x, float z, float heading_deg, float reach,
                              GeSenseContact *out);

/* Exposed for testing: these are the whole stability guarantee, so they are checkable on their own
 * without an engine, a level or a running frame. */
float geSenseQuantise(float v, float cell);
float geSenseQuantiseAngle(float deg, float arc);

/* Flip accounting. A flip is a changed answer at an UNCHANGED query; movement to another cell is
 * not a flip. This is the acceptance number. */
void geSenseStableStats(unsigned long *calls, unsigned long *flips);

#endif
