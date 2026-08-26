# Windows performance: what was measured, and what turned out not to matter

Every number here was measured on the target machine (Surface Pro 3, i5-4300U, Intel HD 4400,
Windows 11) with `tools/bench_windows.ps1` or `tools/bench_ab.ps1`. Claims without a measurement
behind them are not recorded, and two things that *looked* like wins are written down as
non-results so nobody spends a day rediscovering them.

## The headline

The port went from **2.99 fps to 100-138 fps** on this hardware. Almost none of that came from
making the renderer faster. It came from finding things that were not rendering at all.

| change | effect | how it was established |
|---|---|---|
| Gate the ungated `SCISSOR` trace | 2.99 -> 13.70 fps | 27 blocking flushes per frame removed |
| Buffer `stdout` instead of flushing per line | 20.51 -> 63-95 fps | 516 `osSyncPrintf` sites stopped syncing |
| Fix the Windows save path | removes 112 failed writes/run | `rename()` never replaced on Windows |
| `GETV_LOADTRACE=0` | 1,662 -> 1,016 log lines | log length, not frame rate |
| Exclusive fullscreen | 7-13% faster | consistent sign, 3 of 3 paired runs |
| **`-O1` -> `-O2`** | **no measurable difference** | interleaved A/B, sign flipped 2 of 5 |

## The single most important fact

**This build is not CPU-bound.** At steady state the entire render pipeline costs **0.7 ms per
frame** (`GETV_PROF=1`: `draw=0.3ms`, 311 triangles, every texture a cache hit) against a frame
time of 7-16 ms. The process runs at roughly 15-20% CPU utilisation.

Everything follows from that. Compiler optimisation, draw-call batching and shader work all target
computation, and computation is not what this program spends its time on. The wins above are all
I/O and blocking. **Before optimising anything here, check whether the thing you are optimising is
on the critical path** - the profiler answers that in one run and it has now redirected this effort
twice.

## Measuring anything on this machine

**The run-to-run spread is about 20%.** Two identical runs of one binary measured 63 and 95 fps.
A single-run comparison here is worthless and has already produced one wrong answer: a
`GETV_LOADTRACE` A/B came out 44.96 fps with the gate ON and 53.61 with it OFF - noise pointing the
wrong way, which would have been committed as a speedup by anyone reading one number.

**And the spread is not symmetric noise, it is a systematic drift.** Frame rate decays
monotonically within every benchmark as the machine heats: 105 -> 85 fps, then 111 -> 91 on the
next. Whichever build is measured *second* is penalised by a thermal state the first one created.
Running A five times then B five times is therefore **confounded, not merely noisy**, and
repetition cannot fix a bias.

So `tools/bench_ab.ps1` **interleaves** - A B A B A B, alternating which build leads each pair -
and reports a *paired* difference. It prints `NO MEASURABLE DIFFERENCE` when the sign flips
between pairs, which is a real result and the honest one.

**Compare CPU time per frame, not wall clock.** Wall time on a laptop measures the laptop. When the
two disagree that is itself the finding: it means the process is blocked rather than computing,
which is exactly how the `stdout` stall was located.

## Optimisation level

`-Opt` on `build_windows.ps1` selects it; the default is `-O1`, which is what this tree has always
used. `-O2` is **verified not to change behaviour** and is **not measurably faster**, so the
default did not move.

`tools/verify_opt.ps1` is the gate. The port's synthetic clock makes gameplay frames
byte-reproducible (`osGetCount`, `port_os.c`), so two correct builds must emit identical
diagnostics. `-O1` vs `-O2`: **691 distinct line shapes, occurrence counts matched, values
identical.**

**`-fno-strict-aliasing` is not part of the dial and must never be removed.** This source reads
the same memory through incompatible types constantly. That was well-defined on IDO/MIPS and is
undefined in standard C, and letting the optimiser assume it cannot happen miscompiles this tree
into wrong pixels rather than into errors.

**Do not use `-march=native`.** This is meant to be an archival build. `-march=native` produces a
binary that fails with an illegal instruction on any machine older than the one that built it.

**A verification filter can hide the bug it was written to expose.** The first version of
`verify_opt.ps1` reported 18 differences that did not exist: the baseline prints a float
`-26504816079202425976135373291520.000000`, whose digit run is 32 characters, and **decimal digits
are valid hex digits** - so the pointer-normalising rule ate the float's mantissa. The comparison
now collapses "pointer printed as a float" as one unit *before* any general hex rule, and only in
that exact context, so genuinely computed floats are still compared. It also counts line
*categories* separately, because a filter that hides values must not be able to hide an occurrence.

## Where the remaining 5-8 ms per frame goes

`GETV_GPUTIME=1` (`getv/port/src/ge_gpu_timer.c`) answers this with `GL_TIME_ELAPSED` queries.
Seven consecutive 120-frame reports on Train, unambiguous and stable:

```
GPU timeline 5.87 - 7.30 ms | CPU in swap 0.07 - 0.10 ms
```

