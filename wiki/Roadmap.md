# Roadmap

The live list, with the measurements behind each item, is
[`docs/ROADMAP.md`](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/ROADMAP.md).
This page is the honest summary of where things stand.

## Working

- All 27 loadable stages boot, render and exit cleanly. No known crashes, hangs, or stages that
  fail to start or end.
- Split-screen multiplayer, radar, all 64 characters.
- Widescreen that widens the view, applied per split-screen pane.
- Keyboard and mouse, on by default, alongside gamepads rather than instead of them.
- Launcher, rulesets, horde mode, named cheats, Lua mods.
- Bots driving the game's own AI opcodes.
- Simulation decoupled from rendering, with the camera interpolated between ticks.
- Saves persist. The pause watch renders all five pages.

## Next

**Convert the frame-quantised systems.** Fire rates, reload timing, turret delay and reaction
stepping still count iterations rather than seconds. The structural half is done: the
simulation runs on its own clock, the divider picks itself above 60Hz, and game time is
preserved exactly across dividers. What remains is converting these systems one at a time and
checking each against retail behaviour, which frees the simulation rate rather than pinning it
near 30Hz. See [Frame timing](Frame-timing).

**Connect network play.** The transport, the discovery parser and the launcher page are
written and the input seam exists. Nothing calls into it from the game loop. See
[Multiplayer](Multiplayer).

**Finish co-op.** Per-player spawn and camera are fixed. Objectives, AI and cutscenes are all
authored around a single Bond and none of that has been adapted.

## Known broken

- **Missing HMS MI5 crest** on the multiplayer character select. The same crest renders
  correctly on the file select screen, so the asset and its decode path are both fine and the
  fault is in how character select requests and positions it.
- **Select File background** renders flat black. The original has a faint circular watermark
  behind the folders.
- **Multiplayer edge cases.** Score caps are not enforced on the headless path, and `num_shots`
  disagrees with the fire path.
- **Texture packs untested.** The override path exists, is off by default, and has never been
  run against a real pack.

## How "works" is decided here

Not by looking at it once. `tools/playtest.py` drives a stage with scripted input and reads the
run state the game emits: whether the player reached gameplay, how far they moved, how many
objectives the mission has, whether any changed, whether it completed.

Every stage runs twice, once with the scripted stick and once without, and the verdict is
whether the two disagree. A single run cannot separate "the player walked" from "the level
teleported him to his start pad": the earlier version of this tool reported Dam as 17,232 units
moved with the stick held forward and 17,232 with no input at all, and passed both times.

Its current result: **all 21 solo missions reach gameplay, and 20 of the 21 respond to the
stick**, with the input moving the player between 22 and 5,368 units over a 900-frame run and
objective counts matching the missions. Cuba is the twenty-first and is the credits sequence,
so it has no player to steer and is exempt by name. No objective advanced, which is exactly
what "walk forward and nothing else" should produce.

So the port is well past "renders" and well short of "plays start to finish": reaching a
playable state and responding to a controller are both measured across every mission,
completing one is not.

One reading to know about. Cuba is the credits sequence and reports no objectives, so the
all-complete check is trivially true there and the tool prints complete. That is an empty set,
not a finished mission.
