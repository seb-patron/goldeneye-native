# Goldeneye-Native

GoldenEye 007 built as a native program from the [`n64decomp/007`](https://github.com/n64decomp/007)
decompilation. No emulator, no recompiler. The game's own C, compiled for your machine.

Runs on **macOS**, **Linux** and **Windows** from one source tree.

## New here

- **[Setup](Setup)** - build it, start to finish
- **[Configuration](Configuration)** - every setting, and which ones actually do something
- **[FAQ](FAQ)** - the questions people actually ask

## Playing

- **[The launcher](Launcher)** - level select, rulesets, cheats, video, before you play
- **[Rulesets and horde mode](Rulesets)** - the numbers the game already reads, turned into knobs
- **[Cheats](Cheats)** - the game's own cheat system, by name
- **[Frame rate](Frame-timing)** - why this is not a simple slider in GoldenEye, and what is honest to claim

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

All 27 loadable stages boot, render and exit cleanly. Split-screen multiplayer works with the
radar and all 64 characters. Mouse and keyboard are on by default, and any modern pad works
through SDL2. The launcher, the game's own cheats by name, rulesets, horde mode and Lua mods all
work today.

Co-op into the single-player missions is alpha but real: players spawn, render and move. What is
not adapted is everything the campaign authors around a single Bond, which is objectives, AI and
cutscenes.

True widescreen and 21:9 ultrawide are roadmap items. Changing the window aspect today fits the
4:3 view to the window rather than widening the field of view.

Nothing here contains game data. You supply your own ROM.