**The present does not block.** 0.08 ms. The missing time is entirely GPU-side, so it is not the
compositor stalling our thread and not the driver making us wait in `SwapWindow`.

**But "GPU timeline" is not "GPU busy", and the difference decides what to do about it.**
`GL_TIME_ELAPSED` measures wall time between two markers on the GPU's timeline; time the GPU
spends *waiting* inside that window is included. So this proves the time is spent GPU-side. It
does **not** prove the hardware is saturated.

And it is not saturated, because the figure is **indifferent to resolution**: 5.9-7.3 ms at
1280x960 against 5.0-6.6 ms at 320x240, twelve times fewer pixels. A fill-rate-bound GPU cannot
shrug at that. The per-frame cost is something FIXED, not shading work.

That matters because the obvious next moves are all wrong here. With 311 triangles and 48 draw
calls per frame, optimising geometry, batching or shaders targets work the GPU is demonstrably not
doing. **Anyone continuing this should be chasing per-frame fixed cost** - submission overhead,
buffer orphaning, or the swapchain - not draw calls.

**`flush=48(empty 61)` is not a lead.** 61 "empty flushes" per frame sounds like 61 wasted
state validations; it is not. `gfx_flush` returns immediately when the vertex buffer is empty and
issues no GL call at all. Checked before optimising, and recorded so it is not "found" again.

**There is a real, unfixed `GL_INVALID_OPERATION` -- but it is NOT the per-frame cost.**
Hunted with `GETV_GLDEBUG=1` (`getv/port/src/ge_gl_debug.c`), which installs a synchronous
`KHR_debug` callback. Result:

```
callback ERROR lines: 6
poll hits: 1 -> frame 0, after gfx_run
```

**Six errors, all during frame 0, all inside `gfx_run`.** The poll after `gfx_start_frame` comes
back clean, so they are bounded to that one call on that one frame -- where shaders are first
compiled and textures first uploaded.

**The obvious hypothesis was wrong and is recorded as wrong.** The reason to chase this was that
a driver can drop off a fast path once a context is in an error state, which would be a candidate
for a fixed per-frame cost. It is not: a one-time startup error cannot explain a cost paid every
frame, and the error does not recur after frame 0. **The ~6 ms per frame remains unexplained.**

Still worth fixing as correctness -- nothing else in the tree calls `glGetError`, so six real GL
errors have been raised and ignored for the life of the project. Intel's driver reports the
function as `(null)`, so pinning the exact call means instrumenting inside `gfx_run`
(third-party), which has not been done.

**Drain the GL error queue before checking your own call.** `glGetError` reports and clears ONE
error per call from a queue that persists until read, so a stale error from unrelated code gets
attributed to whatever checks next. The first version of the timer reported "could not allocate
timer queries" on a driver where allocation had succeeded - it was reading somebody else's error,
and the misleading message sent the investigation to the wrong place.

**Never read a query with `GL_QUERY_RESULT` on the frame you issued it.** That blocks until the
GPU finishes, which inserts exactly the stall the timer exists to detect - it would report a busy
GPU whatever the truth, self-fulfillingly. Results are collected several frames late, and only
after `GL_QUERY_RESULT_AVAILABLE` says so; a result that is not ready is skipped, never waited for.

### The decisive bisection: `GETV_NODRAW=1`

Eliminating candidates one at a time is slow and assumes the cause is on the list. Skipping
`gfx_run` entirely asks the question directly - with zero draw calls, zero state changes and zero
uploads, does the GPU timeline still show 6 ms?

| | GPU timeline | wall (600 frames) | fps |
|---|---|---|---|
| normal | 6.92 / 7.11 ms | 6.8 / 7.3 s | ~85 |
| `GETV_NODRAW=1` | **0.00 ms** | 3.9 / 2.6 s | 155-231 |

**It collapses to zero.** So the cost is not the frame machinery - not the swapchain, not the
compositor, not context or present overhead. It is the drawing.

**That gives the number to chase: ~145 microseconds per draw call.** 48 draws costing ~7 ms,
while being indifferent to pixel count *and* to triangle count (311 triangles total). That is
per-draw driver overhead, not shading and not geometry.

**`GETV_NODRAW` is a diagnostic, not a rendering mode** - the screen shows nothing. It
still runs `gfx_start_frame` and `gfx_end_frame`, so the present still happens and
the comparison isolates drawing rather than quietly measuring a different frame shape.

Note the wall time does NOT collapse with it: ~4-6 ms per frame remains with zero GPU work.
That is the rest of the program - game logic, the display-list walk that still runs to build
nothing, and per-frame overhead - and it is a separate question from the GPU 6 ms.

**Shader switches are NOT the cause.** `gfx_pc.c:2854` already guards the switch
(`if (prg != rendering_state.shader_program)`), so the 9 per frame are 9 genuinely distinct
colour combiners with no redundancy to remove. At roughly 135 GL calls per frame for switching,
6 ms would mean ~44 microseconds per call, which is implausible.

