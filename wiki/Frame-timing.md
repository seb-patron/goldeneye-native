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

`framerate = 30` sets a 30Hz simulation with a correct time base: elapsed fields per update
becomes 2, animation and the mission clock advance at the same real rate, and the
frame-quantised systems slow to the cadence they expect. **That is the faithful configuration
and it is one setting.**

What does not exist yet is high-refresh rendering with correct gameplay timing. Simulation and
rendering are still locked together, because tick and draw are interleaved inside `lvlRender`:
`propsTick()` mutates state while `bgLevelRender()` builds the display list from it, in the
same pass. Separating them is real work.

**Until that lands, 60fps is the honest ceiling and anything above it changes the game.**

## Licence note

No code from `Graslu/1964GEPD` or the 1964 lineage is used in this project. Those are GPL-2.0
and quarantined. The credit above is for identifying the problem, which is not a licensable
thing and is the more valuable contribution anyway.
