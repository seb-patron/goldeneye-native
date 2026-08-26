# Goldeneye-Native

**GoldenEye 007 as a native executable. Windows, macOS, Linux. No emulator.**

The [decompilation](https://github.com/n64decomp/007) reached 100% in August 2026, which means
Rare's game is readable C now. This compiles it, with a modern platform layer underneath. There
is no N64 being simulated anywhere: mouse and keyboard work, the resolution is whatever you ask
for, mods are Lua files you drop in a folder, and the frame-rate bug that has broken every
attempt to run this game above 30fps is fixed at the source instead of patched from outside.

It is not finished. The parts that aren't are [marked as such](#whats-half-built), with
measurements rather than adjectives.

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
  alpha, and below is exactly how far each one gets.

## Getting it running

You need this repository, the decompilation, and **your own copy of the NTSC (US) ROM**. Nothing
here contains game data and nothing ever will: every texture, model, animation, sound bank and
level layout is read out of your dump at build time and emitted as C. Roughly 746 of the
translation units this build compiles are generated that way. Details and the expected SHA-1 are
in [the ROM section](#the-rom).

```bash
tools/setup.sh          # macOS and Linux
```
```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_deps_windows.ps1
```

That fetches the renderer sources, the decompilation, Lua, Dear ImGui and SDL2, applies the
patches, and stops. Everything it downloads is permissively licensed and comes from its own
upstream. Run it twice if you want; it's safe.

Then supply your ROM, generate the assets, and build:

```bash
./getv/build_mac.sh all        # or build_linux.sh, or build_windows.ps1 -Target all
./getv/build-mac/goldeneye     # --launcher opens the launcher first
```

**[`docs/SETUP.md`](docs/SETUP.md) is the real guide.** Every prerequisite, every command,
the output each one should produce, and a troubleshooting section. Read it if you haven't
built this before. The asset-generation step is the one you can't shortcut, and skipping any
part of it gets you a tree that either fails to compile or misbehaves quietly.

One thing that will look wrong and isn't: `src/tlb_manage.c` fails to build, every time, on
purpose. It programs the R4300's translation lookaside buffer, there's no TLB here, and nothing
links against it, so I let it fail instead of papering over it. Any *second* name in a `FAILED:`
line is a real problem.

## The frame-rate problem

Everybody who has tried to run GoldenEye faster than the cartridge does hits this, so it's worth
being precise about what it actually is.

The game counts iterations, not seconds. 122 of its 135 game files do per-frame work of some
kind, and all of it was tuned against the 20 to 30fps real hardware managed. Lock the loop at 60
and the world doesn't get smoother. It gets faster. Reload timers, recoil, guard reaction, fire
rate. Everything that counted frames is now counting twice as many of them.

The fix is three pieces. The simulation runs on its own divider. The camera interpolates between
ticks, so you still get the smooth image you came for. And the frame-counted systems count time.

The measurement I trust most: hold an FN P90 down for 80 frames. You get 39 rounds at every
divider. It used to be 39, then 20, then 10. A sweep across twelve levels flags nothing.

Above 60Hz you need `GETV_REALCLOCK=1` or the world runs fast, and the game will tell you so if
you forget. Be straight about that path though: it's reasoned from the code and never measured,
because I built this on a 60Hz panel and haven't had a 120Hz one to check it against.

Working, measurements and method: [`docs/FRAME_TIMING.md`](docs/FRAME_TIMING.md). No 1964 or
Mouse Injector code is in here; both are GPL-2.0 and quarantined
([`docs/REUSE_AUDIT.md`](docs/REUSE_AUDIT.md)).

**[Graslu](https://github.com/Graslu) worked this out publicly and got there first.** None of his
code is here and none needed to be. Naming the thing was the hard half.

## Why this isn't an emulator

An emulator runs the retail ROM by pretending to be an N64. It works, and for a lot of people
it's the right answer. What it can't do is change the game, because the game inside it is
compiled MIPS machine code. Frame-rate quirks, control schemes, resolution limits, anything else
baked into the original logic: all of it stays baked in. The usual workaround is reaching in
and patching memory from outside, which is fragile and only ever works against one build.

A decompilation gives you the game as editable C, compiled to a normal executable. No core to
configure, no plugin to pick, no ROM loaded at runtime, no emulation overhead.

The renderer helps more than you'd expect. The N64's RDP is a fixed-function pipeline with a
two-cycle colour combiner and 4KB of TMEM, close enough to the GL most of us grew up on that
turning display lists into draw calls is mostly bookkeeping. Which is why the graphics side of a
port like this comes up in weeks and then spends years on the last five per cent.

So things that sound impossible turn out to be ordinary code changes: widescreen, because the
projection is a function we can call. Mouse and keyboard, because the input layer is ours. Lua
mods, because we own the frame loop.

More in [`docs/FAQ.md`](docs/FAQ.md).

## Where it actually is

All 27 loadable stages boot, render and exit cleanly. 21 load directly, six are
multiplayer-only. No known crashes, no hangs, nothing that fails to start or end properly. Ten
further stage ids carry no data in the ROM at all (Citadel, plus nine cut during development);
reach one and it names the stage and what's missing, then exits.

Multiplayer works: split screen, radar, all 64 characters. The pause watch renders all five
pages. Saves persist.

I'd rather measure the rest than describe it. `tools/playtest.py` drives a stage with scripted
input and reads the run state the game emits under `GETV_STATE`: whether the player reached
gameplay, how far they moved, how many objectives exist, whether any changed.

Where that stands: **all 21 solo missions reach gameplay and the player moves**, between 408 and
19,584 units over a 900-frame run, with objective counts matching the missions. No objective
advanced, which is what you'd expect when the input is "walk forward" and nothing else. So the
port is well past *renders* and well short of *plays*. Reaching a playable state is measured
across every mission. Finishing one is not.

One reading to watch for: Cuba is the credits sequence and reports no objectives, so
`objectiveIsAllComplete()` is trivially true there and the tool prints `complete=yes`. That's an
empty set, not a finished mission.

## What's half-built

Three things work well enough to be worth talking about and not well enough to call finished.
Their limits are written down here instead of left for you to discover.

**Co-op.** Two to four players in the single-player campaign, which the original never had. The
mission loads with its own geometry, props and objectives, every player spawns in, the viewports
render, and they move: measured with a control, player 1 travels 1,779 units with input and 0
without, while player 0 is unaffected. Dam draws 5139 triangles at two players and 8412 at four,
against 2042 solo.

What is not adapted is everything authored around a single Bond — objectives, AI and cutscenes.
It took two wrong baselines to establish that the movement itself was fine, both recorded in
[`docs/COOP.md`](docs/COOP.md), the second being a position readout that reported a field the
engine never updates.

**The player API.** Tick-accurate input injection and a state readout, attached through the
game's own demo-playback hook instead of bolted onto the device layer, so it runs on the game
thread once per simulation step. It fills all four pads in a single call, which is the
single-tick authority multiplayer needs. Bots, external agents and any future netplay all share
this seam, which is why I built it once and not three times.
[`docs/PLAYER_API.md`](docs/PLAYER_API.md).

**Bots.** Eighteen archetypes, compiled to bytecode the game's own AI interpreter runs.
`GETV_BOT_AI=hard` spawns one into a level with its list attached.

This works because of something the decompilation exposed that I didn't expect: GoldenEye already
carries a behaviour VM. 250 AI opcodes with a program counter, conditional branches and
subroutine calls, driving every guard in the campaign. Our archetypes compile into that same
instruction set (`guard_set_speed_rating`, `if_guard_sees_bond`,
`guard_try_fire_or_aim_at_target`), read from the game's own headers, so an upstream change breaks
the build instead of quietly producing a bot that runs the wrong instruction. All 18 fit in 596
bytes.

What isn't done: they spawn, and whether they then engage is unverified. A scripted run with a
bot spawned leaves the player on full health, same as without one. They also spawn as characters
rather than into a player slot, so scoreboard, respawn and character select come with the slot
work. [`docs/BOTS.md`](docs/BOTS.md).

**Route-following bots.** A separate thing from the bytecode archetypes above: a bot that drives a
real player slot along a real route, reading the world through the same APIs an external agent
would use. On Train it walks 11 waypoints across three carriages, opens doors, and stops at a
locked one it has no key for — which is the correct behaviour, since a guard is carrying it.

Two findings from that work are worth more than the bot is. The game already ships a pathfinder:
`padhalllv.c` holds a two-level waypoint graph the guards have always routed over — 104 waypoints,
206 links and 6 groups on Train — and the whole tile-graph reconstruction beside it was
unnecessary. And doors are opened by asking them (`doorsChooseSwingDirection`, `doorActivate`,
exactly as the guards do), not by simulating the player's action button, which additionally
requires the door to be on screen and so never worked for a bot in the dark.

**LAN and online.** A lockstep session exists over the same input seam: only inputs travel,
twelve bytes a tick, with a state fingerprint exchanged once a second so divergence gets caught
where it starts. It's also honestly disputed in this repository.
[`docs/NETPLAY.md`](docs/NETPLAY.md) argues for lockstep;
[`docs/PLAYER_API.md`](docs/PLAYER_API.md) argues against it from measurements. Streets is
verified nondeterministic across processes, and PAL and NTSC clients can never share a session. I
kept both documents and both say so. The audit that settles it hasn't been run.

![Two-player split screen with a radar in each pane](docs/images/screenshot-02.jpg)

![An outdoor stage at night: snow-covered rock, a truck, a glass-walled guard post](docs/images/screenshot-04.jpg)

![The launcher's Controls page](docs/images/launcher-controls.png)

## Mods and modes

All of it is off until you turn it on, and none of it needs a rebuild.

**CRT.** A real one, not a filter slapped on top. Scanlines, aperture mask, barrel curve and
vignette are four independent terms over the same post-process target FXAA uses, so you tune a
look instead of picking from a menu of three. It ships as a Lua mod in `mods/crt_screen/` that
you can read, change or untick.

**Horde.** Guards respawn where they fell and the waves grow as you clear them. Any level
becomes a survival map, including the ones that were absolutely never meant to be.

**Rulesets.** Enemy health, damage, accuracy, ammo, player health and explosion strength, each a
percentage, with presets for classic, hardcore, survival, chaos and horde. Make the guards
one-shot snipers. Or make yourself one.

**Cheats by name.** The game's own, from the launcher or a config file. No GameShark codes and
no memory patching, because we have the source. `paintball`, `dk_mode`, `infinite_ammo`,
`no_radar`, `enemy_rockets` and the rest, set as flags the game itself reads.

**Lua.** `onFrame`, `onPlayerSpawn` and `onWeaponFire`, with a read API into live game state and
`ge.postfx{}` to write the CRT parameters every frame.

Start at [`docs/MODDING.md`](docs/MODDING.md). The roughly 275 `GETV_*` gates are the real
extension surface, and each one defaults to stock behaviour.

## Configuration

Most of it is in the launcher. For the rest there's one file, written on first run and read
straight back. `%APPDATA%\Goldeneye-Native\` on Windows,
`~/Library/Application Support/Goldeneye-Native/` on macOS, `~/.local/share/Goldeneye-Native/`
on Linux. A `goldeneye.cfg` next to the binary beats it, `--config=<path>` and `GETV_CONFIG`
override both, and `--write-config` regenerates the template.

It's generated rather than assumed because some tuned defaults live only in it, and a default
nobody has generated isn't a default.

Saves sit beside it as `eeprom.bin`, 512 bytes, written atomically, because GoldenEye saves to
the cartridge's serial EEPROM.

Every key is in [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md).

## The ROM

You need your own legal copy of the NTSC (US) cartridge, dumped to a file.

| Property | Value |
|---|---|
| Size | 12,582,912 bytes exactly |
| Byte order | Big-endian `z64` |
| Header magic | `80371240` |
| Internal name | `GOLDENEYE` |
| SHA-1 | `abe01e4aeb033b6c0836819f549c791b26cfde83` |

That SHA-1 is the value in `ge007.u.sha1` in the decompilation, so a dump matching it is
byte-identical to what a correct US build has to produce. If yours has a `.n64` or `.v64`
extension, or a header of `37804012`, it's byte-swapped and needs converting to native big-endian
first.

The build reads it through the decompilation's own extraction scripts, which want it at
`vendor/ge-decomp/baserom.u.z64`. I keep dumps in `roms/` and symlink one into place. `.gitignore`
blocks `roms/`, every `*.z64` / `*.n64` / `*.v64` / `*.elf`, all `getv/build-*` directories, and
`vendor/` and `deps/` themselves. Please don't defeat those rules.

Two other things are missing from a fresh clone for related reasons. Fifteen third-party
port-layer files (the Fast3D renderer and the audio mixer, from sm64ex) are fetched from a
pinned upstream commit, because their redistribution terms are unresolved. And SDL2 2.30.9 is
yours to supply in `deps/SDL2-2.30.9` and gets built from source, because a Homebrew running
under Rosetta produces an x86_64 SDL2 that will not link into an arm64 binary. That one cost me
an afternoon.

## Documentation

| | |
|---|---|
| [`docs/SETUP.md`](docs/SETUP.md) | The build guide. Start here. |
| [`docs/FAQ.md`](docs/FAQ.md) | Emulators, widescreen, controllers, 4K, what this is and isn't. |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Current state, known issues, planned work. |
| [`docs/FRAME_TIMING.md`](docs/FRAME_TIMING.md) | The frame-rate question, its cause, and what a complete fix needs. |
| [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) | Every configuration key, implemented and reserved. |
| [`docs/MODDING.md`](docs/MODDING.md) | How the tree is arranged and where the seams are. |
| [`docs/CHEATS.md`](docs/CHEATS.md) | The game's own cheat system, exposed by name. |
| [`docs/PLAYER_API.md`](docs/PLAYER_API.md) | Input injection and state readout. |
| [`docs/BOTS.md`](docs/BOTS.md) | The 18 archetypes and the AI bytecode they compile to. |
| [`docs/COOP.md`](docs/COOP.md) | Two to four players in the campaign, and where it stops. |
| [`docs/NETPLAY.md`](docs/NETPLAY.md) | The case for lockstep. Read next to `PLAYER_API.md`. |
| [`docs/PORTING.md`](docs/PORTING.md) | The platform layer, per file. |
| [`docs/LICENSING.md`](docs/LICENSING.md) | Where every part came from and which terms are settled. |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | No game data, the patch workflow, measuring, provenance. |

## Known issues

- **Co-op, the player API and bots are alpha**, with their exact limits above.
- **Above 60Hz, set `GETV_REALCLOCK=1`.** The default clock counts a rendered frame as a video
  field, so a 120Hz display runs the world at double speed. The game warns you.
- **Jungle hit registration changes with the divider.** Bond lands 8 hits at divider 1 and 3 at
  divider 2 on the same scripted run. Guard positions advance once per tick, so a moving target
  occupies a coarser set of positions and a shot can pass between them. That's what a lower
  simulation rate means. It isn't a timer bug and I'm not papering over it.
- **The gun barrel intro renders scrambled**, and explosions render magenta unless
  `GETV_RGBA16BE=1`. Both are texture-data bugs, both are being chased with measurements rather
  than guesses. Both are on the roadmap.
- **The HMS MI5 crest is missing** from the multiplayer character select, though the same crest
  renders correctly on the file select screen.

[`docs/ROADMAP.md`](docs/ROADMAP.md) has the full list.

## Licensing

[`LICENSE`](LICENSE) is MIT and covers the original work here: the platform layer under
`getv/port/` excluding fetched third-party sources, the build scripts, `tools/`, and the
documentation. It does not and cannot cover anything else. [`NOTICE`](NOTICE) states the scope
precisely.

Read [`getv/port/PROVENANCE.md`](getv/port/PROVENANCE.md) before you redistribute anything. In
particular the licence status of `getv/port/fast3d/`, which descends from sm64ex's copy of
`Emill/n64-fast3d-engine`, is **unresolved**. The notice sm64ex ships is the pre-2021 form, whose
second condition bans binary redistribution outright. Don't assume it's MIT.

The decompilation itself has no licence file, and its libultra sources carry SGI proprietary
headers. That's upstream's situation, but it's a fact about the base this port stands on.

The ROM, the extracted assets, and anything derived from them are never distributable under any
licence.

This project isn't affiliated with, endorsed by, or connected to Nintendo, Rare, MGM, Danjaq or
EON Productions.

## Credits

**GoldenEye 007 was made by Rare in 1997.** Everything here is a wrapper around their work.

[`n64decomp/007`](https://github.com/n64decomp/007), led by **KholdFuzion** with dozens of
contributors over nine years, hitting 100% in August 2026. Without it there's no project here at
all. The mirror of record is
[gitlab.com/kholdfuzion/goldeneye_src](https://gitlab.com/kholdfuzion/goldeneye_src), and
[`kholdfuzion/goldeneye_docs`](https://github.com/kholdfuzion/goldeneye_docs) answers the
questions the source alone doesn't.

Code that ships here: [sm64ex](https://github.com/sm64pc/sm64ex) for the Fast3D renderer and
audio mixer this port's layer descends from, and
[Emill/n64-fast3d-engine](https://github.com/Emill/n64-fast3d-engine) for the original Fast3D by
**Emill** and **MaikelChan**. [SDL](https://github.com/libsdl-org/SDL) for window, input, audio
and gamepads. [Dear ImGui](https://github.com/ocornut/imgui), **Omar Cornut's** immediate-mode UI,
behind the launcher and dev overlay. [Lua](https://www.lua.org/) from PUC-Rio, which is what makes
mods possible without a rebuild. And Roboto Condensed, by the Roboto Project Authors under the
SIL Open Font License 1.1. File-by-file provenance is in
[`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) and [`NOTICE`](NOTICE).

### Work that shaped this without a line of code being taken

Ports are built on each other's hard-won knowledge as much as their source, and several projects
below are licence-quarantined precisely so that nothing was taken. The debt is real either way.

- **[Graslu](https://github.com/Graslu)** worked out the frame-rate problem and said so publicly.
  It's the thing people complain about most above 30fps, and he got there first.
- **[The Perfect Dark PC port](https://github.com/perfect-dark-pc-port/perfect_dark)** is the
  closest sibling to this work and the more mature one. Its structure showed me what a
  decompilation port should look like, its netplay design settled the client-server question here,
  and its simulant model shaped how bots are approached. Read for approach, not copied.
- **Joel Middendorf** and the **1964** emulator (1999-2002), and **`Graslu/1964GEPD`**, which
  forked it for GoldenEye and Perfect Dark. GPL-2.0 and quarantined, so what I took is a list of
  symptoms worth chasing and nothing else.
- **[Ship of Harkinian](https://github.com/HarbourMasters/Shipwright)** and
  **[libultraship](https://github.com/kenix3/libultraship)** proved this shape of port on
  console-class hardware years before I started.
- **GoldenRecomp**, **`cblock85/GoldenEye64Recomp`** and **`chrissotraidis/goldenpad`** took
  different routes at the same problem. All on the do-not-read list here, all worth knowing about.
- The wider **N64 decompilation and homebrew community**, whose documentation of the RCP, TMEM,
  F3D microcode and the RDP's combiner is why the renderer came up in weeks and not years.

If your work is in here and you're not named, that's an oversight and not an opinion. Open an
issue and I'll fix it.

Built by Evan King ([@SegfaultEvan](https://github.com/SegfaultEvan)).
