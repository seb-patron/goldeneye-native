# Goldeneye-Native

**GoldenEye 007, compiled as a native program for Windows, macOS and Linux. No emulator, no
recompiler, no compatibility layer.**

Rare's game reached 100% [decompilation](https://github.com/n64decomp/007) in August 2026, which
means it is readable C now. This builds that C for your machine and puts a modern platform layer
underneath it: mouse and keyboard, any controller on your desk, whatever resolution you ask for,
a launcher, and mods you drop in a folder.

Nothing here contains game data. You bring your own ROM and the build reads your dump.

![Silo, from the walkway beside the missile](docs/images/screenshot-01.jpg)

## What you get

**Two to four players in the single-player campaign.** GoldenEye never had co-op. This does:
the mission loads with its own geometry, props and objectives, every player spawns in, and they
move. It is alpha, and the limits are spelled out further down.

**A launcher.** Pick the level, the ruleset, the cheats, the resolution and the field of view
before you play, without touching a config file.

**The game's own cheats, by name.** `paintball`, `dk_mode`, `infinite_ammo`, `no_radar`,
`enemy_rockets`, `extra_mp_chars` and the rest, set as flags the game itself reads. No GameShark codes,
no memory patching, because we have the source.

**Mouse and keyboard, on by default.** Sensitivity, Y-invert, ESC to release the cursor. Or use
whatever pad is already plugged in: DualSense, DualShock 4, Xbox, Switch Pro, 8BitDo, both sticks
live, bindings per player.

**All 27 stages, split-screen multiplayer, 64 characters, radar, and the pause watch.**

**Rulesets and horde mode.** Enemy health, damage, accuracy and ammunition as percentages, with
presets for classic, hardcore, survival and chaos. Horde respawns guards where they fell and grows
the waves as you clear them, which turns any level into a survival map, including the ones nobody
designed for it.

**Lua mods, no rebuild.** Drop a `mod.lua` into `mods/`, get `onFrame`, `onPlayerSpawn` and
`onWeaponFire`, plus a read API into live game state. The CRT shader ships as a mod you can read
and change.

**A picture you can actually tune.** FXAA, supersampling, MSAA, anisotropic filtering, arbitrary
internal resolution, and a real CRT treatment with scanlines, aperture mask, barrel curve and
vignette as four independent terms rather than a preset. All off until you turn them on.

**A complete game API.** This is the part nothing else has. Read player state, enemy state and
belief, every prop, door, objective and collectable in the level, and the engine's own navigation
graph. Post input per player, per tick, with frame accuracy. It is enough to play GoldenEye from
outside the game: `tools/play_cli.py` drives a mission from a terminal through the API alone, and
the bots are wiring on top of the same seam. Anything that can read a socket can play this.

**A crouch key.** The original makes you hold aim and push down.

![Two-player split screen, each pane with its own radar](docs/images/screenshot-02.jpg)

![An outdoor stage at night: snow-covered rock, a truck, a glass-walled guard post](docs/images/screenshot-04.jpg)

