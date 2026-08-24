# Frame timing

The most-cited problem with running GoldenEye above its original frame rate, what actually
causes it, what this port does about it today, and what a complete fix requires.

Credit where it is due: **Graslu** raised this publicly and was right to. It is the first
thing a knowledgeable player checks, and a port that waves it away has not understood its
own subject.

## What people mean by "it breaks above 60Hz"

The usual shorthand is that GoldenEye's animations break at high frame rates. That is the
symptom. The cause is more specific, and the distinction matters because it determines
whether a fix is even possible.

`waitForNextFrame()` (`src/game/frametiming.c`) is the whole story:

```c
do {
    nextFrameTime = ((osGetCount() - copy_of_osgetcount_value_1) + 387937) / 775875;
} while (nextFrameTime < frameDelay);
...
updateFrameCounters(nextFrameTime);
```

`osGetCount()` is a real-time counter, so the loop waits until at least `frameDelay` video
fields of **real time** have elapsed, then reports how many did. Time itself is handled
correctly. The mission clock does not drift.

The problem is everything the game quantises **per iteration** rather than per unit of time.
Automatic weapon fire rates, ammunition counting, turret tracking and delay, guard reaction
stepping, and a good deal of animation all advance once per pass through the loop. **122 of
the 135 files under `src/game` do per-frame work.**

Real hardware managed roughly 20 to 30 frames per second, and those systems were tuned
against that cadence. Run the same loop at a locked 60, and they execute two to three times
as often per second. Guns fire faster. Guards react sooner. The game is not running "fast"
in the sense of a clock running fast; it is running *more often*, and the systems that count
iterations instead of seconds all inherit that.

## Why an emulator cannot fix this and a native port can

This is the part worth being precise about, because it is the difference between the two
approaches rather than a claim about anyone's effort.

An emulator runs the retail ROM. The frame-quantised logic is compiled MIPS code inside it.
You can run the machine faster, which speeds everything up, and you can patch memory from
outside to nudge specific values, which is what the mouse-injector and `1964GEPD` lineage
does. What you cannot do is change how the game counts, because you do not have the source
that does the counting.

**This port is built from the decompilation**, so the counting is ordinary C in front of us.
`frametiming.c` is 140 lines and we can edit it. The fix is available here in a way it
structurally is not to an emulator.

⚠️ That is a statement about where the problem can be solved, not a claim that it is solved.
It is not, yet. See below.

> No code from `Graslu/1964GEPD` or the 1964 lineage is used in this project. Those are
> GPL-2.0 and are quarantined -- see `docs/REUSE_AUDIT.md`. The credit above is for
> identifying the problem, which is not a licensable thing and is the more valuable
> contribution anyway.

## What the port does today

**`GETV_TICKFIELDS=<n>`** (`frametiming.c`) holds the simulation to one update per `n` video
fields, by writing `frameDelay`.

- `1` (default): simulation updates once per field. On a host holding 60fps, every
  per-frame system runs at 60Hz.
- `2`: a 30Hz simulation with a correct time base. Elapsed fields per update becomes 2, so
  `g_GlobalTimerDelta` becomes 2, animation and the mission clock advance at the same real
  rate, and only the frame-quantised systems slow to the cadence they were tuned for.

`framerate = 30` in `goldeneye.cfg` sets this automatically, because the two must be paired:
a 30Hz simulation rendered at 60Hz makes game time run fast.

**So there is a correct configuration available today**, and it is the default pairing when
you ask for 30. What there is not, yet, is high-refresh rendering with correct gameplay
timing, because simulation and rendering are still locked to each other.

## What a complete fix requires

The standard answer, and the one every mature port arrives at: **a fixed simulation tick
with interpolated rendering.** The simulation advances at its authored cadence regardless of
display rate; the renderer draws as often as the display allows and interpolates visual
state between ticks.

In this codebase that decomposes into three pieces of very different size.

### 1. Decouple the tick from the render (tractable)

An accumulator around the game tick: add real elapsed time each pass, run the simulation
step whenever a fixed quantum has accumulated, render every pass regardless. `osGetCount()`
already provides the time base, so nothing new has to be invented for it.

This alone fixes the correctness problem. Fire rates, reactions and animation cadence become
independent of how fast the host draws, which is the whole of the reported bug. What it does
**not** give is smoothness: rendering the same simulation state twice in a row produces
judder, because positions do not change between draws.

### 2. Interpolate the visual state (the real work)

For smooth high-refresh output, anything visible has to be drawn between two simulation
states rather than at one of them: player and camera position and orientation, prop and
character transforms, animation phase.

The difficulty is not the interpolation, it is deciding what may be interpolated. GoldenEye
stores plenty of state that the AI reads as a discrete fact, and any of it blended between
two values is a bug that presents as erratic behaviour rather than as visual judder -- much
harder to diagnose than the problem being solved.

⚠️ **AI opcodes branch on render visibility** (`IFImOnScreen`, `IFMyRoomIsOnScreen`), so the
render path and the AI are not independent in this game. Interpolation touches both.

The Perfect Dark port's experimental high-FPS support carries warnings above roughly 165 FPS.
That is a useful signal about how much of this is genuinely hard rather than merely tedious.

### 3. Audit the frame-quantised systems (open-ended)

Even with 1 and 2, a system that counts iterations is still tuned for one particular tick
rate. Converting fire rates, reload timing, turret delay and reaction stepping to real time
would make the tick rate a free parameter instead of a tuned constant.

This is the piece with no natural end. Every converted system needs checking against retail
behaviour, and `docs/research/GE_RETAIL_BEHAVIOUR.md` exists precisely because "what does
the real game do" is a question with 173 numbered answers rather than one.

## Position

Until step 1 lands, **60fps is the honest ceiling and anything above it changes the game.**
`GETV_TICKFIELDS=2` with `framerate = 30` is the configuration that matches the cadence the
game was authored against.

Step 1 is the next piece of work on this and is worth doing on its own, because it converts
"do not run this above 60" into "run it at whatever your display does, and the simulation is
unaffected". That is the outcome people actually want, and the decompilation is what makes
it reachable.
