# Goldeneye-Native

GoldenEye 007 built as a native program from the [`n64decomp/007`](https://github.com/n64decomp/007)
decompilation. No emulator, no recompiler. The game's own C, compiled for your machine.

Runs on **macOS**, **Linux** and **Windows** from one source tree, with tvOS and iOS harnesses
in bring-up and a native **Metal** renderer alongside the OpenGL one.

## New here

- **[Setup](Setup)** - build it, start to finish
- **[Configuration](Configuration)** - every setting, and which ones actually do something
- **[Platforms and renderers](Platforms)** - where it runs, and OpenGL against Metal
- **[FAQ](FAQ)** - the questions people actually ask

## Playing

- **[The launcher](Launcher)** - level select, rulesets, cheats, video, before you play
- **[Rulesets and horde mode](Rulesets)** - the numbers the game already reads, turned into knobs
- **[Cheats](Cheats)** - the game's own cheat system, by name
- **[Frame rate](Frame-timing)** - why this is not a simple slider in GoldenEye, and what is honest to claim
- **[Widescreen and video](Widescreen)** - aspect, supersampling, filtering, field of view
- **[Bots](Bots)** - the game's own 250-opcode AI, driven for our own ends
- **[Co-op and multiplayer](Multiplayer)** - split screen, co-op, and where network play stands
- **[Crosshair colour](Configuration#the-keys-worth-knowing)** - `crosshair_color`, or the picker in the launcher

## Modding

- **[Lua mods](Lua-mods)** - `onFrame`, `onPlayerSpawn`, `onWeaponFire`, no rebuild
- **[Behaviour gates](Configuration#gates)** - roughly 275 `GETV_*` switches
- **[Modding overview](Modding)** - how the tree is arranged and where the seams are

## Contributing

- **[Roadmap](Roadmap)** - what is being worked on, what is known broken
- **[Vision](Vision)** - the long arc, scored against what exists today
- **[Reuse audit](Reuse-audit)** - what to borrow from the wider N64 port ecosystem, and what is licence-quarantined
- **[Provenance](Provenance)** - where every part came from

## The short version of what works

All 27 loadable stages boot, render and exit cleanly. Multiplayer works with split screen,
radar and all 64 characters. Keyboard and mouse are on by default. Widescreen genuinely widens
the view rather than stretching it, and corrects split screen the same way. Lua mods, a
launcher, rulesets, horde mode and bots all work.

The renderer sustains 881 fps at 1280x960 on an M1 with the cap and vsync released, and the
simulation can be run at its own rate underneath that with the camera interpolated between
ticks. See [Frame timing](Frame-timing) for what that does and does not fix.

Horde mode spawns replacements where a guard falls and grows the waves as they are cleared.
The crosshair takes any colour. Both are settings rather than builds.

Co-op into single-player missions is bring-up quality: per-player spawn and camera are fixed,
the mission is still authored around one Bond. Network play is written but not connected to the
game loop, so no session starts yet.

Nothing here contains game data. You supply your own ROM.
