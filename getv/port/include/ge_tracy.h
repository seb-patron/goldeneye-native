/* Tracy profiler zones, or nothing at all.
 *
 * Adapted from wolfpld/tracy @ v0.14.1's public/tracy/TracyC.h (BSD-3-Clause). See
 * docs/REUSE_AUDIT.md and docs/LICENSING.md section 6.
 *
 * REUSE_AUDIT.md's own reasoning for Tracy: "Frame cost is currently unattributed: there is
 * no measurement separating game tick, render, audio and AI." ge_gpu_timer.c already answers
 * a different, narrower question -- GPU timeline busy vs. CPU stalled in swap -- and does not
 * attribute CPU time to any of those four systems. This header is what a call site reaches
 * for instead of including Tracy's own header directly, for one reason: Tracy is fetched on
 * demand into a prefix outside the tree (tools/fetch_deps_windows.ps1, same arrangement as
 * Lua and ImGui), so a checkout that has not run that step still needs to compile. Real
 * TracyC.h already makes every macro a no-op when TRACY_ENABLE is not defined -- that much
 * needs no wrapping. What it cannot do is make itself includable when it was never fetched at
 * all, which is the one thing this file adds.
 *
 * Every call site in this tree uses only the three macros below. Add to this list rather than
 * including <tracy/TracyC.h> directly at a new call site, so there stays exactly one place
 * that knows whether Tracy is present. */
#ifndef GE_TRACY_H
#define GE_TRACY_H

#ifdef GE_WITH_TRACY

#include <tracy/TracyC.h>

#else

/* TracyCZoneCtx itself matches real TracyC.h's own disabled-branch definition exactly (a
 * pointer-sized dummy, never dereferenced). TracyCZoneN does not: upstream's disabled arm is
 * a truly empty macro, correct for the common case where a zone begins and ends in the same
 * scope and nothing outside references the context at all. gfx_opengl.c's frame zone begins
 * in start_frame() and ends in end_frame() -- two different functions -- so it has to carry
 * the context in a file-scope variable between them, and copying out of a variable that was
 * never declared does not compile. Declaring a real (if unused) local here is what keeps that
 * copy legal in both configurations, matching upstream's own type exactly so it compiles
 * identically to real Tracy either way. */
typedef const void* TracyCZoneCtx;

#define TracyCZoneN(ctx, name, active) TracyCZoneCtx ctx = (TracyCZoneCtx) 0
#define TracyCZoneEnd(ctx) (void) (ctx)
#define TracyCFrameMark

#endif /* GE_WITH_TRACY */

#endif /* GE_TRACY_H */
