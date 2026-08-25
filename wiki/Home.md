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
- **[Player API](Player-API)** - tick-accurate input injection and state readout, the one seam
  bots, external AI and future netplay all share
- **[Behaviour gates](Configuration#gates)** - roughly 275 `GETV_*` switches
- **[Modding overview](Modding)** - how the tree is arranged and where the seams are

## Contributing

- **[Roadmap](Roadmap)** - what is being worked on, what is known broken
- **[Vision](Vision)** - the long arc, scored against what exists today
- **[Reuse audit](Reuse-audit)** - what to borrow from the wider N64 port ecosystem, and what is licence-quarantined
- **[Provenance](Provenance)** - where every part came from

## The short version of what works

All 27 loadable stages boot, render and exit cleanly on **Windows, macOS and Linux** from one
source tree. Multiplayer works with split screen, radar and all 64 characters. Keyboard and
mouse are on by default, with a crouch key the original never had. Lua mods, a launcher,
per-player bindings, rulesets and horde mode all work. FXAA and a CRT mod run through a shared
post-process pass.

The frame-timing problem is fixed rather than mitigated: the simulation runs on its own
divider, the camera interpolates between ticks, and the systems that counted frames now count
time. `GETV_SIMDIV=auto` picks the divider from your refresh rate.

Two things are alpha and labelled as such. Co-op players spawn into single-player missions and
do not move. Bots have a working input API and a first consumer, but do not yet play.

Nothing here contains game data. You supply your own ROM.
