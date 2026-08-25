# Goldeneye-Native

**GoldenEye 007 as a native executable. Windows, macOS, Linux. No emulator.**

Built from the [`n64decomp/007`](https://github.com/n64decomp/007) decompilation, so the
binary *is* the game. Mouse and keyboard, any resolution, mods in Lua, and the frame-rate
problem actually fixed rather than worked around.

![Silo, from the walkway beside the missile](docs/images/screenshot-01.jpg)

## What you get

- **All 27 stages**, single player and split-screen multiplayer, 64 characters, radar.
- **Co-op in the campaign, bots and an agent API** in alpha. [What that means](#in-alpha-in-the-open).
- **Mouse and keyboard**, on by default. Sensitivity, invert, ESC to release the cursor.
- **Any controller you already own.** DualSense, DualShock 4, Xbox, Switch Pro, 8BitDo.
  Both sticks live, bindings per player.
- **A crouch key.** The original makes you hold aim and push down. This gives it a button.
- **A launcher** for level, ruleset, cheats, resolution and field of view.
- **Rulesets and horde mode.** Health, damage, accuracy and ammo as percentages, with presets.
- **Lua mods.** Drop a `mod.lua` in `mods/`, get `onFrame` and friends, no rebuild.
- **Start any mission with any gun**, dual wielding included.
- **FXAA, CRT, supersampling, MSAA, arbitrary resolution.** All off by default.

![The launcher's Controls page](docs/images/launcher-controls.png)

![FXAA off, left; on, right](docs/images/fxaa-comparison.png)

*FXAA off left, on right, on the PP7 barrel. It is a screen-space approximation, not MSAA, but
on geometry this sparse it is nearly free.*

## Getting it running

Four things: this repository, the [decompilation](https://github.com/n64decomp/007), your own
ROM, and one build command. The ROM is read once to extract assets and never touched again.

```bash
tools/setup.sh          # macOS and Linux
```
```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_deps_windows.ps1
```

That fetches the renderer sources, the decompilation, Lua, Dear ImGui and SDL2, applies the
patches, and stops. Everything it downloads is permissively licensed and comes from its own
upstream. Re-running is safe.

Then supply your ROM and generate the assets:
**[`docs/SETUP.md`](docs/SETUP.md)** has every command with its expected output.

## The frame-rate problem

GoldenEye counts per iteration, not per second. 122 of its 135 game files do per-frame work,
tuned against the 20 to 30fps real hardware managed. Lock the loop at 60 and everything
happens twice as often.

Fixed: the simulation runs on its own divider, the camera interpolates between ticks, and the
frame-counted systems count time now. An FN P90 held for 80 frames fires 39 rounds at every
divider. It used to be 39, 20 and 10. A twelve-level sweep flags nothing.

Above 60Hz set `GETV_REALCLOCK=1` or the world runs fast; the game warns you. That path is
reasoned from the code and not measured, because this was built on a 60Hz panel.

Working, measurements and method in [`docs/FRAME_TIMING.md`](docs/FRAME_TIMING.md). No 1964 or
Mouse Injector code is used; those are GPL-2.0 and quarantined
([`docs/REUSE_AUDIT.md`](docs/REUSE_AUDIT.md)).

**[Graslu](https://github.com/Graslu) raised this publicly and was right to.** Identifying it
is the harder half and it is not a licensable thing, so the credit is worth more than the
code would have been.

## If you came here looking for a GoldenEye emulator

Reasonable place to land. The difference matters.

An emulator runs the retail N64 ROM by pretending to be an N64. It works, and for a lot of
people it is the right answer. What it cannot do is change the game, because the game inside
it is compiled MIPS machine code. Frame-rate quirks, control schemes, resolution limits and
anything else baked into the original logic stay baked in. The usual workaround is patching
memory from outside, which is fragile and specific to one build.

This is built from a **decompilation**: the game as readable, editable C. It compiles to a
normal executable for your operating system. There is no N64 being simulated, so there is no
emulation overhead, no core to configure, no plugin to pick, and no ROM loaded at runtime.

The renderer helps more than it sounds like it should. The N64's RDP is a fixed-function
pipeline with a two-cycle colour combiner and 4KB of TMEM, which is close enough to the GL
most of us grew up on that translating display lists into draw calls is mostly bookkeeping
rather than reinvention. That is why the graphics side of a port like this comes up quickly
and then spends years on the last five per cent.

So these are ordinary code changes rather than impossible:

- widescreen and arbitrary resolution, because the projection is a function we can call
- mouse and keyboard, because the input layer is ours
- mod scripting in Lua, because we control the frame loop
- **the frame-rate problem**, which is a real bug in the original scripting and is the one
  thing everybody asks about. See [frame timing](#the-frame-rate-problem).

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

**Is there widescreen?** Resolution is fully configurable, but measured: **changing the
window aspect does not widen the field of view.** The renderer fits the 4:3 view to whatever
window it is given. Use `fov` for a genuinely wider view; it is the setting that changes what
you can see. Real aspect-aware widescreen is a roadmap item.

**Can I mod it?** Yes, three ways: Lua scripts in `mods/`, roughly 275 `GETV_*` behaviour
gates, and `goldeneye.cfg`. Texture replacement is on the roadmap, not implemented.

**Is multiplayer online?** No. Split-screen multiplayer works. LAN and online are roadmap
items and are downstream of the frame-timing work.

**Can two people play the single-player missions?** Partly. It is alpha.
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

**Can I use a PS5 or Xbox controller?** Yes. Input goes through SDL2's game controller
layer, so a DualSense, DualShock 4, Xbox pad, Switch Pro controller or 8BitDo is recognised
without a mapping file, wired or wireless. Both analogue sticks are live, which the N64
controller could not do.

**Is there a crouch button?** Yes, and the original does not have one. Retail crouch means
holding aim, pushing down, then releasing aim while staying low. Here it is C or LSHIFT, with
V to stand, and the original gesture still works if you prefer it.

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

## Setup: sources and assets

You need a C compiler, Python 3 and CMake. macOS wants Xcode Command Line Tools and works with
the stock `/bin/bash` 3.2; Linux wants SDL2 and GL headers; Windows needs none of that beyond
what `fetch_deps_windows.ps1` installs for you.

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

## Build: compiling

Three scripts, same targets: `lib`, `port`, `app`, `all`, `run`, `env`. `all` is the one you
want. `port` recompiles only the platform layer, which is about 23 seconds against a full
rebuild.

### Windows

Native mingw-w64. No MSYS2, no Cygwin, no WSL. One command installs the toolchain, SDL2, GLEW,
Lua and Dear ImGui:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_deps_windows.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File getv\build_windows.ps1 -Target all
```

PowerShell rather than bash on purpose: it needs a compiler, not a working POSIX layer.
`getv/build_windows.sh` exists for hosts with a healthy MSYS2. `-Target dist` stages a folder
that runs on a machine with no toolchain at all.

### Linux

```bash
sudo apt install build-essential pkg-config libsdl2-dev libgl1-mesa-dev
./getv/build_linux.sh all
```

Uses clang if you have it, gcc otherwise.

### macOS

```bash
./getv/build_mac.sh sdl     # once per machine
./getv/build_mac.sh all
```

`sdl` builds SDL2 2.30.9 into `~/.n64tvos/sdl2-mac`, outside the repository, because the
repository path contains a space and that has broken header search here before.

### Optional everywhere

`tools/fetch_lua.sh` for mod scripting, `tools/fetch_imgui.sh` for the launcher and dev
overlay. Without them the build omits the feature and the entry points compile away.

### What a good build looks like

```
game:        167 built, 1 failed
assets:      746 built, 0 failed
audio:        40 built, 0 failed
port layer:   31 built, 0 failed
```

**The one failure is expected.** `src/tlb_manage.c` programs the R4300's translation lookaside
buffer. There is no TLB here and nothing links against it, so it fails rather than being
papered over. Seven more N64-hardware and SGI-dev-host files are excluded by name for the same
reason. Windows excludes three rather than one. **Any second name in a `FAILED:` line is a real
problem.**

There is no incremental check: every `lib` is a full recompile of all 992 objects, 21 seconds
on an M1 with warm caches. `GETV_JOBS` caps parallelism, default 6.

## Run

```bash
./getv/build-mac/goldeneye            # --launcher opens the launcher first
```

Mouse and keyboard are on by default.

| | |
|---|---|
| Move | `W` `A` `S` `D` |
| Look | mouse, or the arrow keys |
| Fire | left mouse, `Space` or `L Ctrl` |
| Aim | right mouse or `Q` |
| Use | `E` or `F` |
| Crouch / stand | `C` or `L Shift` / `V` |
| Weapon | `R` |
| Pause and watch | `Tab` |
| Release the cursor | `Esc` |

A gamepad works alongside, not instead: whichever input is being held wins, so plugging a pad
in never takes the keyboard away.

## Mods and modes

Everything here is off unless you turn it on, and nothing needs a rebuild.

**CRT.** A real one, not a filter slapped on top. Scanlines, aperture mask, barrel curve and
vignette are four independent terms over the same post-process target FXAA uses, so you tune a
look instead of picking a preset. It ships as a Lua mod in `mods/crt_screen/` that you can read,
change or untick.

**Horde.** Guards respawn where they fell and the waves grow as you clear them. Any level
becomes a survival map, including the ones that were never meant to be.

**Rulesets.** Enemy health, damage, accuracy, ammo, player health and explosion strength, each a
percentage. Presets for classic, hardcore, survival, chaos and horde. Make guards one-shot
snipers, or make yourself one.

**Cheats by name.** The game's own, from the launcher or a config file. No GameShark codes, no
memory patching, because we have the source: `paintball`, `dk_mode`, `infinite_ammo`,
`no_radar`, `enemy_rockets` and the rest, set as flags the game itself reads.

**Start anywhere with anything.** Any mission, any weapon, dual wielded if you like. Dam with
two RC-P90s is a config line.

**Lua.** Drop a `mod.lua` in `mods/` and you get `onFrame`, `onPlayerSpawn` and `onWeaponFire`
with a read API into live game state, plus `ge.postfx{}` to write the CRT parameters every
frame. No rebuild, no recompile.

**Presentation.** FXAA, supersampling, MSAA, anisotropic filtering, arbitrary resolution and
three texture filters including the N64's own three-point.

**Bots and an agent API.** Alpha, and honest about it: the input path and state readout exist
and a first bot runs against them, but it does not play yet. The interesting part is that
GoldenEye already ships a behaviour VM -- 250 AI opcodes with a program counter, branches and
subroutine calls, driving every guard in the campaign -- so bots are assembly over machinery
that already works. See [`docs/BOTS.md`](docs/BOTS.md).

## In alpha, in the open

Three things work enough to be worth talking about and not enough to call finished. They are
in the tree, they are documented, and their limits are stated rather than discovered.

**Co-op.** Two players in the single-player campaign, which the original never had. They spawn
into the mission and do not move yet. Four explanations have been eliminated with
measurements; what is left is a narrow window between the stick being read and movement being
applied. The spawning, the viewports, the split screen and the second player's HUD all work.

**The player API.** Tick-accurate input injection and a state readout, attached through the
game's own demo-playback hook rather than bolted onto the device layer, so it runs on the game
thread once per simulation step. It fills all four pads in one call, which is the single tick
authority multiplayer needs. This is the seam bots, external agents and future netplay all
share, which is why it was built once instead of three times.
[`docs/PLAYER_API.md`](docs/PLAYER_API.md).

**Bots.** A first bot runs against that API. It does not play yet. The reason to be optimistic
is what the decompilation exposed: GoldenEye already carries a behaviour VM. 250 AI opcodes
with a program counter, conditional branches and subroutine calls, driving every guard in the
campaign and exercised by twenty levels of shipped content. Bots are assembly over machinery
that already runs, not a new AI system.
[`docs/BOTS.md`](docs/BOTS.md).

Supporting that, every stage now carries extracted level knowledge: the navigation graph,
objectives resolved to positions from the game's own structures, and 537 nuance entries across
all 20 campaign missions and all 17 arenas.

**LAN and online are not started**, and the research that will shape them is done: not
lockstep, client-server with prediction, for reasons written up with the measurements behind
them.

## Configuration

Most of it is in the launcher. For the rest there is one file, written on first run and read
back immediately:

| | |
|---|---|
| Windows | `%APPDATA%\Goldeneye-Native\goldeneye.cfg` |
| macOS | `~/Library/Application Support/Goldeneye-Native/goldeneye.cfg` |
| Linux | `~/.local/share/Goldeneye-Native/goldeneye.cfg` |

A `goldeneye.cfg` sitting next to the binary wins over that, which is what the Windows `dist`
folder ships. `--config=<path>` or `GETV_CONFIG` override both. `--write-config` regenerates
the template.

The file is generated rather than assumed because some tuned defaults live only in it, and a
default nobody has generated is not a default.

Saves are separate: `eeprom.bin` in the same directory, 512 bytes, written atomically, because
GoldenEye saves to the cartridge's serial EEPROM.

| Key | Values | Default |
|---|---|---|
| `resolution` | `WIDTHxHEIGHT`, `fullscreen`, `native` | `1280x960` |
| `supersample` | `1`, `2` | `1` |
| `filtering` | `point`, `bilinear`, `three-point` | `three-point` |
| `framerate` | `30`, `50`, `60`, `off` | `60` |
| `controls` | any of Rare's eight styles, by number or name | `2.2 galore` |
| `roster` | `8`, `64` | `8` |

Everything else is in [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md), cheats by name in
[`docs/CHEATS.md`](docs/CHEATS.md), and modding in [`docs/MODDING.md`](docs/MODDING.md). Around
275 `GETV_*` gates are the real extension surface; each defaults to stock behaviour.

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

- **Co-op, the player API and bots are alpha**, covered in
  [In alpha, in the open](#in-alpha-in-the-open) above with their exact limits.
- **Above 60Hz, set `GETV_REALCLOCK=1`.** The default clock counts a rendered frame as a video
  field, so a 120Hz display runs the world at double speed without it. The game warns you. The
  fix is reasoned from the code and not measured on real high-refresh hardware.
- **Jungle hit registration changes with the divider.** Bond lands 8 hits at divider 1 and 3
  at divider 2 on the same scripted run. Guard positions advance once per tick, so a moving
  target occupies a coarser set of positions and a shot can pass between them. That is what a
  lower simulation rate means, not a timer bug, and it is not being papered over.
- **Missing HMS MI5 crest on the multiplayer character select.** The same crest renders
  correctly on the file select screen, so the asset and its decode path are sound.
- **Select File background** renders flat black; the original has a faint circular watermark
  behind the folders.
- **Multiplayer edge cases.** Score caps are not enforced on the headless path, and
  `num_shots` disagrees with the fire path.

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

**GoldenEye 007 was made by Rare in 1997.** Everything here is a wrapper around their work.

### The decompilation

[`n64decomp/007`](https://github.com/n64decomp/007), led by **KholdFuzion** with dozens of
contributors over nine years, reaching 100% in August 2026. Without it there is no project
here at all. The mirror of record is
[gitlab.com/kholdfuzion/goldeneye_src](https://gitlab.com/kholdfuzion/goldeneye_src), and
[`kholdfuzion/goldeneye_docs`](https://github.com/kholdfuzion/goldeneye_docs) is the reference
that answers questions the source alone does not.

### Code that ships in this repository

| | |
|---|---|
| [sm64ex](https://github.com/sm64pc/sm64ex) | the Fast3D renderer and audio mixer this port's layer descends from |
| [Emill/n64-fast3d-engine](https://github.com/Emill/n64-fast3d-engine) | the original Fast3D, by **Emill** and **MaikelChan** |
| [SDL](https://github.com/libsdl-org/SDL) | window, input, audio and gamepads on every platform |
| [Dear ImGui](https://github.com/ocornut/imgui) | **Omar Cornut's** immediate-mode UI, behind the launcher and the dev overlay |
| [Lua](https://www.lua.org/) | PUC-Rio's language, which is what makes mods possible without a rebuild |
| [Roboto Condensed](https://github.com/googlefonts/roboto-classic) | The Roboto Project Authors, under the SIL Open Font License 1.1 |

Provenance for every file, line by line, is in
[`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) and [`NOTICE`](NOTICE).

### Work that shaped this without a line of code being taken

Ports are built on each other's hard-won knowledge as much as their source, and several
projects below are licence-quarantined precisely so that nothing was taken. The debt is real
regardless.

- **[Graslu](https://github.com/Graslu)** identified the frame-rate problem publicly. It is
  the single most-cited complaint about the game above 30fps and naming it correctly is the
  harder half of fixing it.
- **[The Perfect Dark PC port](https://github.com/perfect-dark-pc-port/perfect_dark)** is the
  closest sibling to this work, and the more mature one. Its structure showed what a
  decompilation port should look like; its netplay design settled the client-server question
  here; and its simulant model shaped how bots are approached. Read for approach, not copied.
- **Joel Middendorf** and the **1964** emulator (1999-2002), and **`Graslu/1964GEPD`** which
  forked it for GoldenEye and Perfect Dark. GPL-2.0 and quarantined, so what was taken is a
  list of symptoms worth chasing and nothing else.
- **[Ship of Harkinian](https://github.com/HarbourMasters/Shipwright)** and
  **[libultraship](https://github.com/kenix3/libultraship)** proved this shape of port on
  console-class hardware years before this started.
- **GoldenRecomp**, **`cblock85/GoldenEye64Recomp`** and **`chrissotraidis/goldenpad`** took
  different routes at the same problem. All on the do-not-read list here, all worth knowing
  about.
- The wider **N64 decompilation and homebrew community**, whose documentation of the RCP,
  TMEM, F3D microcode and the RDP's combiner is why the renderer came up in weeks rather than
  years.

If your work is in here and is not named, that is an oversight rather than an opinion. Open
an issue and it gets fixed.

### This port

Built by Evan King ([@SegfaultEvan](https://github.com/SegfaultEvan)).
