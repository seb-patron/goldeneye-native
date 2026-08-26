/* GPU-side frame timing, to answer one question the CPU profiler cannot.
 *
 * the QUESTION. At steady state this port spends 0.7 ms/frame in the render pipeline and 1.6-3.3
 * ms of CPU per frame, against a 7-16 ms frame. Five to eight milliseconds of every frame has no
 * CPU ATTACHED and is not vsync, not the frame cap, not fill rate and not stdout -- all of those
 * were measured and excluded (docs/PERFORMANCE.md). Two explanations remain and they are opposite:
 *
 *   the GPU is genuinely busy      -> the work is real; reduce it or accept it
 *   the CPU is blocked in the swap -> the GPU is idle and the driver or compositor is stalling us
 *
 * From outside the process these look identical: neither burns CPU in our thread. A GPU timer
 * separates them, and nothing else available here does.
 *
 * The measurement must not perturb what it measures. Reading a query result with
 * GL_QUERY_RESULT blocks until the GPU has finished, which inserts exactly the pipeline stall this
 * code exists to detect -- it would report a busy GPU no matter what was true, and it would be
 * Self-fulfilling. Results are therefore read several frames late and only after
 * GL_QUERY_RESULT_AVAILABLE says so. If a result is not ready, it is skipped rather than waited
 * for.
 *
 * Enabled with GETV_GPUTIME=1. Off by default: GL_TIME_ELAPSED is cheap but not free, and an
 * archival build should not carry an always-on profiler.
 */
#ifndef GE_GPU_TIMER_H
#define GE_GPU_TIMER_H

/* Is the timer switched on and usable? False when GETV_GPUTIME is unset, when the driver lacks
 * ARB_timer_query, or after the queries failed to allocate. Callers check this rather than
 * tracking the reason themselves. */
int  geGpuTimerEnabled(void);

/* Bracket the whole frame's GL work. Begin before the first draw of the frame, end after the last
 * -- including the buffer swap, because whether the swap blocks is half the question. */
void geGpuTimerFrameBegin(void);
void geGpuTimerFrameEnd(void);

/* CPU wall time spent inside the present, in milliseconds, recorded by the caller that brackets
 * it. Kept separate from the GPU figure: "the GPU was busy 6 ms" and "we sat in
 * SwapWindow for 6 ms" are the two answers being told apart, and averaging them into one number
 * would destroy the distinction. */
void geGpuTimerRecordSwap(double ms);

#endif /* GE_GPU_TIMER_H */
