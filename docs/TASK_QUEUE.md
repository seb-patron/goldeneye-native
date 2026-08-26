# Task queue: Windows tree

Ordered. Each item says what "done" means, because a task without a finish condition gets
argued about instead of finished.

## W1. Vsync toggle in the launcher

`GETV_VSYNC=0` releases the swap interval and exists as of today. Vsync had been hardcoded on
with no way off, which pinned the frame rate to the display: 55 fps uncapped on a 60 Hz panel
against 354-562 with it released.

The launcher is the Windows tree's. Add a checkbox beside the resolution and framerate controls,
defaulting on, with help text saying it raises the frame rate and that gameplay is still coupled
to the render rate.

**Done when** the checkbox writes the config key, the key survives a write-and-read round trip,
and the frame rate visibly changes.

## W2. Benchmark the Surface with vsync off

`bench_windows.ps1` with `GETV_VSYNC=0`, interleaved the way the harness already does.

**Done when** there is a measured number in `docs/PERFORMANCE.md`.

## W3. HD texture packs

The Perfect Dark port loads replacement textures from an `ext_tex` folder beside the game data
(`vendor/pd-ext/port/src/video.c:98`, PR #653). The same approach fits here, because the
decompilation names every texture.

**Done when** one replaced texture renders in game, loaded from a folder, with the original
intact on disk.

## W4. True widescreen, 16:9 and 21:9

The renderer fits the 4:3 view to whatever shape the window is, so a wider window does not show
more of the level.

**Done when** 16:9 and 21:9 show more of the level rather than a stretched view, with the HUD and
the watch placed for the real aspect.

## W5. Render several frames per simulation tick

The field accounting is done: `framerate=30` ticks the simulation at 30 Hz while game time runs
at real speed. This is the other half, and it is what turns a 500 fps renderer into a 500 fps
game.

**Done when** the render rate and the simulation rate can be set independently and a mission plays
correctly at both.

## W6. Wire the bot arbiter

`ge_bot_arbiter.c` is written and tested and is not yet called from `ge_bot_route.c`.

**Done when** the follower takes its heading from the arbiter and the Train waypoint count is
reported, better or worse.

## W7. Netplay determinism audit

The longest-open question: whether two peers stay identical over thousands of ticks. Streets is
verified nondeterministic across processes.

**Done when** there is a number for how long two peers agree, and a statement of what diverges
first.
