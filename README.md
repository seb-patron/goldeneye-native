# Goldeneye-Native

**GoldenEye 007 as a native executable. Windows, macOS, Linux. No emulator.**

Rare's 1997 game, built from the [`n64decomp/007`](https://github.com/n64decomp/007)
decompilation into a real binary for your machine. Nothing in here is pretending to be an N64.
Mouse and keyboard work, any resolution works, mods are Lua files you drop in a folder, and the
frame-rate bug that has wrecked every attempt to run this game above 30fps is fixed in the
source instead of patched from outside.

![Silo, from the walkway beside the missile](docs/images/screenshot-01.jpg)

## What you get

- All 27 stages. Single player, split-screen multiplayer, 64 characters, radar.
- Mouse and keyboard, on by default. Sensitivity, Y-invert, ESC to let go of the cursor.
- Whatever controller is already on your desk: DualSense, DualShock 4, Xbox, Switch Pro,
  8BitDo. Both sticks live, bindings per player.
- A crouch key. The original makes you hold aim and push down. This gives it a button.
- A launcher, for level, ruleset, cheats, resolution and field of view.
- Rulesets and horde mode. Enemy health, damage, accuracy and ammo as percentages, with presets.
- Lua mods. Drop a `mod.lua` in `mods/`, get `onFrame` and friends, no rebuild.
- Any mission, any gun, dual wielded if you want it.
- FXAA, CRT, supersampling, MSAA, arbitrary resolution. All off until you turn them on.
- Bots: 18 archetypes compiled to the game's own AI bytecode. Co-op, the agent API and LAN are
  alpha, and I mean alpha — [here is exactly how far each one gets](#whats-half-built).

![The launcher's Controls page](docs/images/launcher-controls.png)

![FXAA off, left; on, right](docs/images/fxaa-comparison.png)

*FXAA off left, on right, on the PP7 barrel. It's a screen-space approximation and not MSAA,
but on geometry this sparse it's nearly free.*

## Getting it running

Four things: this repository, the [decompilation](https://github.com/n64decomp/007), your own
ROM, and one build command. The ROM gets read once, at build time, to pull the assets out.
After that nothing touches it again.

```bash
tools/setup.sh          # macOS and Linux
```
```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_deps_windows.ps1
```

That fetches the renderer sources, the decompilation, Lua, Dear ImGui and SDL2, applies the
patches, and stops. Everything it downloads is permissively licensed and comes from its own
upstream. Run it twice if you want; it's safe.

Then supply your ROM and generate the assets.
**[`docs/SETUP.md`](docs/SETUP.md)** has every command with the output you should see.

## The frame-rate problem

Everybody who has tried to run GoldenEye faster than the cartridge does hits this, so it's
worth being precise about what it actually is.

The game counts iterations, not seconds. 122 of its 135 game files do per-frame work of some
kind, and every bit of it was tuned against the 20 to 30fps real hardware managed. Lock the
loop at 60 and the world doesn't get smoother. It gets faster. Reload timers, recoil, guard
reaction, fire rate — everything that counted frames is now counting twice as many of them.

The fix is three pieces. The simulation runs on its own divider. The camera interpolates
between ticks, so you still get the smooth image you came for. And the frame-counted systems
count time now.

The measurement I trust most: hold an FN P90 down for 80 frames. You get 39 rounds at every
divider. It used to be 39, then 20, then 10. A sweep across twelve levels flags nothing.

Above 60Hz you need `GETV_REALCLOCK=1` or the world runs fast, and the game will tell you so if
you forget. I want to be straight about that path — it's reasoned from the code and never
measured, because I built this on a 60Hz panel and haven't had a 120Hz one to check it against.

Working, measurements and method live in [`docs/FRAME_TIMING.md`](docs/FRAME_TIMING.md). No 1964
or Mouse Injector code is in here; both are GPL-2.0 and quarantined
([`docs/REUSE_AUDIT.md`](docs/REUSE_AUDIT.md)).

**[Graslu](https://github.com/Graslu) worked this out publicly and got there first.** None of
his code is here and none needed to be. Naming the thing was the hard half.

## If you landed here looking for an emulator

Reasonable place to end up. The difference is worth two minutes.

An emulator runs the retail ROM by pretending to be an N64. It works, and for a lot of people
it's the right answer. What it can't do is change the game, because the game inside it is
compiled MIPS machine code. Frame-rate quirks, control schemes, resolution limits, anything
else baked into the original logic — all of it stays baked in. The usual workaround is reaching
in and patching memory from outside, which is fragile and only ever works against one build.

A **decompilation** gives you the game as readable, editable C. It compiles to a normal
executable for your operating system. There's no N64 being simulated, so no emulation overhead,
no core to configure, no plugin to pick, and no ROM loaded at runtime.

The renderer helps more than you'd expect. The N64's RDP is a fixed-function pipeline with a
two-cycle colour combiner and 4KB of TMEM, close enough to the GL most of us grew up on that
turning display lists into draw calls is mostly bookkeeping. Which is why the graphics side of
a port like this comes up in weeks and then spends years on the last five per cent.

So a pile of things that sound impossible turn out to be ordinary code changes:

- widescreen and arbitrary resolution, because the projection is a function we can call
- mouse and keyboard, because the input layer is ours
- Lua mod scripting, because we own the frame loop
- **the frame-rate problem**, a genuine bug in the original scripting and the first thing
  anybody asks about. See [frame timing](#the-frame-rate-problem).

You still need your own ROM. The build reads it once for assets and never again.

## Questions people actually ask

**Can I play GoldenEye 007 on a modern PC, Mac or Linux machine without an emulator?**
Yes. That's the whole idea. You supply your own ROM, the build extracts the assets from it, and
what you run afterwards is a native executable.

**Does it support mouse and keyboard?** Yes, and it's the default. Mouse look with sensitivity
and Y-invert, WASD to move, ESC to let go of the cursor.

**Does it run at 60fps? Can it go higher?** It renders at 60 out of the box and the resolution
is arbitrary. Above 60 the game's per-frame systems run faster than they were tuned for — see
the section above. `framerate = 30` is the faithful setting.

**Is there widescreen?** Resolution is fully configurable, but here's the measured truth:
**changing the window aspect does not widen the field of view.** The renderer fits the 4:3 view
to whatever window you give it. Use `fov` if you want to genuinely see more; that's the setting
that does it. Proper aspect-aware widescreen is still a roadmap item.

**Can I mod it?** Three ways. Lua scripts in `mods/`, roughly 275 `GETV_*` behaviour gates, and
`goldeneye.cfg`. Texture replacement is on the roadmap and not implemented.

**Is multiplayer online?** No. Split-screen works. LAN and online sit downstream of the
frame-timing work; there's a lockstep session in the tree and an honest argument about whether
it can work at all, [below](#whats-half-built).

**Can two people play the single-player missions?** Partly, and it's alpha. Two to four players
spawn into a solo mission with its own geometry, props and objectives, and every viewport
renders. They don't move yet. Details below.

**Do I need the ROM?** Yes, your own copy. Nothing in this repository contains game data.

**Is this the Xbox 360 remaster, or the cancelled XBLA build?** Neither. Those are separate
codebases. This is the Nintendo 64 game, from its decompilation.

**How is this different from a source port like Ship of Harkinian?** Same idea, different game.
A decompilation gets turned into a native program with a modern platform layer. This one is
GoldenEye.

**Does it need Wine, Proton, WSL or a compatibility layer?** No. Every platform gets a real
native binary. Windows uses mingw-w64 with no MSYS2, Cygwin or WSL anywhere near it.

**What about split-screen on one PC?** Works, with all 64 characters, the radar and the full
multiplayer setup.

**Can I use a PS5 or Xbox controller?** Yes. Input goes through SDL2's game controller layer, so
a DualSense, DualShock 4, Xbox pad, Switch Pro controller or 8BitDo is recognised without a
mapping file, wired or wireless. Both analogue sticks are live, which the N64 controller could
never do.

**Is there a crouch button?** Yes, and the original doesn't have one. Retail crouch means
holding aim, pushing down, then releasing aim while staying low. Here it's C or LSHIFT, with V
to stand, and the old gesture still works if you're attached to it.

**Does it support gamepads?** Yes, through SDL2, including wireless pads your OS has already
paired. Keyboard and mouse are the default and both work at once.

**Can it run at 4K?** Internal resolution is arbitrary and supersampling is there. Whether
that's a good idea on the frame-timing front is covered above.

**Is the source available?** All of it. That's the point.

## How far it actually gets

All 27 loadable stages boot, render and exit cleanly. 21 load directly; six are
multiplayer-only and want two or more players. No known crashes, no hangs, nothing that fails
to start or end properly.

Ten more stage ids carry no data in the ROM at all: Citadel, which has a background file but no
setup, and nine cut during development. Reach one now and it prints which stage it is and what
is missing, then exits. They used to be worse than useless — two spun at full CPU while
ignoring SIGTERM, three crashed, and one of those alternated between hanging, SIGBUS and
SIGSEGV depending on what the heap happened to be holding that day. Load a multiplayer-only
stage on its own and it now says so and names the flag.

Multiplayer works: split screen, the radar, all 64 selectable characters. The pause watch
renders all five pages. Saves persist.

Two to four players can also share a single-player mission in split screen with `coop = 2`. The
mission loads with its own geometry, props and objectives, every player spawns into it, and the
viewports render — Dam draws 5139 triangles at two players and 8412 at four, against 2042 solo.

Then they stand there.

That's the alpha, and it's measured, not guessed. Under `GETV_STATE` the players spawn
correctly and separated, and not one of them moves under scripted input that carries the solo
player 16,930 units on the same level. Player 0 is affected too, which kills the obvious
explanations. Four have been eliminated with measurements and the remaining search is written
down in the roadmap. Objectives, AI and cutscenes are all authored around one Bond, and none of
that has been adapted either.

Whether the game plays correctly start to finish is untested, and the frame-timing section
above is a concrete reason to expect that it doesn't yet.

I'd rather measure that gap than describe it. `tools/playtest.py` drives a stage with scripted
input and reads the machine-readable run state the game emits under `GETV_STATE`: whether the
player reached gameplay at all, how far they moved, how many objectives the mission has,
whether any changed, whether it completed.

Where that stands today: **all 21 solo missions reach gameplay and the player moves**, between
408 and 19,584 units over a 900-frame run, with objective counts matching the missions. No
objective advanced, which is what you'd expect when the input is "walk forward" and nothing
else. So the port is well past "renders" and well short of "plays". Reaching a playable state
is measured across every mission. Finishing one is not.

One reading to watch for. Cuba is the credits sequence and reports no objectives, so
`objectiveIsAllComplete()` is trivially true there and the tool prints `complete=yes`. That's
an empty set, not a finished mission.

## What this is not

Not an emulator: no MIPS interpreter, no dynamic recompiler. Not static recompilation: no
generated C, no ELF input, no recompiler tooling. Not a fork of the Xbox 360 XBLA remaster.

This project isn't affiliated with, endorsed by, or connected to Nintendo, Rare, MGM, Danjaq or
EON Productions.

## Bring your own ROM

**No ROM, no extracted assets and no game data ship with this repository, and none ever will.**
You need your own legal copy of the NTSC (US) cartridge, dumped to a file.

This isn't a licensing formality that a mirror could quietly work around. Every texture, model,
animation, sound bank and level layout gets read out of your dump at build time and emitted as
C. Roughly 746 of the translation units this build compiles are generated that way. There is no
version of this project that works without your dump.

| Property | Value |
|---|---|
| Size | 12,582,912 bytes exactly |
| Byte order | Big-endian `z64` |
| Header magic | `80371240` |
| Internal name | `GOLDENEYE` |
| SHA-1 | `abe01e4aeb033b6c0836819f549c791b26cfde83` |

That SHA-1 is the value in `ge007.u.sha1` in the decompilation, so a dump matching it is
byte-identical to what a correct US build has to produce. If yours has a `.n64` or `.v64`
extension, or a header of `37804012`, it's byte-swapped and needs converting to native
big-endian first.

The build reads the ROM through the decompilation's own extraction scripts, which expect it at
the root of the decomp checkout under a fixed name, `vendor/ge-decomp/baserom.u.z64`. I keep
dumps in `roms/` at the repository root and symlink one into place. `.gitignore` blocks
`roms/`, every `*.z64` / `*.n64` / `*.v64` / `*.elf`, all `getv/build-*` directories (they hold
object files compiled from extracted ROM data), `*.bmp` frame captures, and `vendor/` and
`deps/` themselves. Please don't defeat those rules.

Two other things are missing from a fresh clone for related reasons. Fifteen third-party
port-layer files — the Fast3D renderer and the audio mixer, inherited from sm64ex — get fetched
from a pinned upstream commit by `tools/fetch-thirdparty.sh`, because their redistribution
terms are unresolved. And the SDL2 2.30.9 source tree is yours to supply in `deps/SDL2-2.30.9`
and gets built from source, because a Homebrew running under Rosetta produces an x86_64 SDL2
that will not link into an arm64 binary. That one cost me an afternoon.

## Setup: sources and assets

You need a C compiler, Python 3 and CMake. macOS wants Xcode Command Line Tools and is happy
with the stock `/bin/bash` 3.2. Linux wants SDL2 and GL headers. Windows needs none of it
beyond what `fetch_deps_windows.ps1` installs for you.

```bash
tools/fetch-thirdparty.sh fetch                                       # the fifteen files
git clone https://github.com/n64decomp/007 vendor/ge-decomp           # the game's C source
# then: apply getv/patches/0001-source.patch, place your ROM, generate the asset sources
# from it, and apply getv/patches/0002-assets.patch - docs/SETUP.md sections 2.4 and 3
cd getv && ./build_mac.sh sdl && ./build_mac.sh all && ./build_mac.sh run
```

**[`docs/SETUP.md`](docs/SETUP.md) is the real guide** — every prerequisite, every command, the
output each one should produce, and a troubleshooting section. Read it if you haven't built this
before. The asset-generation step is the one you can't shortcut: it's a sequence of extraction
and code-generation passes, not the single line above, and skipping any of them gets you a tree
that either fails to compile or misbehaves quietly.

## Screenshots

![Two-player split screen with a radar in each pane](docs/images/screenshot-02.jpg)

![An outdoor stage at night: snow-covered rock, a truck, a glass-walled guard post](docs/images/screenshot-04.jpg)

![Split screen in a concrete interior with weapons and armour on the floor](docs/images/screenshot-05.jpg)

## Build: compiling

Three scripts, same targets: `lib`, `port`, `app`, `all`, `run`, `env`. `all` is the one you
want. `port` recompiles only the platform layer, about 23 seconds against a full rebuild.

### Windows

Native mingw-w64. No MSYS2, no Cygwin, no WSL. One command installs the toolchain, SDL2, GLEW,
Lua and Dear ImGui:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_deps_windows.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File getv\build_windows.ps1 -Target all
```

PowerShell and not bash, on purpose: the build should need a compiler and not a working POSIX
layer. `getv/build_windows.sh` is there for hosts with a healthy MSYS2. `-Target dist` stages a
folder that runs on a machine with no toolchain at all.

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

`sdl` builds SDL2 2.30.9 into `~/.n64tvos/sdl2-mac`, outside the repository, because my
repository path contains a space and that has broken header search here before.

### Optional everywhere

`tools/fetch_lua.sh` for mod scripting, `tools/fetch_imgui.sh` for the launcher and dev
overlay. Skip them and the build omits the feature; the entry points compile away.

### What a good build looks like

```
game:        167 built, 1 failed
assets:      746 built, 0 failed
audio:        40 built, 0 failed
port layer:   31 built, 0 failed
```

**The one failure is supposed to be there.** `src/tlb_manage.c` programs the R4300's translation
lookaside buffer. There's no TLB here and nothing links against it, so I let it fail instead of
papering over it. Seven more N64-hardware and SGI-dev-host files are excluded by name for the
same reason. Windows excludes three instead of one. **Any second name in a `FAILED:` line is a
real problem.**

There's no incremental check: every `lib` is a full recompile of all 992 objects, 21 seconds on
an M1 with warm caches. `GETV_JOBS` caps parallelism and defaults to 6.

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

A gamepad works alongside the keyboard, not instead of it. Whichever input is being held wins,
so plugging a pad in never takes the keyboard away from you.

## Mods and modes

All of it is off until you turn it on, and none of it needs a rebuild.

**CRT** — a real one, not a filter slapped on top. Scanlines, aperture mask, barrel curve and
vignette are four independent terms over the same post-process target FXAA uses, so you tune a
look instead of picking from a menu of three. It ships as a Lua mod in `mods/crt_screen/` that
you can read, change or untick.

**Horde** — guards respawn where they fell and the waves grow as you clear them. Any level
becomes a survival map, including the ones that were absolutely never meant to be.

**Rulesets** — enemy health, damage, accuracy, ammo, player health and explosion strength, each
a percentage. Presets for classic, hardcore, survival, chaos and horde. Make the guards
one-shot snipers. Or make yourself one.

**Cheats by name** — the game's own, from the launcher or a config file. No GameShark codes and
no memory patching, because we have the source. `paintball`, `dk_mode`, `infinite_ammo`,
`no_radar`, `enemy_rockets` and the rest, set as flags the game itself reads.

**Start anywhere with anything** — any mission, any weapon, dual wielded if you like. Dam with
two RC-P90s is one config line.

**Lua** — drop a `mod.lua` in `mods/` and you get `onFrame`, `onPlayerSpawn` and `onWeaponFire`
with a read API into live game state, plus `ge.postfx{}` to write the CRT parameters every
frame. No rebuild, no recompile.

**Presentation** — FXAA, supersampling, MSAA, anisotropic filtering, arbitrary resolution, and
three texture filters including the N64's own three-point.

**Bots and an agent API** — alpha, and honest about it. The input path and the state readout
exist and a first bot runs against them, but it doesn't play yet. See
[`docs/BOTS.md`](docs/BOTS.md).

## What's half-built

Three things work well enough to be worth talking about and not well enough to call finished.
They're in the tree, they're documented, and their limits are written down here instead of left
for you to discover.

### Co-op

Two players in the single-player campaign, which the original never had. They spawn into the
mission and don't move. Four explanations have been eliminated with measurements; what's left
is a narrow window between the stick being read and movement being applied. The spawning, the
viewports, the split screen and the second player's HUD all work, which is the frustrating part.

### The player API

Tick-accurate input injection and a state readout, attached through the game's own
demo-playback hook instead of bolted onto the device layer, so it runs on the game thread once
per simulation step. It fills all four pads in a single call, which is the single-tick authority
multiplayer needs. Bots, external agents and any future netplay all share this seam, which is
why I built it once and not three times. [`docs/PLAYER_API.md`](docs/PLAYER_API.md).

### Bots

Eighteen archetypes, compiled to bytecode the game's own AI interpreter runs.
`GETV_BOT_AI=hard` spawns one into a level with its list attached.

This works because of something the decompilation exposed and I did not expect: GoldenEye
already carries a behaviour VM. 250 AI opcodes with a program counter, conditional branches and
subroutine calls, driving every guard in the campaign. Our archetypes compile into that same
instruction set — `guard_set_speed_rating`, `if_guard_sees_bond`,
`guard_try_fire_or_aim_at_target` — read from the game's own headers, so an upstream change
breaks the build instead of quietly producing a bot that runs the wrong instruction. All 18 fit
in 596 bytes.

What isn't done: they spawn, and whether they then engage is unverified. A scripted run with a
bot spawned leaves the player on full health, same as without one. They also spawn as
characters and not into a player slot, so scoreboard, respawn and character select all come
with the slot work. [`docs/BOTS.md`](docs/BOTS.md).

Behind all that, every stage now carries extracted level knowledge: the navigation graph,
objectives resolved to positions from the game's own structures, and 537 nuance entries across
all 20 campaign missions and all 17 arenas.

### LAN and online

A lockstep session exists over the same input seam. Only inputs travel, twelve bytes a tick,
with a state fingerprint exchanged once a second so divergence gets caught where it starts.

It's also honestly disputed inside this repository. [`docs/NETPLAY.md`](docs/NETPLAY.md) argues
for lockstep. [`docs/PLAYER_API.md`](docs/PLAYER_API.md) argues against it from measurements —
Streets is verified nondeterministic across processes, and PAL and NTSC clients can never share
a session. I kept both documents and both of them say so. The determinism audit that settles it
hasn't been run.

## Configuration

Most of it is in the launcher. For the rest there's one file, written on first run and read
straight back:

| | |
|---|---|
| Windows | `%APPDATA%\Goldeneye-Native\goldeneye.cfg` |
| macOS | `~/Library/Application Support/Goldeneye-Native/goldeneye.cfg` |
| Linux | `~/.local/share/Goldeneye-Native/goldeneye.cfg` |

A `goldeneye.cfg` sitting next to the binary beats that one, which is what the Windows `dist`
folder ships. `--config=<path>` or `GETV_CONFIG` override both. `--write-config` regenerates the
template.

The file gets generated because some of the tuned defaults live only in it, and a default
nobody has generated isn't a default.

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
[`docs/CHEATS.md`](docs/CHEATS.md), modding in [`docs/MODDING.md`](docs/MODDING.md). The roughly
275 `GETV_*` gates are the real extension surface, and each one defaults to stock behaviour.

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

Anything not listed as tracked is fetched, cloned, or derived from your ROM.

## Documentation

| | |
|---|---|
| [`docs/SETUP.md`](docs/SETUP.md) | The build guide. Start here. |
| [`tools/playtest.py`](tools/playtest.py) | Drive a stage and report whether it reached gameplay, moved, and advanced objectives. |
| [`tools/stage_census.sh`](tools/stage_census.sh) | Every named stage id: loads, multiplayer only, or carries no data. |
| [`tools/render_refs.py`](tools/render_refs.py) | Rendering baseline; `check` reports any stage that drifts. |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | What's specific to this project: no game data, the patch workflow, measuring, building one file, provenance. |
| [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) | Every configuration key, which are implemented, and which are reserved. |
| [`docs/CHEATS.md`](docs/CHEATS.md) | The game's own cheat system, exposed by name. |
| [`docs/MODDING.md`](docs/MODDING.md) | How the tree is arranged and where the seams are. |
| [`docs/BOTS.md`](docs/BOTS.md) | The 18 archetypes, the AI bytecode they compile to, and what is unverified. |
| [`docs/PLAYER_API.md`](docs/PLAYER_API.md) | Input injection and state readout, and the case against lockstep. |
| [`docs/NETPLAY.md`](docs/NETPLAY.md) | The case for lockstep. Read it next to the one above. |
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

- **Co-op, the player API and bots are alpha**, with their exact limits in
  [What's half-built](#whats-half-built) above.
- **Above 60Hz, set `GETV_REALCLOCK=1`.** The default clock counts a rendered frame as a video
  field, so a 120Hz display runs the world at double speed without it. The game warns you. The
  fix is reasoned from the code and not measured on real high-refresh hardware.
- **Jungle hit registration changes with the divider.** Bond lands 8 hits at divider 1 and 3 at
  divider 2 on the same scripted run. Guard positions advance once per tick, so a moving target
  occupies a coarser set of positions and a shot can pass between them. That's what a lower
  simulation rate means. It's not a timer bug and I'm not papering over it.
- **The HMS MI5 crest is missing** from the multiplayer character select. The same crest renders
  correctly on the file select screen, so the asset and its decode path are both fine.
- **Select File background** renders flat black. The original has a faint circular watermark
  behind the folders.
- **Multiplayer edge cases.** Score caps aren't enforced on the headless path, and `num_shots`
  disagrees with the fire path.

[`docs/ROADMAP.md`](docs/ROADMAP.md) has the full list and the planned work.

## Provenance and licensing

[`LICENSE`](LICENSE) is MIT and it covers the original work here: the platform layer under
`getv/port/` excluding the fetched third-party sources, the build scripts, `tools/`, and the
documentation. It does not and cannot cover anything else. [`NOTICE`](NOTICE) states the scope
precisely.

Read [`getv/port/PROVENANCE.md`](getv/port/PROVENANCE.md) before you redistribute anything. It
records, per directory, where the platform layer's code came from — and in particular that the
licence status of `getv/port/fast3d/`, which descends from sm64ex's copy of
`Emill/n64-fast3d-engine`, is **unresolved**. The notice sm64ex ships is the pre-2021 form,
whose second condition bans binary redistribution outright. Don't assume it's MIT.

The decompilation itself has no licence file, and its libultra sources carry SGI proprietary
headers. That's upstream's situation, but it's a fact about the base this port stands on.

The ROM, the extracted assets, and anything derived from them are never distributable under any
licence.

## Credits

**GoldenEye 007 was made by Rare in 1997.** Everything here is a wrapper around their work.

### The decompilation

[`n64decomp/007`](https://github.com/n64decomp/007), led by **KholdFuzion** with dozens of
contributors over nine years, hitting 100% in August 2026. Without it there's no project here
at all. The mirror of record is
[gitlab.com/kholdfuzion/goldeneye_src](https://gitlab.com/kholdfuzion/goldeneye_src), and
[`kholdfuzion/goldeneye_docs`](https://github.com/kholdfuzion/goldeneye_docs) answers the
questions the source alone doesn't.

### Code that ships in this repository

| | |
|---|---|
| [sm64ex](https://github.com/sm64pc/sm64ex) | the Fast3D renderer and audio mixer this port's layer descends from |
| [Emill/n64-fast3d-engine](https://github.com/Emill/n64-fast3d-engine) | the original Fast3D, by **Emill** and **MaikelChan** |
| [SDL](https://github.com/libsdl-org/SDL) | window, input, audio and gamepads on every platform |
| [Dear ImGui](https://github.com/ocornut/imgui) | **Omar Cornut's** immediate-mode UI, behind the launcher and the dev overlay |
| [Lua](https://www.lua.org/) | PUC-Rio's language, which is what makes mods possible without a rebuild |
| [Roboto Condensed](https://github.com/googlefonts/roboto-classic) | The Roboto Project Authors, under the SIL Open Font License 1.1 |

File-by-file provenance is in [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) and
[`NOTICE`](NOTICE).

### Work that shaped this without a line of code being taken

Ports are built on each other's hard-won knowledge as much as their source, and several
projects below are licence-quarantined precisely so that nothing was taken. The debt is real
either way.

- **[Graslu](https://github.com/Graslu)** worked out the frame-rate problem and said so
  publicly. It's the thing people complain about most above 30fps, and he got there first.
- **[The Perfect Dark PC port](https://github.com/perfect-dark-pc-port/perfect_dark)** is the
  closest sibling to this work and the more mature one. Its structure showed me what a
  decompilation port should look like, its netplay design settled the client-server question
  here, and its simulant model shaped how bots are approached. Read for approach, not copied.
- **Joel Middendorf** and the **1964** emulator (1999-2002), and **`Graslu/1964GEPD`**, which
  forked it for GoldenEye and Perfect Dark. GPL-2.0 and quarantined, so what I took is a list of
  symptoms worth chasing and nothing else.
- **[Ship of Harkinian](https://github.com/HarbourMasters/Shipwright)** and
  **[libultraship](https://github.com/kenix3/libultraship)** proved this shape of port on
  console-class hardware years before I started.
- **GoldenRecomp**, **`cblock85/GoldenEye64Recomp`** and **`chrissotraidis/goldenpad`** took
  different routes at the same problem. All on the do-not-read list here, all worth knowing
  about.
- The wider **N64 decompilation and homebrew community**, whose documentation of the RCP, TMEM,
  F3D microcode and the RDP's combiner is why the renderer came up in weeks and not years.

If your work is in here and you're not named, that's an oversight and not an opinion. Open an
issue and I'll fix it.

### This port

Built by Evan King ([@SegfaultEvan](https://github.com/SegfaultEvan)).
