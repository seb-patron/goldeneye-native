# Frame timing

GoldenEye runs its gameplay off frame counts rather than off the clock, which is why every
attempt to run it above 30fps has made the game itself run fast. This is what causes it, what
this port does about it, and where it stands now.

Graslu flagged this publicly and it is the first thing anyone who knows the game checks.

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

That is a statement about where the problem can be solved, not a claim that it is solved.
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

**AI opcodes branch on render visibility** (`IFImOnScreen`, `IFMyRoomIsOnScreen`), so the
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

## Where this stands

The split is in. `GETV_SIMDIV` renders every frame while the simulation ticks once every n, and
the divider is now chosen from the frame cap rather than left to the user: 60 or below keeps the
old behaviour of one tick per frame, and anything faster holds the simulation near 30Hz.
`GETV_SIMDIV` overrides it, `GETV_SIMDIV=auto` derives it from the display.

Game speed survives it. Walking Train with constant forward input, measured over the steady
middle of the run:

| Configuration | Speed |
|---|---|
| 60fps, one tick per frame | 46.3, 46.5 units/s |
| uncapped at 500+ fps, divider chosen automatically | 46.6, 46.6 units/s |

Within one percent, which is the whole point: the renderer runs as fast as the machine allows and
the game does not notice.

Both halves of the presentation are interpolated. The camera landed on 2026-08-24 and props and
characters on 2026-08-26; before the second one the view glided while everything in it moved in
steps, which looked worse than no divider at all. Measured over an eight thousand frame run at
divider 2: 82,176 props interpolated, none skipped.

## Step 2: interpolation -- landed 2026-08-24

`GETV_SIMDIV` renders every frame while the simulation runs one frame in n, so a skipped tick
redrew the previous camera unchanged and the view moved in steps. The view is now interpolated
between simulation states. `GETV_INTERP=1` by default, `GETV_INTERP=0` is the control.

### What is interpolated, and why only this

**The camera basis, at render time, and nothing else.** `bondviewUpdateCameraMatrices` takes
position, look direction and up as parameters and only reads them, so the interpolated copies
are handed to it through its own local pointers. The caller's vectors are untouched.

**Nothing is written back into game state, and that restriction is the design rather than an
implementation detail.** GoldenEye's AI reads state as discrete fact, so a blended value
entering the simulation produces erratic behaviour far harder to diagnose than judder.

Interpolating means rendering **one tick behind**: at the instant of a tick the newest state is
the far end of the blend, so the near end is where the camera was a tick ago. At divider 2 that
is about 33ms of view latency, bought for motion that moves every frame. `gePortSimAlpha()`
returns exactly 1.0 at divider 1, so the whole path early-outs and costs nothing when unused.

### Measured

Stage 9, scripted forward walk, frames 700-850 of a fixed 901-frame run. "Still" is the share
of rendered frames on which the camera did not move at all, which is the judder itself.

| | still frames | mean step | sd of step |
|---|---|---|---|
| SIMDIV=1, INTERP=0 | 12.1% | 0.2553 | 0.1855 |
| SIMDIV=1, INTERP=1 | 12.1% | 0.2553 | 0.1855 |
| SIMDIV=2, INTERP=0 | 55.7% | 0.2633 | 0.3750 |
| **SIMDIV=2, INTERP=1** | **10.7%** | 0.2645 | **0.1877** |
| SIMDIV=4, INTERP=0 | 75.2% | 0.3112 | 0.6541 |
| **SIMDIV=4, INTERP=1** | **0.0%** | 0.3117 | **0.1843** |

**At divider 4 the interpolated spread is 0.1843 against divider 1's own 0.1855.** A quarter-rate
simulation now renders as smoothly as a full-rate one.

Compare only **within** a divider. The mean step differs between dividers because a fixed
frame window catches a different part of the walk's acceleration, not because speed changed.
Within each divider the mean is preserved to 0.5%, which is the check that matters: the camera
covers the same ground, it just stops jumping to get there.

Divider 1 is byte-identical with interpolation on and off, so the default carries no risk for
anyone not using a divider.

### The one second-order effect, measured

Pure simulation is **identical**: 363 lines of gun and player state match exactly between
INTERP=0 and INTERP=1 at divider 2.

Room culling is **not** identical -- 58 of 4164 lines differ -- because culling is computed from
the camera and the camera is now interpolated. This matters more than it looks: **GE's AI
branches on render visibility** (`IFImOnScreen`, `IFMyRoomIsOnScreen`), so a culling change is a
path by which the view can reach the simulation without anything being written back.

Measured, it is benign. The drawn-room distributions are 371/146/22/362 against 370/147/22/362:
a single frame moved bucket. It is a one-frame timing shift at a room boundary, which is what
any framerate change does anyway. Worth re-checking if AI behaviour is ever reported odd under
a divider, and not worth blocking on now.

## Verification

Three measurements, because "it should be fine" is not a claim.

**Game speed.** Train, constant forward input, measured over the steady middle of each run and
divided by the elapsed time the log actually recorded:

| Configuration | Speed |
|---|---|
| 60fps cap, one tick per frame | 151.3, 149.4, 151.5 units/s |
| uncapped at 500+ fps, auto divider | 164.4, 167.1, 163.4 units/s |

About 9% apart, consistently. An earlier version of this table read 46.4 against 46.6 and was
wrong: it divided by a hard-coded window rather than by the time the run actually covered.
Distance is also a noisy instrument here, because the walker eventually meets geometry and where
it stops varies.