### State-change redundancy audit: nothing to remove

48 draw calls for 311 triangles is about 6.5 triangles per draw, and the vertex buffer holds 256 -
so the flushes are not capacity, they are state changes. The obvious hope was redundant ones:
state being re-set to a value it already holds, each costing a flush and a draw.

**There are none.** Every state-change site in `gfx_pc.c` already guards on `rendering_state`:

| line | state | guard |
|---|---|---|
| 2671 | depth test | `!= rendering_state.depth_test` |
| 2678 | depth mask | `!= rendering_state.depth_mask` |
| 2685 | decal mode | `!= rendering_state.decal_mode` |
| 2692 | viewport | `memcmp` against `rendering_state.viewport` |
| 2697 | scissor | `memcmp` against `rendering_state.scissor` |
| 2855 | shader | `!= rendering_state.shader_program` |
| 2862 | alpha blend | `!= rendering_state.alpha_blend` |
| 2880 | texture | `rdp.textures_changed[i]` |
| 2969 | sampler | compares filter and both wrap modes |

The only unguarded flushes are line 3439 (vertex buffer actually full) and 5424 (end of frame),
both legitimate. The counters agree that the cheap causes are already at zero: steady state reports
`ccgen=0` (no combiner generation) and `smp=0` (no sampler changes), with `shsw=9`.

**So the draw count is inherent to the source data**, not a porting inefficiency. The N64 display
list changes render state every few triangles and the port is already collapsing every change it
legitimately can.

**What would reduce it further, and why it has not been done.** Batching across state changes
means reordering draws. On a project whose whole purpose is reproducing the original output, a
change that can alter draw order is a change that can alter what appears on screen - and the
existing verification (`tools/verify_opt.ps1`) compares diagnostics, not pixels, so it would not
catch a regression of that kind. That is a deliberate stop, not an oversight.

### Buffer orphaning: tried, measured, REFUTED

`gfx_opengl_draw_triangles` calls `glBufferData` with a **different size on every draw**. That
looks wrong: orphaning is cheap when a driver can hand back a pooled block of the same size, and a
size that changes each time is supposed to defeat that. At 48 draws per frame the theory was 48
fresh allocations - a cost per **draw**, indifferent to pixels and to triangle count, which is
exactly the unexplained signature.

Tested by adding a constant-size orphan plus `glBufferSubData`, A/B'd on **one binary** with
`GETV_GPUTIME`, interleaved, three pairs:

| | GPU timeline |
|---|---|
| existing (varying size) | 6.49 / 7.34 / 7.79 ms |
| constant orphan + SubData | 7.51 / 7.86 / 8.96 ms |

**Slower in 3 of 3 pairs, by 0.5-1.2 ms/frame.** One call beats two, and this driver suballocates
the varying sizes without trouble. The existing code is correct and now carries a comment saying
so, because it will look like a bug to the next reader too.

The experiment was **removed rather than left behind a flag**. A measurably slower path kept "for
reference" is one somebody eventually switches on.

Note this was A/B'd on a single binary with an environment variable. That is strictly better
than comparing two builds: it removes build-to-build variation, which on this machine is larger
than the effect being looked for. Prefer it whenever a change can be expressed as a runtime branch.

## Things that were tested and are NOT the bottleneck

Recorded so they are not re-tested:

- **Fill rate / resolution.** 1280x960 -> 320x240 is 12x fewer pixels and bought 11%.
- **The frame cap.** `GETV_FPS=0` (uncapped) changed nothing measurable.
- **vsync.** `GETV_VSYNC=0` gained ~13%, with the driver confirming the interval actually changed.
- **The presented frame.** `gfx_end_frame` is 0-1 ms.
- **The texture cache.** Every lookup a hit, zero imports at steady state.

**Read the `DONE start=/run=/end=` line, not the `-> gfx_end_frame (Nms)` line.** The latter is
labelled for the stage it is *about to enter* and reports the time of the stage *before* it. Read
carelessly it blames the buffer swap for `gfx_run`'s cost, and that instrumentation only covers the
first five frames - which are dominated by asset loading and are not representative of anything.

## Environment knobs

| variable | default | what it does |
|---|---|---|
| `GETV_LOGFLUSH` | off | per-line `fflush`. Costs ~7x. On only to chase a crash that loses buffered output. |
| `GETV_LOADTRACE` | off | the ~646 per-asset load diagnostics |
| `GETV_SCISSORTRACE` | off | the scissor rectangle, per change |
| `GETV_VSYNC` | unset | SDL swap interval: `1` blocks, `0` does not, `-1` adaptive |
| `GETV_PROF` | off | per-frame render breakdown. Costs ~25 ms/frame itself; read it as a ratio. |

**Do not redirect `stdout` to a file when measuring or playing.** A flushed line costs ~24 ms on
this machine's storage. That single fact accounted for a 7x difference (17.80 fps redirected vs
124.78 fps discarded) and is why the benchmark harness sends output to `NUL` by default.
