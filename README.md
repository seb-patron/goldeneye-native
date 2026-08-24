# Goldeneye-Native

**GoldenEye 007, compiled as a native program for Windows, macOS and Linux.** Not an
emulator. Not a static recompilation. The game's own C source, from the
[`n64decomp/007`](https://github.com/n64decomp/007) decompilation, built directly for your
machine with a modern platform layer underneath it: SDL2, OpenGL, mouse and keyboard,
arbitrary resolution, and a Fast3D display-list renderer in place of the N64's RCP.

There is no MIPS interpreter and no dynamic recompiler. The binary *is* the game. Every
system in it is ordinary C that can be read, changed and rebuilt, which is the whole
difference: things an emulator can only work around from the outside, this can fix at the
source.

![Silo, from the walkway beside the missile](docs/images/screenshot-01.jpg)

| | |
|---|---|
| **macOS** | Apple Silicon and Intel. Builds and plays. |
| **Linux** | x86-64. Builds and plays. |
| **Windows** | Builds, links and runs; world geometry is being fixed. Active bring-up. |

Same source tree, same features, one `build` script each.

## What you get

- **Every stage.** All 27 loadable missions boot, render and exit cleanly. Twenty-one load
  directly; six are multiplayer-only and need two or more players.
- **Multiplayer.** Split screen, radar, all 64 selectable characters.
- **Keyboard and mouse**, on by default. Mouse look with sensitivity and invert, left button
  fires, right aims, ESC releases the cursor. Gamepads work alongside it, not instead of it.
- **A launcher.** Pick a level, a ruleset, cheats, resolution and field of view before you
  play, instead of editing a config file.
- **Rulesets and horde mode.** Enemy health, damage, accuracy, ammo, player health and
  explosion strength as percentages, with presets. Horde spawns replacements where a guard
  falls and grows the waves as you clear them.
- **Lua mod scripting.** Drop a `mod.lua` in `mods/` and get `onFrame`, `onPlayerSpawn` and
  `onWeaponFire` with a read API into live game state. No rebuild.
- **The cheats the game already had**, by name, from the launcher or a config file.
- **Modern presentation, off by default.** Arbitrary resolution, supersampling, MSAA,
  anisotropic filtering, adjustable field of view, and three texture filters including the
  N64's real three-point sampling.

## On frame timing, and a thank you

The most-cited problem with GoldenEye above its original frame rate is real, and
**[Graslu](https://github.com/Graslu) raised it publicly and was right to.**

The short version: `waitForNextFrame()` is already real-time based, so the clock does not
drift. What breaks is everything the game counts *per iteration* rather than per second, and
122 of the 135 files under `src/game` do per-frame work. Real hardware managed 20 to 30
frames per second, and automatic fire rates, turret delay and guard reaction stepping were
tuned against that. Run the same loop at a locked 60 and they simply happen more often.

**Where this port stands: `framerate = 30` gives a simulation running at the cadence the game
was authored for, with a correct time base.** That is the faithful configuration and it is one
setting away. High-refresh rendering with correct gameplay timing needs the simulation
separated from the draw, which is real work and is not done. The full analysis, including why
this is fixable here and structurally is not in an emulator, is in
[`docs/FRAME_TIMING.md`](docs/FRAME_TIMING.md).

No code from the 1964 or Mouse Injector lineage is used here. Those are GPL-2.0 and
quarantined; see [`docs/REUSE_AUDIT.md`](docs/REUSE_AUDIT.md). The credit above is for
identifying the problem, which is the more valuable contribution and is not a licensable
thing.

## If you came here looking for a GoldenEye emulator

Reasonable place to land, and worth being straight about the difference.

An emulator runs the retail N64 ROM by pretending to be an N64. It works, and for a lot of
people it is the right answer. What it cannot do is change the game, because the game inside
it is compiled MIPS machine code. Frame-rate quirks, control schemes, resolution limits and
anything else baked into the original logic stay baked in. The usual workaround is patching
memory from outside, which is fragile and specific to one build.

This is built from a **decompilation**: the game as readable, editable C. It compiles to a
normal executable for your operating system. There is no N64 being simulated, so there is no
emulation overhead, no core to configure, no plugin to pick, and no ROM loaded at runtime.

Practically, that means things like these are ordinary code changes rather than impossible:

- widescreen and arbitrary resolution, because the projection is a function we can call
- mouse and keyboard, because the input layer is ours
- mod scripting in Lua, because we control the frame loop
- **the frame-rate problem**, which is a real bug in the original scripting and is the one
  thing everybody asks about. See [frame timing](#on-frame-timing-and-a-thank-you).

You still need your own ROM. The build reads it once to extract assets. What you run
afterwards does not touch it.

## Questions people actually ask

**Can I play GoldenEye 007 on a modern PC, Mac or Linux machine without an emulator?**
Yes. That is what this is. You supply your own ROM, the build extracts the assets from it,
and what you run afterwards is a native executable.

**Does it support mouse and keyboard?** Yes, and it is the default. Mouse look with
sensitivity and Y-invert, WASD movement, ESC to release the cursor.

**Does it run at 60fps? Can it run higher?** It renders at 60 by default and the resolution
is arbitrary. Above 60 the game's per-frame systems run faster than they were tuned for; see
the section above. `framerate = 30` is the faithful setting.

**Is there widescreen?** Resolution and aspect are configurable. Whether the widescreen path
widens the field of view or stretches the image has not been measured yet, and that is
recorded honestly in the roadmap rather than claimed either way.

**Can I mod it?** Yes, three ways: Lua scripts in `mods/`, roughly 275 `GETV_*` behaviour
gates, and `goldeneye.cfg`. Texture replacement is on the roadmap, not implemented.

**Is multiplayer online?** No. Split-screen multiplayer works. LAN and online are roadmap
items and are downstream of the frame-timing work.

**Can two people play the single-player missions?** Partly, and it is honest to call it alpha.
Two to four players spawn into a solo mission with its own geometry, props and objectives, and
every viewport renders. They do not move yet. Details below.

**Do I need the ROM?** Yes, your own copy. Nothing in this repository contains game data.
**Is this the Xbox 360 remaster or the cancelled XBLA version?** Neither. Those are separate
codebases. This is the Nintendo 64 game, from its decompilation.

**How is this different from a source port like Ship of Harkinian?** Same idea, different
game. A decompilation is turned into a native program with a modern platform layer. This one
is GoldenEye.

**Does it need Wine, Proton, WSL or a compatibility layer?** No. Each platform gets a real
native binary. Windows uses mingw-w64 with no MSYS2, Cygwin or WSL involved.

**What about split-screen on one PC?** That works, with all 64 characters, the radar and the
full multiplayer setup.

**Does it support gamepads?** Yes, through SDL2, including wireless controllers your OS
already pairs. Keyboard and mouse are the default and both work at once.

**Can it run at 4K?** The internal resolution is arbitrary and supersampling is available.
Whether that is a good idea on the frame-timing front is covered above.

**Is the source available?** All of it. That is the point.


## What works, in detail

All 27 loadable stages boot, render and exit cleanly: 21 load directly, and six are
multiplayer-only and need two or more players. There are no known crashes, hangs, or stages
that fail to start or end properly.

The remaining ten ids carry no data in the ROM: Citadel, which has a background file but no
setup, and nine cut during development. Reaching one now prints which stage it is and what is
missing, then exits. Previously they were worse than useless: two spun at full CPU while
ignoring SIGTERM, three crashed, and one of those varied between hanging, SIGBUS and SIGSEGV
depending on what the heap happened to contain. Loading a multiplayer-only stage on its own
says so and names the flag, rather than reporting it as missing data.

Multiplayer works, including split screen, the radar and all 64 selectable characters. The
pause watch renders all five pages. Saves persist.

Two to four players can also share a single-player mission, split screen, with `coop = 2`.
The mission loads with its own geometry, props and objectives, every player spawns into it,
and the viewports render: Dam draws 5139 triangles at two players and 8412 at four, against
2042 solo.

It is alpha, and the limitation is measured rather than suspected. Under `GETV_STATE` the
players spawn correctly and separated, and none of them moves under scripted input that
carries the solo player 16,930 units on the same level. Player 0 is affected too, which rules
out the obvious explanations; four have been eliminated with measurements and the remaining
search is recorded in the roadmap. Objectives, AI and cutscenes are authored around one Bond
and none of that has been adapted either.

Whether it plays correctly from start to finish is untested, and the frame-timing section
above is a concrete reason to expect it is not yet feature complete.

That gap is measurable rather than merely stated. `tools/playtest.py` drives a stage with
scripted input and reads the machine-readable run state the game emits under `GETV_STATE`:
whether the player reached gameplay at all, how far they moved, how many objectives the
mission has, whether any changed, and whether it completed.

Its current result: **all 21 solo missions reach gameplay and the player moves**, between 408
and 19,584 units over a 900-frame run, with objective counts matching the missions. No
objective advanced, which is expected when the input is "walk forward" and nothing else. So
the port is further than "renders" and well short of "plays": reaching a playable state is
measured across every mission, and completing one is not.

One reading to know about. Cuba is the credits sequence and reports no objectives, so
`objectiveIsAllComplete()` is trivially true there and the tool prints `complete=yes`. It is
an empty set, not a finished mission.

## What this is not

Not an emulator: no MIPS interpreter and no dynamic recompiler. Not static recompilation: no
generated C, no ELF input, no recompiler tooling. Not a fork of the Xbox 360 XBLA remaster. 

This project is not affiliated with, endorsed by, or connected to Nintendo, Rare, MGM, Danjaq or
EON Productions.

## Bring your own ROM

**No ROM, no extracted assets and no game data are distributed with this repository, and none
ever will be.** You need your own legal copy of the NTSC (US) cartridge, dumped to a file. This is
not a licensing formality that a mirror quietly works around: every texture, model, animation,
sound bank and level layout is read out of your dump at build time and emitted as C. Roughly 746
of the translation units this build compiles are generated that way.

| Property | Value |
|---|---|
| Size | 12,582,912 bytes exactly |
| Byte order | Big-endian `z64` |
| Header magic | `80371240` |
| Internal name | `GOLDENEYE` |
| SHA-1 | `abe01e4aeb033b6c0836819f549c791b26cfde83` |

That SHA-1 is the value in `ge007.u.sha1` in the decompilation, so a dump with that hash is
byte-identical to what a correct US build must produce. If your dump has a `.n64` or `.v64`
extension, or a header of `37804012`, it is byte-swapped and must be converted to native
big-endian first.

The build reads the ROM through the decompilation's own extraction scripts, which expect it at the
root of the decomp checkout under a fixed name, `vendor/ge-decomp/baserom.u.z64`. The convention
used here is to keep dumps in `roms/` at the repository root and symlink one into place.
`.gitignore` blocks `roms/`, every `*.z64` / `*.n64` / `*.v64` / `*.elf`, all `getv/build-*`
directories (they contain object files compiled from extracted ROM data), `*.bmp` frame captures,
and `vendor/` and `deps/` themselves. Do not defeat those rules.

Two further things are absent from a fresh clone for related reasons. Fifteen third-party
port-layer files - the Fast3D renderer and the audio mixer, inherited from sm64ex - are fetched
from a pinned upstream commit by `tools/fetch-thirdparty.sh`, because their redistribution terms
are unresolved. The SDL2 2.30.9 source tree is supplied by you in `deps/SDL2-2.30.9`, and built
from source, because a Homebrew running under Rosetta produces an x86_64 SDL2 that cannot link
into an arm64 binary.

## Quick start

Prerequisites: macOS 13+ on Apple silicon, Xcode Command Line Tools, CMake, Python 3, and the
stock `/bin/bash` 3.2. Nothing newer is needed.

```bash
tools/fetch-thirdparty.sh fetch                                       # the fifteen files
git clone https://github.com/n64decomp/007 vendor/ge-decomp           # the game's C source
# then: apply getv/patches/0001-source.patch, place your ROM, generate the asset sources
# from it, and apply getv/patches/0002-assets.patch - docs/SETUP.md sections 2.4 and 3
cd getv && ./build_mac.sh sdl && ./build_mac.sh all && ./build_mac.sh run
```

**[`docs/SETUP.md`](docs/SETUP.md) is the step-by-step guide** - every prerequisite, every command,
the expected output of each one, and a troubleshooting section. Read it if you have not built this
before. The asset-generation step is the one that cannot be shortened: it is a sequence of
extraction and code-generation passes, not the one line above, and skipping any of them produces
a tree that fails to compile or silently misbehaves.

## Screenshots

![Two-player split screen with a radar in each pane](docs/images/screenshot-02.jpg)

![An outdoor stage at night: snow-covered rock, a truck, a glass-walled guard post](docs/images/screenshot-04.jpg)

![Split screen in a concrete interior with weapons and armour on the floor](docs/images/screenshot-05.jpg)

## Build

`getv/build_mac.sh` takes one of `sdl`, `lib`, `port`, `app`, `all`, `run` or `env`.

- `sdl` builds SDL2 2.30.9 for arm64 into `~/.n64tvos/sdl2-mac` - deliberately outside the
  repository, because the repository path contains a space and that has broken header search
  paths here before. Once per machine.
- `lib` compiles the game, assets, audio and platform layer into `build-mac/obj`.
- `port` recompiles only `getv/port/**` and the two harness objects. About 23 s.
- `app` archives the objects into `build-mac/libge.a` and links `build-mac/goldeneye`.
- `all` is `lib` followed by `app`.
- `run` launches the linked binary, forwarding any arguments.
- `env` prints the resolved SDK, SDL prefix, target triple and output paths.

There is no incremental check - every `lib` is a full recompile of all 992 objects. Measured at
21 s wall for `all` on an Apple M1 with warm caches. Compilation is parallel; `GETV_JOBS` caps the
job count and defaults to 6.

Expected output from `./build_mac.sh all`:

```
  mac FAILED: src/tlb_manage.c
mac game: 167 built, 1 failed
mac assets: 746 built, 0 failed
mac audio: 40 built, 0 failed
mac port layer: 23 built, 0 failed
```

**The one failure is expected.** `src/tlb_manage.c` programs the N64's MIPS R4300 translation
lookaside buffer. There is no TLB to program here and nothing links against it, so it is left to
fail rather than being papered over. Seven further N64-hardware and SGI-dev-host files
(`usb.c`, `rmon.c`, `sched.c`, `ramrom.c`, `init.c`, `indy_comms.c`, `indy_commands.c`) are
excluded by name in the build script for the same reason. Any second name in a `mac FAILED:` line
is a real problem.

### Linux

`getv/build_linux.sh` takes the same targets. Needs a C compiler, SDL2 and GL headers:

```
sudo apt install build-essential pkg-config libsdl2-dev libgl1-mesa-dev
CC=gcc ./getv/build_linux.sh all
```

Optional: `tools/fetch_lua.sh` for mod scripting and `tools/fetch_imgui.sh` for the launcher
and dev overlay. Both are optional at every level; without them the build omits the feature
and the entry points compile away.

### Windows

Native, with mingw-w64. No MSYS2, no Cygwin, no WSL. One command installs the whole
toolchain, SDL2, GLEW, Lua and Dear ImGui:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_deps_windows.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File getv\build_windows.ps1 -Target all
```

The build is driven from PowerShell rather than bash on purpose: it needs only the compiler,
not a working POSIX emulation layer. `getv/build_windows.sh` exists for hosts with a healthy
MSYS2.

## Run

```bash
./build-mac/goldeneye
```

Keyboard is bound to controller port 0 by default: WASD to move, arrow keys to look, Space or
Left Ctrl to fire, E or Return to use, Q to aim, Z and X for the shoulder buttons, Tab to pause,
IJKL for the d-pad, F11 for fullscreen. A connected gamepad works alongside it - whichever input
is held wins, so plugging in a pad never degrades the keyboard and vice versa.

## Configuration

On first run, with no configuration file present anywhere, the game writes a commented template to
`~/Library/Application Support/GoldenEye/goldeneye.cfg` and immediately reads it back. This is not
a convenience: several of the port's tuned defaults - `invert_look` being the case in point - only
exist in that template, and a default that lives in a file nobody has generated is not a default.
Edit that file to taste, or regenerate it with `--write-config`.

Save data is separate, and lands in
`~/Library/Application Support/Goldeneye-Native/eeprom.bin`. It is 512 bytes: GoldenEye saves to
the cartridge's serial EEPROM, and writes are atomic.

A few keys worth knowing:

| Key | Values | Default |
|---|---|---|
| `resolution` | `WIDTHxHEIGHT`, `fullscreen`, `native` | `1280x960` |
| `supersample` | `1`, `2` | `1` |
| `filtering` | `point`, `bilinear`, `three-point` | `three-point` |
| `framerate` | `30`, `50`, `60`, `off` | `60` |
| `controls` | any of Rare's eight styles, by number or name | `2.2 galore` |
| `roster` | `8`, `64` | `8` |

Every setting is documented in [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md). The named cheat
system is in [`docs/CHEATS.md`](docs/CHEATS.md). If you want to change the game rather than play
it, start at [`docs/MODDING.md`](docs/MODDING.md); the roughly 250 `GETV_*` environment gates are
the practical extension surface, and each defaults to preserving stock behaviour.

## Layout

```
getv/port/        platform layer: renderer, audio, input, config, saves, paths
getv/patches/     this port's changes to the decompilation and to the fetched sources
getv/build_mac.sh the macOS build
getv/tools/       measurement harnesses
tools/            asset generation, and the third-party fetcher
docs/             documentation
vendor/           decompilation and fetched upstream sources   (untracked)
deps/             SDL2 source                                  (untracked)
roms/             your ROM                                     (untracked)
```

Everything not listed as tracked is fetched, cloned, or derived from your ROM.

## Documentation

| | |
|---|---|
| [`docs/SETUP.md`](docs/SETUP.md) | The build guide. Start here. |
| [`tools/playtest.py`](tools/playtest.py) | Drive a stage and report whether it reached gameplay, moved, and advanced objectives. |
| [`tools/stage_census.sh`](tools/stage_census.sh) | Every named stage id: loads, multiplayer only, or carries no data. |
| [`tools/render_refs.py`](tools/render_refs.py) | Rendering baseline; `check` reports any stage that drifts. |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | What is specific to this project: no game data, the patch workflow, measuring, building one file, provenance. |
| [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) | Every configuration key, which are implemented, and which are reserved. |
| [`docs/CHEATS.md`](docs/CHEATS.md) | The game's own cheat system, exposed by name. |
| [`docs/MODDING.md`](docs/MODDING.md) | How the tree is arranged and where the seams are. |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Current state, known issues, planned work. |
| [`docs/FRAME_TIMING.md`](docs/FRAME_TIMING.md) | The frame-rate question, what causes it, and what a complete fix needs. |
| [`docs/VISION.md`](docs/VISION.md) | The long arc, scored against what the tree does today. |
| [`docs/REUSE_AUDIT.md`](docs/REUSE_AUDIT.md) | What to borrow, what is already borrowed, and what must not be touched. |
| [`docs/PORTING.md`](docs/PORTING.md) | The platform layer, per file. |
| [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) | The fifteen fetched files: what, whence, and why not vendored. |
| [`docs/LICENSING.md`](docs/LICENSING.md) | Where every part came from and which terms are settled. |
| [`docs/ASSET_LOADING.md`](docs/ASSET_LOADING.md) | Level asset loading: map, holes, plan. |
| [`docs/PERFECT_DARK.md`](docs/PERFECT_DARK.md) | What the MIT-licensed Perfect Dark port offers this one. |
| [`getv/port/PROVENANCE.md`](getv/port/PROVENANCE.md) | Per-directory origin of the platform layer. |

## Known issues

- **Gameplay is coupled to the render rate.** GoldenEye scales some systems by elapsed video
  frames and runs the rest once per game update, and a game update is one rendered frame. Only 13
  of the 135 translation units under `src/game` reference `g_GlobalTimerDelta`: recoil, sway,
  breathing, camera. Everything else is per-frame. Enemy automatic fire is the clearest case --
  `chraction.c:6694` increments `firecount[hand]` once per tick and fires on
  `firecount % automaticFiringRate == 0`, so the cadence is a frame count, not a duration.
  On hardware those systems ran at the N64's real 20 to 30 fps. Rendering at a locked 60 runs them
  roughly twice as fast, which shows up as turrets and guards firing too quickly, ammunition
  draining too quickly, and AI state machines stepping faster than they were tuned for. Animation
  looks correct throughout, because animation is in the 13.
  There is a partial mitigation as of now: `framerate=30` also sets `GETV_TICKFIELDS=2`, so each
  update reports two elapsed fields. Game time stays real -- thirty updates a second times two
  fields is sixty fields a second, leaving animation and the mission clock untouched -- while the
  frame-counted systems drop to 30 Hz, close to the cadence the game was tuned for. That setting
  previously capped the renderer without the second half and ran the game at half speed, which was
  a defect rather than the inherent property it was documented as.
  It is a trade, not a fix: 60 renders smoothly and runs gameplay fast, 30 runs gameplay correctly
  and renders less smoothly. Having both at once needs a fixed-rate simulation tick with rendering
  interpolated above it, which is architecture rather than a setting, and is the next substantial
  thing this port needs.

- **Missing HMS MI5 crest on the multiplayer character select.** The same crest renders correctly on
  the file select screen, so the asset and its decode path are sound.
- **Select File background.** Renders flat black; the original has a faint circular watermark
  behind the folders.
- **Multiplayer edge cases.** Score caps are not enforced on the headless path, and `num_shots`
  disagrees with the fire path.
- **Keyboard and mouse.** Mouse look is not supported.

[`docs/ROADMAP.md`](docs/ROADMAP.md) carries the full list and the planned work.

## Provenance and licensing

[`LICENSE`](LICENSE) is MIT, and it covers the original work here: the platform layer under
`getv/port/` excluding the fetched third-party sources, the build scripts, `tools/`, and the
documentation. It does not and cannot cover anything else. [`NOTICE`](NOTICE) states the scope
precisely.

Read [`getv/port/PROVENANCE.md`](getv/port/PROVENANCE.md) before redistributing anything. It
records, per directory, where the platform layer's code came from - in particular that the license
status of `getv/port/fast3d/`, which descends from sm64ex's copy of
`Emill/n64-fast3d-engine`, is **unresolved**. The notice sm64ex ships is the pre-2021 form, whose
second condition bans binary redistribution outright. Do not assume it is MIT.

The decompilation itself has no license file, and its libultra sources carry SGI proprietary
headers. That is upstream's situation, but it is a fact about the base this port is built on.

The ROM, extracted assets, and anything derived from them are never distributable under any
license.

## Credits

Built by Evan King ([@SegfaultEvan](https://github.com/SegfaultEvan)).

The game's C source is the work of the [`n64decomp/007`](https://github.com/n64decomp/007)
decompilation project. The renderer and audio mixer descend from
[sm64ex](https://github.com/sm64pc/sm64ex), which in turn derives its Fast3D implementation from
[Emill/n64-fast3d-engine](https://github.com/Emill/n64-fast3d-engine); neither is redistributed
here. See [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) and [`NOTICE`](NOTICE).

GoldenEye 007 was made by Rare.