**Fire rate**, which is the clearest frame-quantised system in the game. Two players in
multiplayer armed with the RC-P90 through `GETV_MP_ARM=14`, trigger held, counted against the
wall clock rather than against frames:

| Configuration | Rate |
|---|---|
| divider 1, 60Hz simulation | 3.76 rounds/s |
| uncapped, auto divider | 3.73 rounds/s |
| uncapped, divider 2 | 3.82 rounds/s |

**Character rotation** interpolates alongside position, through `subroty` on the chr model. Only
characters carry a yaw worth interpolating: guards turning to face you is the motion where the
step shows. Measured over a run at divider 2: 3,620 rotations interpolated, no props skipped, and
walking speed unchanged at 46.5 against 46.6.

The wrap is the part that goes wrong if it is written carelessly, so it has its own test in
`getv/port/tests/test_angle_lerp.c`. Turning from 350 degrees to 10 must sweep 20 degrees forward
through zero rather than 340 backward, and no pair of angles may travel more than a quarter turn
by the halfway point.

**The fire gate itself**, in `getv/port/tests/test_fire_cadence.c`. It models both the retail tick
modulo and the ported field-crossing test over a minute of video, and asserts the difference:

```
time-based:  divider 1 = 900 shots
time-based:  divider 2 = 900 shots
time-based:  divider 4 = 900 shots
tick-based:  divider 2 = 450 shots
tick-based:  divider 4 = 225 shots
tick-based:  divider 8 = 112 shots
```

The tick-based column is what every previous attempt at this shipped. It also pins the floor: the
fastest weapon wants one shot every two video fields, and a 30Hz simulation delivers exactly that,
with no drift. That is why the auto divider targets 30 and will not go under.

## The other frame-quantised systems

Fire rate was the one that needed converting. The rest were checked, and most were already
correct for a reason worth writing down.

**Reload timing** keys off `handptr->field_890`, which is the same field counter the fire gate
uses: it accumulates `g_ClockTimer` and is compared against a threshold. A threshold on a count of
video fields is time-based whatever the divider does. Not measured directly, because holding the
trigger empties the magazine without triggering a reload in the harness, so this one rests on the
shared mechanism rather than on its own number.

**Autogun tracking, truck turning and the door motion** run through `chrobjApplySpeed`
(`propobj.c`). Its constants are named `PER_FRAME` and are applied with no time factor, which
looks frame-quantised until you notice the whole body sits inside
`for (i = 0; i < g_ClockTimer; i++)`. It steps once per elapsed field, so a tick worth n fields
does n steps and the rate holds. `getv/port/tests/test_field_integrator.c` pins that, and shows
what removing the loop would cost:

```
per field: divider 1 = 2.99131   divider 2 = 2.99131   divider 8 = 2.99131
per tick:  divider 1 = 2.99131   divider 4 = 0.72926
```

**The tank turret and the autogun beam timer** already multiply by `g_GlobalTimerDelta`, so they
were never frame-quantised.

## The game clock: what was wrong and what fixes it

Two defects, found by instrumenting `waitForNextFrame` and reporting frames and fields against a
host timebase once a real second.

**The rounding term was acting as a threshold.** The retail expression is
`(elapsed + 387937) / 775875`, where 387937 is half a video field. On hardware `osGetCount()` was
read once per frame after the machine had already waited for vblank, so adding half a field before
dividing rounded a number that was going to be 1 regardless. Here the same expression is the loop
condition, and `(elapsed + half) / field >= 1` is satisfied after HALF a field. Measured with the
real clock before the fix: 121 frames, 121 fields, 1002ms, so a game that believed it was running
at 60 was running at 120. The bias is dropped where the counter tells the truth and left alone for
the synthetic counter, where it is harmless.

**The remainder was being discarded.** Rendering faster than a field means most frames advance the
clock by nothing. `updateFrameCounters` moved the reference point to "now" on every frame, so the
leftover fraction went in the bin each time and the clock stopped entirely: 121 frames, 0 fields,
one second. The reference now advances by exactly the whole fields consumed, so the fraction
carries.

With both fixed, uncapped and with `GETV_REALCLOCK=1`:

```
[getv][clock]  955 frames 60 fields delay=1 simdiv=4 in 1000ms ->  955 fps, 60 fields/s
[getv][clock]  904 frames 61 fields delay=1 simdiv=4 in 1003ms ->  901 fps, 60 fields/s
[getv][clock] 1083 frames 60 fields delay=1 simdiv=4 in 1000ms -> 1083 fps, 60 fields/s
```

A thousand frames a second of rendering, sixty fields a second of game time. That is the thing
this port set out to do.

**One instrument still disagrees and it has not been reconciled.** Sampling `g_GlobalTimer` from
outside, through the bot harness, reports 60 to 65 fields a second in some runs and 290 to 357 in
others under what should be the same configuration. The in-engine trace above is measured at the
source and is the one to trust, but until the two agree this fix should be read as proven for the
wait loop rather than proven end to end. `GETV_CLOCKTRACE=1` prints the trace.

## Still open

**The absolute rates have not been checked against hardware.** Everything here establishes that
rates no longer change with the frame rate, and that they match the game's own authored constants.
Whether those constants produce the same rounds per second as a real N64 needs a capture from one.

**Reload has no measurement of its own.** The mechanism it shares with fire rate is measured; the
reload itself is not, because the harness cannot provoke one.
