# Frame timing

The most-cited problem with GoldenEye above its original frame rate, what actually causes it,
and what this port does about it.

**[Graslu](https://github.com/Graslu) raised this publicly and was right to.** It is the first
thing a knowledgeable player checks.

## What actually happens

`waitForNextFrame()` waits on a real-time counter, so the clock does not drift and the mission
timer stays correct.

The problem is everything the game quantises **per iteration** rather than per second:
automatic fire rates, ammunition counting, turret tracking and delay, guard reaction stepping,
and much of the animation. **122 of the 135 files under `src/game` do per-frame work.**

Real hardware managed roughly 20 to 30 frames per second, and those systems were tuned against
that. Run the same loop at a locked 60 and they execute two to three times as often per second.
Guns fire faster. Guards react sooner. The game is not running fast in the sense of a clock
running fast, it is running *more often*.

## Why a native port can fix this and an emulator cannot

An emulator runs the retail ROM. The frame-quantised logic is compiled MIPS inside it. You can
run the machine faster, which speeds everything up, and you can patch memory from outside,
which is what the mouse-injector and 1964 lineage does. What you cannot do is change how the
game counts, because you do not have the source that does the counting.

This port is built from the decompilation. `frametiming.c` is 140 lines of C and we can edit
it. That is a statement about where the problem can be solved, not a claim that it is solved.

## Where it stands

Running above 60 keeps correct game speed. The route there is worth reading, because the obvious
measurement is the wrong one.

`currentFrameCounter` is the game's own clock in video fields. A correct build advances it by 60
per real second whatever the renderer did. Counting rendered frames, or comparing game state at a
fixed frame number, tells you nothing: game time per rendered frame is preserved by construction
and looks perfect while the game runs thirteen times too fast. Measured on DAM, ten seconds each,
against a real millisecond clock:

| `framerate` | clock | fields/sec (60.0 is correct) | fps |
|---|---|---|---|
| `60` (default) | synthetic | **60.0** | 60 |
| `120` | synthetic | 117.6 | 118 |
| uncapped | synthetic | 811.9 | 812 |
| `120` | real | 60.3 | 60 |
| **`off`** | **real** | **60.5** | **456** |

The synthetic counter advances a fixed amount per call, so a rendered frame *is* a video field
and the world runs as fast as the renderer. The tick divider does not rescue that: it changes how
often the simulation ticks and hands the skipped fields to the tick that runs, so game time per
real second still tracks the render rate. A cap above 60 is therefore refused rather than played
wrong.

`framerate = off` implies the real timebase. A field becomes a unit of real time and
`waitForNextFrame` stops blocking on the field boundary, so the renderer runs ahead while the
world keeps its own clock. A cap never free-runs, which is why `120` on the real clock still
delivers 60 fps.

The cost is reproducibility: elapsed fields become load-dependent and two runs are no longer
frame-for-frame comparable. That is why 60 on the synthetic clock stays the default.

## The divider trap

The tick divider has to be 1 under free-run, and getting this wrong is visible as flicker rather
than as a wrong number.

`gePortSimAlpha()` returns `phase / divider`, so at divider 4 the interpolation alpha cycles
0.25, 0.5, 0.75, 0 every four rendered frames. Under a real clock that phase has no relationship
to how much of a tick has actually elapsed, so the camera is blended against a meaningless
fraction. Elapsed time already gates the simulation there, which makes the divider not just
unnecessary but harmful. Fixed in `0009-freerun-divider.patch`.

## Retrace work is not render work

The same trap as the divider, in a second place, and it cost more to find because the symptom
was a feel rather than a number.

`port_render.c` drives `joyPoll()` and `musicFadeTick()` once per rendered frame. On hardware
both were retrace work, and the retrace was 60Hz. While the renderer was capped at 60 those are
the same statement; uncapped they are not, and a music fade at 472 fps runs about eight times
too fast.

Input is worse than a rate error. `joyPoll` writes one sample per call, the game advances its
read window once per tick, and `joyGetStickX` reads a single sample, `samples[curlast]`
(`joy.c:546`). At 472 fps that is roughly eight writes per read, so where the read lands inside
that window decides how much of the mouse backlog it catches: sometimes most of it, sometimes
almost none. The motion all arrives, in uneven helpings, which is what a jerky mouse is.

Two fixes were tried on the input side before the real one. Draining the backlog only on frames
that a field elapsed concentrates the motion into one sample in eight, and `curlast` then points
at a zeroed sample almost every tick, so the mouse stops responding altogether. Handing the full
backlog to every sample and clearing it on a field is better but still uneven, because the
clear and the read drift against each other.

The fix is upstream of all of that: **poll at the rate the game consumes.** Gating `joyPoll` on
an elapsed field puts writes and reads back at 1:1 exactly as they are at 60, and SDL's relative
mouse state accumulates between calls so a skipped poll loses nothing. Measured, the poll rate
went from about five times the field rate to 1.1 times it, with rendering unchanged at 641 fps.

`gePortAudioFrame()` is deliberately not gated. It feeds a buffer that drains in real time
rather than in game time, and starving it is how you get underruns.

## What is still open

The frame-quantised systems. Automatic fire is converted and time-based by default on both the
player and the AI side (`GETV_TIMEFIRE`), but the rest still advance once per update rather than
per second. What the clock work above changes is that their rate is now correct because time is
correct, rather than because a divider is holding them at a particular cadence.

## Licence note

No code from `Graslu/1964GEPD` or the 1964 lineage is used in this project. Those are GPL-2.0
and quarantined. The credit above is for identifying the problem, which is not a licensable
thing and is the more valuable contribution anyway.

## The two profiles are two different frame rates

GoldenEye+ renders at twice the linear resolution before anything else is switched on, so the
two profiles are not one number. Same machine and stage, 1280x960 on an M1 with vsync
released, three runs each:

| profile | fields/sec (60.0 is correct) | fps |
|---|---|---|
| 97 Console, as shipped | 59.2 | 59 (the 60 cap) |
| 97 Console, uncapped | 61.0 | **486** (449-504) |
| GoldenEye+ | 61.0 | **182** (180-182) |
| GoldenEye+ with an HD pack | 60.8 | 177 (177-179) |

Almost all of the difference is supersampling: 2x means 2560x1920 downsampled, four times the
pixels, before MSAA and anisotropic filtering are counted. A pack costs about another 3%.

The first column matters more than the second. Every uncapped row is within 1.7% of the
correct 60.0, so the world keeps its own speed whether the renderer manages 486 frames or 177.