![The launcher's Controls page](docs/images/launcher-controls.png)

## Quick start

You need this repository, the decompilation, and your own NTSC (US) ROM.

```bash
tools/setup.sh                 # macOS and Linux: fetches deps, applies patches
./getv/build_mac.sh all        # or build_linux.sh
./getv/build-mac/goldeneye --launcher
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_deps_windows.ps1
.\getv\build_windows.ps1 -Target all
```

[`docs/SETUP.md`](docs/SETUP.md) is the real guide: every prerequisite, every command, the output
each should produce, and a troubleshooting section. The asset-generation step is the one you
cannot shortcut.

## If you came here looking for a GoldenEye emulator

You do not need one. An emulator interprets or recompiles N64 machine code and hands you the
console's limits along with the game: fixed internal resolution, controller-shaped input, and
frame timing you cannot touch without breaking the simulation.

This is the game's own C, compiled for your CPU. There is no MIPS interpreter and no dynamic
recompiler anywhere in the tree. That is what makes the rest possible, because a native build can
change things an emulator can only approximate: mouse look reading real deltas, resolution as a
number rather than a scaling factor, a launcher that sets flags the game itself reads, and mods
that run alongside the game instead of patching its memory.

## Questions people actually ask

**Can I play GoldenEye 007 on a modern PC, Mac or Linux machine without an emulator?** Yes. That
is what this is. You supply your own ROM dump and the build reads the assets out of it.

**Does it support mouse and keyboard?** Yes, and it is the default.

**Is there co-op in the single-player missions?** Yes, two to four players in split screen, and
it is alpha. See the state section below.

**How fast does it run?** Uncapped with `GETV_VSYNC=0 GETV_FPS=0`, this machine measures 354 to
562 fps on an M-series Mac and a Surface Pro 3 manages 100 to 138. The renderer is not the
limit.

Gameplay is a different question, and the honest answer is the interesting one. GoldenEye's
timestep is whole video frames, so rendering faster also runs guards, rate of fire and ammunition
drain faster. `framerate=30` already pairs with `GETV_TICKFIELDS=2` to tick the simulation at 30 Hz
while game time runs at real speed, which is most of the accounting problem solved. What remains
is rendering several frames per tick with interpolation between them, and that is the roadmap item
that turns 500 fps of renderer into 500 fps of game.

**Is there widescreen?** Resolution is fully configurable, but changing the window aspect does not
widen the field of view: the renderer fits the 4:3 view to whatever shape the window is. True
aspect-aware widescreen and ultrawide are roadmap items, listed below.

**Is multiplayer online?** Not yet. Split screen works today. A LAN session exists and is alpha.

**Can I mod it?** Three ways: Lua scripts in `mods/`, roughly 275 `GETV_*` behaviour gates, and
the source itself.

**Does it run on Steam Deck, Raspberry Pi, Android or iPhone?** Linux builds run where Linux runs,
so a Deck is a Linux build. An Apple TV target builds from this tree today.

iOS and Android are coming and are not here yet. The groundwork is: SDL2 underneath, a platform
layer already split from the game, an Apple TV target proving the ARM and touch-free path, and a
renderer that already targets GLES. What is missing is the shipping work, not the architecture.
Nothing mobile is distributed and nothing mobile is claimed as working.

**Do I need the ROM?** Yes, your own copy. Nothing in this repository contains game data and
nothing ever will.

**Is this the Xbox 360 remaster or the cancelled XBLA version?** Neither. Those are separate
codebases. This is the N64 game's own C.

**How is this different from a source port like Ship of Harkinian?** Same idea, different game and
a different starting point.

## Where it actually stands

Measurements rather than adjectives.

**All 27 loadable stages boot, render and exit cleanly.** 21 load directly and six are
multiplayer-only. There are no known crashes or hangs. The ten remaining stage ids carry no data
in the ROM; reaching one names the stage and what is missing, then exits.

**Split-screen multiplayer works**, with the radar, all 64 characters and all five watch pages.
Saves persist.

**Co-op works and is alpha.** Players spawn, render and move: measured with a control run, the
driven slot travels 1,779 units with input and 0 without, while the other is unaffected. What is
not adapted is everything the campaign authors around a single Bond, which is objectives, AI and
cutscenes. Two wrong conclusions on the way there are written up in [`docs/COOP.md`](docs/COOP.md).

**Simulation and rendering are separated, which is the thing that has broken every previous
attempt to run this game fast.** The renderer runs free while the simulation holds its authored
cadence, and the camera, props and characters are interpolated between ticks. Fire rate is measured invariant
across the divider and walking speed holds within about 9% from a 60fps cap to 500+ fps. What is
not yet right is the game clock itself, which still tracks the render rate rather than real time.
[`docs/FRAME_TIMING.md`](docs/FRAME_TIMING.md) has the measurements.

**Uncapped, this machine renders at 354 to 562 fps** with `GETV_VSYNC=0 GETV_FPS=0`, and a Surface
Pro 3 manages 100 to 138. [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) has the Windows table.

**Bots are two separate things.** Eighteen archetypes compile into the game's own AI bytecode, 596
bytes for all of them, in the instruction set the campaign already runs. Separately, a
route-following bot drives a real player slot along a real route: on Train it walks ten waypoints
across three carriages and opens doors. Whether the archetypes engage once spawned is unverified.

**LAN play is alpha and disputed in this repository.** A lockstep session exists over the input
seam. [`docs/NETPLAY.md`](docs/NETPLAY.md) argues for lockstep and
[`docs/PLAYER_API.md`](docs/PLAYER_API.md) argues against it from measurements. Streets is verified
nondeterministic across processes. Both documents are kept and both say so.

## Roadmap

The full list is [`docs/ROADMAP.md`](docs/ROADMAP.md). The headline items:

- **True widescreen, 16:9 and 21:9 ultrawide.** Aspect-aware projection rather than fitting the
  4:3 view into a wider window, with the HUD and watch laid out for the real aspect.
- **HD fonts.** The HUD, the watch and the menus draw from font tables the decompilation names,
  so the glyphs are addressable rather than baked into a screenshot. Higher resolution faces are a
  matter of supplying them and letting the existing text path scale.

- **VR.** The groundwork is already here and is the unusual part: the camera is a position and a
  pair of vectors this port already substitutes at render time for interpolation, so injecting a
  per-eye view transform is the same seam. What VR needs beyond that is stereo rendering, a head
  pose source, and a control scheme that does not assume a fixed body direction. Listed because
  the hooks exist, not because it works.

- **Android.** SDL2 underneath, a platform layer already separated from the game, and a GLES path
  in the renderer. The missing pieces are the toolchain, a touch or gamepad input map, and the
  asset pipeline running on device.

- **HD texture packs.** The Perfect Dark port loads replacement textures from an `ext_tex` folder
  beside the game data, and the same approach fits here: the renderer already knows every texture
  by name because the decompilation names them.
- **Online play**, once the determinism audit settles the lockstep question.
- **iOS and Android.** SDL2, a platform layer already separated from the game, an Apple TV target
  that builds today, and a GLES path in the renderer. The architecture is there; the shipping is
  not.
- **Co-op the campaign was never written for**: objectives, AI and cutscenes adapted for more
  than one Bond.
- **A Metal backend** for macOS.

## Bring your own ROM

Your own legal copy of the NTSC (US) cartridge, dumped to a file.

| Property | Value |
|---|---|
| Size | 12,582,912 bytes exactly |
| Byte order | Big-endian `z64` |
| Header magic | `80371240` |
| Internal name | `GOLDENEYE` |
| SHA-1 | `abe01e4aeb033b6c0836819f549c791b26cfde83` |

That SHA-1 is the value in `ge007.u.sha1` in the decompilation, so a dump matching it is
byte-identical to what a correct US build has to produce. A `.n64` or `.v64` extension, or a header
of `37804012`, means byte-swapped: convert to native big-endian first.

The build reads it through the decompilation's own extraction scripts, which want it at
`vendor/ge-decomp/baserom.u.z64`. `.gitignore` blocks `roms/`, every `*.z64` / `*.n64` / `*.v64`,
all `getv/build-*` directories, and `vendor/` and `deps/`. Please do not defeat those rules.

Two other things are missing from a fresh clone for related reasons. Fifteen third-party
port-layer files are fetched from a pinned upstream commit, because their redistribution terms are
unresolved. SDL2 2.30.9 is yours to supply in `deps/SDL2-2.30.9` and gets built from source,
because a Homebrew running under Rosetta produces an x86_64 SDL2 that will not link into an arm64
binary.

## Configuration

Most of it is in the launcher. For the rest there is one file, written on first run and read
straight back:

| Platform | Path |
|---|---|
| Windows | `%APPDATA%\Goldeneye-Native\` |
| macOS | `~/Library/Application Support/Goldeneye-Native/` |
| Linux | `~/.local/share/Goldeneye-Native/` |

A `goldeneye.cfg` beside the binary beats it, `--config=<path>` and `GETV_CONFIG` override both,
and `--write-config` regenerates the template. Saves sit alongside as `eeprom.bin`, 512 bytes,
written atomically, because GoldenEye saves to the cartridge's serial EEPROM.

Every key is in [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md).

## Known issues

- **Gameplay is coupled to the render rate.** Only 13 of the 135 translation units under
  `src/game` scale by elapsed time; the rest advance once per update. At a locked 60 the
  frame-counted systems run about twice as fast as they were tuned for, which shows as guards
  firing quickly and ammunition draining quickly. `framerate=30` pairs with `GETV_TICKFIELDS=2`
  and gets the cadence right at the cost of smoothness. It is a trade, not a fix.
- **Window aspect does not widen the field of view.** The 4:3 view is fitted to the window.
- **Missing HMS MI5 crest on the multiplayer character select.** The same crest renders correctly
  on the file select screen, so the asset and its decode path are sound.
- **The Select File background renders flat black**, where the original has a faint circular
  watermark behind the folders.
- **Multiplayer edge cases.** Score caps are not enforced on the headless path, and `num_shots`
  disagrees with the fire path.

[`docs/ROADMAP.md`](docs/ROADMAP.md) carries the full list.

## Documentation

Start at [`docs/README.md`](docs/README.md), which indexes all thirty documents.
[`docs/SETUP.md`](docs/SETUP.md) is the build guide,
[`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) is every setting, and
[`docs/MODDING.md`](docs/MODDING.md) is where the seams are.

