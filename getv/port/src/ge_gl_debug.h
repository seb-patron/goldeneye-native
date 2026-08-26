/* GETV_GLDEBUG=1 -- find out who is raising GL errors, and when.
 *
 * why. ge_gpu_timer.c drains the GL error queue before allocating its queries and finds a
 * GL_INVALID_OPERATION (0x0502) already pending on every run. Nothing else in this tree calls
 * glGetError, so that error has been raised and ignored for the whole life of the project. An
 * error state can drop a driver off a fast path, which is a candidate for the fixed ~6 ms of
 * GPU-side time per frame that is indifferent to resolution (docs/PERFORMANCE.md).
 *
 * two MECHANISMS, because they answer different questions and the cheap one is not always
 * available:
 *
 *   KHR_debug   a callback fired at the offending call. With GL_DEBUG_OUTPUT_SYNCHRONOUS the
 *               callback runs on the calling thread before the call returns, so a breakpoint in
 *               it yields the real stack. This is the one that names the culprit.
 *
 *   polling     glGetError once per frame, reporting the frame number. Says nothing about WHICH
 *               call, but answers the question that decides where to look at all: is this raised
 *               ONCE during init, or EVERY frame? A one-shot init error and a per-frame error
 *               live in completely different code.
 *
 * Both are off unless asked for. Polling is not free -- glGetError can force a driver flush --
 * so it must not be left on in a build anyone measures with.
 */
#ifndef GE_GL_DEBUG_H
#define GE_GL_DEBUG_H

/* Install the KHR_debug callback if the driver has it. Safe to call repeatedly; acts once.
 * Call as EARLY as possible after the context exists: an error raised before installation is
 * already in the queue and the callback will never see it. */
void geGlDebugInstall(void);

/* Poll once, attributing anything found to `where` and the frame number. Returns the number of
 * errors drained. Call at a known point so "at frame 3, after gfx_run" is a fact rather than an
 * inference. */
int  geGlDebugPoll(const char *where, int frame);

/* Is GETV_GLDEBUG=1 set? Callers skip their instrumentation entirely when it is not. */
int  geGlDebugEnabled(void);

#endif /* GE_GL_DEBUG_H */
