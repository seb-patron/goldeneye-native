# Goldeneye-Native

GoldenEye 007 built as a native program from the [`n64decomp/007`](https://github.com/n64decomp/007)
decompilation. No emulator, no recompiler. The game's own C, compiled for your machine.

Runs on **macOS**, **Linux** and **Windows** from one source tree.

## Pages here

- **[FAQ](FAQ)** - the questions people actually ask
- **[Frame rate](Frame-timing)** - why this is not a simple slider in GoldenEye, and what is honest to claim
- **[Rulesets and horde mode](Rulesets)** - the numbers the game already reads, turned into knobs
- **[Lua mods](Lua-mods)** - `onFrame`, `onPlayerSpawn`, `onWeaponFire`, no rebuild

## In the repository

The reference documentation lives with the code so it is reviewed alongside it.

| | |
|---|---|
| [Setup](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/SETUP.md) | Build it, start to finish. |
| [Configuration](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/CONFIGURATION.md) | Every setting, and which ones do something. |
| [Cheats](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/CHEATS.md) | The game's own cheat system, by name. |
| [Modding](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/MODDING.md) | How the tree is arranged and where the seams are. |
| [Roadmap](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/ROADMAP.md) | What is being worked on, and what is known broken. |
| [Vision](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/VISION.md) | The long arc, scored against what exists today. |
| [Reuse audit](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/REUSE_AUDIT.md) | What to borrow from the wider N64 port ecosystem, and what is licence-quarantined. |
| [Licensing](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/LICENSING.md) | Where every part came from and which terms are settled. |
| [Full documentation index](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/README.md) | Everything else. |

## The short version of what works

All 27 loadable stages boot, render and exit cleanly. Multiplayer works with split screen, radar
and all 64 characters. Keyboard and mouse are on by default. Lua mods, a launcher, rulesets and
horde mode all work.

Co-op into the single-player missions is alpha but real: players spawn, render and move. What is
not adapted is everything the campaign authors around a single Bond - objectives, AI and
cutscenes.

Nothing here contains game data. You supply your own ROM.