The [`docs/research/`](docs/research/) folder holds ten documents on the N64 hardware, GoldenEye's
own systems and the wider decomp ecosystem. Every claim in them is tagged VERIFIED, CONTESTED or
FOLKLORE, and each carries a note on what could not be established.

## Provenance and licensing

[`LICENSE`](LICENSE) is MIT and covers the original work here: the platform layer under
`getv/port/` excluding the fetched third-party sources, the build scripts, `tools/`, and the
documentation. It does not and cannot cover anything else. [`NOTICE`](NOTICE) states the scope.

Read [`getv/port/PROVENANCE.md`](getv/port/PROVENANCE.md) before redistributing anything. It
records, per directory, where the platform layer came from, and in particular that the license
status of `getv/port/fast3d/`, which descends from sm64ex's copy of `Emill/n64-fast3d-engine`, is
**unresolved**. The notice sm64ex ships is the pre-2021 form, whose second condition bans binary
redistribution outright. Do not assume it is MIT.

The decompilation itself has no license file, and its libultra sources carry SGI proprietary
headers. That is upstream's situation, and it is a fact about the base this port builds on.

The ROM, extracted assets, and anything derived from them are never distributable.

## Credits

Built by Evan King ([@SegfaultEvan](https://github.com/SegfaultEvan)).

The game's C source is the work of the [`n64decomp/007`](https://github.com/n64decomp/007)
decompilation project. The renderer and audio mixer descend from
[sm64ex](https://github.com/sm64pc/sm64ex), which derives its Fast3D implementation from
[Emill/n64-fast3d-engine](https://github.com/Emill/n64-fast3d-engine); neither is redistributed
here. [Graslu](https://github.com/Graslu) raised the frame-timing problem publicly and was right
to. See [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) and [`NOTICE`](NOTICE).

GoldenEye 007 was made by Rare.

---

This project is not affiliated with, endorsed by, or connected to Nintendo, Rare, MGM, Danjaq or
EON Productions. Not an emulator: no MIPS interpreter and no dynamic recompiler. Not static
recompilation. Not a fork of the Xbox 360 remaster.
