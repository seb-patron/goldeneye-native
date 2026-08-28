# Goldeneye-Native

**A native PC port of GoldenEye 007, built from the decompiled source, for Windows, macOS and
Linux.** Not an emulator, and not a static recompilation. It is the game's own C, from the
[`n64decomp/007`](https://github.com/n64decomp/007) decompilation, compiled straight to an
executable for your machine, with a modern platform layer underneath: SDL2, OpenGL or Metal,
mouse and keyboard, any resolution you like, and a Fast3D display-list renderer standing in for
the N64's RCP.
# Quick Start — Even If You've Never Used a Terminal

You don't need to know how the game works or how to compile anything. Just follow the steps below.

## 1. Download the game files

First, download this project:

[GoldenEye Native on GitHub](https://github.com/SegfaultEvan/goldeneye-native?utm_source=chatgpt.com)

On the GitHub page, click the green **Code** button, then choose **Download ZIP**.

Unzip the downloaded file somewhere easy to find, such as your **Desktop**.

---

## 2. Get your own copy of GoldenEye 007

You need your **own legally dumped copy of the GoldenEye 007 game cartridge**.

This project does **not** provide the game ROM, download it for you, or contain a copy of the game.

Once you have your ROM file, open the `goldeneye-native` folder you downloaded.

Inside it, you'll see a folder called:

`roms`

Put your GoldenEye ROM file **inside the `roms` folder**.

You don't need to rename it.

The ROM can be in any of the three common N64 byte orders and can use any of the three common ROM file extensions.

---

# 3. Install and run it

## Windows

On Windows, open the `goldeneye-native` folder.

Right-click an empty area inside the folder while holding **Shift**.

Choose **Open PowerShell window here** or **Open in Terminal**.

Then copy and paste this entire command:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1
```

Press **Enter**.

Follow any instructions that appear on screen. 

---

## Linux

Open your **Terminal** application.

Go to the folder you downloaded. For example:

```bash
cd ~/Desktop/goldeneye-native
```

Then run:

```bash
bash tools/install.sh
```

Follow any instructions that appear on screen.

---

macOS

Open **Terminal**.

The easiest way:

1. Press **Command + Space**.
2. Type `Terminal`.
3. Press **Enter**.

Now type the following commands **one at a time**, pressing **Enter** after each one:

```bash
cd ~/Desktop/goldeneye-native
```

If you put the folder somewhere other than your Desktop, drag the `goldeneye-native` folder into Terminal after typing `cd ` instead.

Then type:

```bash
bash tools/install.sh
```

Follow any instructions that appear on screen.

## Important

**You must provide your own copy of GoldenEye 007.**
Nothing in this repository downloads a ROM, and no GoldenEye ROM is included with the project.

It fetches the third-party port sources, clones the decompilation, applies every patch in
`getv/patches`, fetches Lua and Dear ImGui, builds SDL2, finds and verifies your ROM, runs the
whole asset pipeline in the order it has to run in, and builds. When it finishes it tells you
what to run. Re-running it is safe and is how you resume if something stops it.

Prerequisites: macOS 13+ on Apple silicon with the Xcode Command Line Tools, or a Linux box with
a compiler, CMake, Python 3 and the SDL2 development headers. The installer checks for each and
prints the exact package command for your distribution if one is missing. It does not run `sudo`
on your behalf.

Supply your own copy of the game. Nothing here downloads a ROM and nothing here ships one. Any
byte order works; the installer identifies the header and converts a `.v64` or `.n64` dump to
the `.z64` the build wants, then checks the result against the known retail SHA-1 before using
it. Put it in `roms/` or point at it:

```bash
bash tools/install.sh --rom ~/Desktop/goldeneye.n64
```

Useful flags: `--no-build` stops once the assets are ready, `--yes` never prompts, and
`--desktop` installs a menu entry and icons under `$HOME` on Linux.

The Windows one takes the same flags spelled the PowerShell way (`-Rom`, `-NoBuild`, `-Yes`)
and delegates the toolchain to `tools\fetch_deps_windows.ps1` rather than deciding for itself
which mingw-w64 to use. It needs git and Python on PATH first, and names both and stops if
either is missing.

**[`docs/SETUP.md`](docs/SETUP.md) is the same procedure written out by hand** - every
prerequisite, every command, the expected output of each one, and a troubleshooting section.
Read it when something goes wrong, or when you want to know why a step is where it is. The
installer is that document with the ordering constraints encoded rather than described; the
asset-generation sequence in particular is a set of extraction and code-generation passes where
skipping any one produces a tree that fails to compile or, worse, silently misbehaves.

There is no MIPS interpreter and no dynamic recompiler anywhere in it. The binary *is* the
game. Every system is ordinary C that can be read, changed and rebuilt, and that is the whole
difference: the things an emulator can only work around from the outside, this fixes at the
source.

Which is not an abstract claim. GoldenEye's most famous problem is that it counts time in whole
video frames, so running it faster runs the *game* faster: guards firing at double rate,
ammunition draining, AI stepping quicker than it was tuned for. Every emulator inherits that,
because it is in the game rather than in the hardware. **Here it is fixed.** The world holds 60
video fields a second while the renderer runs at 486, measured against a real clock rather than
a frame counter. There is a whole section on it below, including the two attempts that looked
like they had worked and had not.

What that unlocks is the rest of this page: uncapped high-refresh play with correct game speed,
true widescreen that widens the view instead of stretching it, mouse look, HD texture packs,
Lua mods, bots that drive the game's own AI, split-screen with all 64 characters, horde mode,
and a launcher to pick it all from.

![Silo, from the walkway beside the missile](docs/images/screenshot-01.jpg)

| | |
|---|---|
| **macOS** | Apple Silicon and Intel. Builds and plays. |
| **Linux** | x86-64 and arm64. Builds and renders. Verified on Debian 12 aarch64. |
| **Windows** | x86-64, mingw-w64. Builds and plays. |
| **tvOS / iOS** | Bring-up. Builds and deploys to real hardware, with a native Metal renderer. |

Same source tree, same features, one `build` script each. There are two rendering backends
behind one interface, OpenGL and a native Metal one (not MoltenVK), which is what keeps the
Apple targets viable as desktop GL and GL ES are deprecated out from under them.

## What you get

- **Every stage.** All 27 loadable missions boot, render and exit cleanly. Twenty-one load
  directly; six are multiplayer-only and need two or more players.
- **Widescreen that actually widens the view.** Not a stretched 4:3 image and not a
  pillarbox: the projection is recomputed for the window's real aspect, so a 16:9 window
  shows more of the room to either side. Split screen is corrected the same way.
- **The frame-rate problem, solved rather than worked around.** GoldenEye counts in whole
  video fields, so running it faster used to run the *game* faster. That is fixed. Set
  `framerate = off` and the renderer free-runs while the world keeps its own time: measured
  on Dam, 60.8 fields per second against the correct 60.0, at 406 frames per second. The
  number that matters there is the first one, and it is the one a frame counter cannot see.
  There is a whole section on this below, including the two traps that cost the most.
- **It renders fast.** 394 to 433 frames per second at 1280x960 on an M1, three runs, with
  the cap and vsync released. The spread is the honest report; the renderer has not been the
  bottleneck for a while.
- **Multiplayer.** Split screen, radar, all 64 selectable characters.
- **Bots that use the game's own AI.** GoldenEye already ships a behaviour VM with 250 AI
  opcodes driving every guard in the campaign. The bots drive that rather than replacing it,
  with navigation, door handling and an arbiter on top.
- **Keyboard and mouse**, on by default. Mouse look with sensitivity and invert, left button
  fires, right aims, ESC releases the cursor. Gamepads work alongside it, not instead of it.
- **A launcher.** Pick a level, a ruleset, cheats, resolution and field of view before you
  play, instead of editing a config file.
- **Rulesets and horde mode.** Enemy health, damage, accuracy, ammo, player health and
  explosion strength as percentages, with presets. Horde spawns replacements where a guard
  falls and grows the waves as you clear them.
- **Lua mod scripting.** Drop a `mod.lua` in `mods/` and get `onFrame`, `onPlayerSpawn` and
  `onWeaponFire` with a read API into live game state. No rebuild.
- **Co-op.** Two to four players in a single-player mission, sharing its geometry, props and
  objectives. Bring-up quality, and the section below is specific about what that means.
- **A crosshair you can colour.** `crosshair_color = FF0000`, or a picker in the launcher. The
  RDP already multiplied the sight texture by a colour at that call site and retail always
  passed white, so this changes the value rather than adding a pass.
- **The cheats the game already had**, by name, from the launcher or a config file.
- **HD texture packs, with parallax.** Drop PNGs named by content hash into a folder and
  they replace the N64 originals as they decode. `GETV_TEXPACK_DUMP` writes the baseline
  first, so a pack starts as a copy of what the game already produced with individual files
  swapped out, rather than a guessing game about what to name things. A pack that also ships
  `<hash>_h.png` height maps gets parallax displacement on top, which GoldenEye+ turns on and
  97 Console leaves off, so the same installed pack means different things under the two.
  No pack is the normal case and costs nothing.
- **A GoldenEye+ profile.** One switch that turns on everything this port added and verified:
  HD textures, parallax, FXAA, MSAA 4x, anisotropic 8x, mipmaps, supersampling, a smaller
  reticle, and uncapped frames on the real clock. It is a profile and not a fork, so every
  item under it stays individually toggleable, and `97 Console` clears the same set rather
  than merely not setting it. `preset = plus` in the config file, or the launcher.
- **A reticle sized for a monitor.** The 1997 sight was 32 pixels against a 320x240 field of
  view on a CRT across a room. At 1280x960 on a desk it covers a good deal more of what you
  are aiming at than it used to. `crosshair_scale` moves it, GoldenEye+ asks for 0.6, and
  1.0 is the retail size exactly. It is applied after the aspect corrections, so the shape
  never changes and only the size does.
- **Two renderers.** OpenGL everywhere, and a native Metal backend on Apple hardware.
  `GETV_RENDERER=metal` picks it; both build from the same tree and each gets its own binary.
- **Modern presentation, off by default.** Arbitrary resolution, supersampling, MSAA,
  anisotropic filtering, FXAA, adjustable field of view, and three texture filters including
  the N64's real three-point sampling. Off by default because faithful is the default; the
  GoldenEye+ profile is the one clearly labelled door they all live behind.

Sixteen unit test files, 418 checks, covering the parts that are testable without a window:
the bot arbiter and policy, the input queue, the discovery parser, sense stability, fire
cadence, the field integrator, the config parser and the mouse arithmetic. They run on macOS
and Linux with `getv/port/tests/run_tests.sh` and on Windows with the PowerShell twin beside
it. A test that exits cleanly having asserted nothing is treated as a failure, because three
of them used to print only on failure and scored zero checks while looking exactly like the
twelve doing real work.

## On frame timing, and a thank you

GoldenEye runs above 60 frames per second here with correct game speed. That is the headline,
and it is worth being precise about what it means: the game advances `currentFrameCounter` in
whole video fields, a correct build advances it by 60 per real second however many frames it
drew, and this one does.

The problem was real and **[Graslu](https://github.com/Graslu) raised it publicly and was
right to.** `waitForNextFrame()` was already real-time based, so the clock never drifted. What
broke is everything the game counts *per iteration* rather than per second, and 122 of the 135
files under `src/game` do per-frame work. Real hardware managed 20 to 30 frames a second, and
automatic fire rates, turret delay and guard reaction stepping were tuned against that. Run the
same loop at a locked 60 and they simply happen more often.

Getting here took three attempts, and the reason the first two failed is more useful than the
fix. Both were measured with a frame counter, which cannot see this defect at all: game time
per rendered frame is preserved by construction, so comparing state at a fixed frame number
looks perfect while the game runs thirteen times too fast. Every number below is what the
game's own clock did against a real millisecond clock instead.

Measured on DAM, ten seconds each:

| `framerate` | clock | fields/sec (60.0 is correct) | fps |
|---|---|---|---|
| `60` (default) | synthetic | **60.0** | 60 |
| `120` | synthetic | 117.6 | 118 |
| uncapped | synthetic | 811.9 | 812 |
| `120` | real | 60.3 | 60 |
| **`off`** | **real** | **60.5** | **456** |

The synthetic counter advances a fixed amount per call, so one rendered frame *is* one video
field by construction and the world runs exactly as fast as the renderer. That is why a frame
cap above 60 is refused rather than accepted and quietly played wrong, and why the tick divider
does not rescue it: the divider changes how often the simulation ticks and hands the skipped
fields to the tick that runs, so total game time per real second still follows the render rate.

**`framerate = off` is the setting that works, and it now implies the real timebase.** On the
real clock a field is a unit of real time, and `waitForNextFrame`'s free-run path stops blocking
on the field boundary, so the renderer runs ahead while the world keeps its own time. A cap does
not free-run, which is why `120` with the real clock still delivers 60 fps and is refused too.

The cost is reproducibility. Elapsed fields become load-dependent, so two runs are no longer
frame-for-frame comparable, which is why 60 and the synthetic clock remain the default.

### The two profiles are two different frame rates

They are worth quoting separately, because GoldenEye+ renders at twice the linear resolution
before it draws anything else. Same machine, same stage, 1280x960 on an M1 with vsync
released, three runs each, median with the spread:

| profile | fields/sec (60.0 is correct) | fps |
|---|---|---|
| 97 Console, as shipped | 59.2 | 59 (the 60 cap doing its job) |
| 97 Console, uncapped | 61.0 | **486** (449-504) |
| GoldenEye+ | 61.0 | **182** (180-182) |
| GoldenEye+ with an HD pack installed | 60.8 | 177 (177-179) |

The profile costs roughly two thirds of the frame rate, and almost all of that is
supersampling: at 2x it renders 2560x1920 and downsamples, which is four times the pixels
before MSAA, anisotropic filtering and FXAA are considered. A texture pack costs about another
3%. The pack measured here was the game's own textures upscaled 2x, which is what a real one
costs in memory and bandwidth without pretending to be art.

The first column is the point. Every uncapped row sits at 60.8 to 61.0 fields a second, so the
world runs at the right speed in all of them, whether the renderer is managing 486 frames or
177.

Interpolation covers the gap between ticks. Measured on a scripted walk, the share of rendered
frames on which the camera did not move at all:

| | still frames | spread of step |
|---|---|---|
| full-rate simulation | 12.1% | 0.1855 |
| quarter-rate, no interpolation | 75.2% | 0.6541 |
| **quarter-rate, interpolated** | **0.0%** | **0.1843** |

**One trap worth recording, because it produced visible flicker.** The tick divider must be 1
under free-run. The interpolation alpha is `phase / divider`, so a divider greater than 1 blends
the camera against a fraction of a frame phase rather than a fraction of elapsed time, and under
a real clock those are unrelated. Elapsed time already gates the simulation there, so dividing
again on top of it is not merely redundant. Fixed in `0009-freerun-divider.patch`.

**Still open:** the frame-quantised systems. Automatic fire is converted and time-based by
default on both the player and the AI side, but the rest still advance once per update rather
than per second. The clock work above is what makes their rate correct rather than the divider
holding them there.

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

**Does it run at 60fps? Can it run higher?** Yes to both. It renders at 60 by default. For a
high-refresh display set `framerate = off`, which uncaps the renderer and switches to the real
timebase so the world keeps its own time: measured on Dam at 1280x960 on an M1 with vsync
released, 60.8 fields per second against the correct 60.0, at 406 frames per second, with a
394 to 433 spread over three runs. Frame rates vary a good deal by stage, so treat that as one
stage's number rather than the renderer's ceiling. A frame cap above 60 is refused, because on the default clock one rendered frame is one
video field and the game would simply run fast. See the frame-timing section above.

**Can I use HD texture packs?** Yes. A pack is a folder of PNGs named by content hash, and
the game will write you the baseline to start from: `GETV_TEXPACK_DUMP=<dir>` dumps each
texture the first time it decodes, so a pack begins as a copy of what the game already produced
with individual files replaced by an upscaler. Set `hd_textures = 1` and point `texpack` at the
folder. A pack that also ships `<hash>_h.png` height maps gets parallax displacement on top
under GoldenEye+. No pack installed is the normal case and costs nothing.

**What is GoldenEye+?** One switch, `preset = plus`, for everything this port has added and
verified: supersampling, MSAA, anisotropic filtering, mipmaps, HD textures, parallax, FXAA, a
smaller reticle sized for a monitor rather than a 1997 CRT, and uncapped frames on the real
clock. It is a profile and not a fork, so every item under it stays individually toggleable and
faithful stays the default. It fills gaps rather than displacing anything, so a setting you
wrote yourself always wins and the rest of the profile still arrives.

**Is there an installer, or do I have to build it by hand?** `bash tools/install.sh` does the
whole thing on macOS and Linux: dependencies, the decompilation, every patch, the asset
pipeline in the order it has to happen in, and the build. Re-running it is safe and is how you
resume if something stops it. It will not download a ROM and it will not run `sudo`.

**Does it work with a controller?** Yes, alongside keyboard and mouse rather than instead of
them. Xbox, PlayStation and generic pads all work through SDL2, bindings are configurable by
name, and there is a deadzone trimmer for a worn stick. All eight of the original control
styles are there, including the two-controller 2.x layouts, which map onto one modern pad's
two sticks.

**Is there widescreen?** Yes, and it widens the view rather than stretching or cropping it.
The projection is recomputed for the window's real aspect ratio, so a 16:9 window shows more
of the room to either side, with the HUD placed against the true window edges. Split screen
gets the same treatment. `fov` is still there if you want to go wider again.

**Can I mod it?** Yes, three ways: Lua scripts in `mods/`, roughly 275 `GETV_*` behaviour
gates, and `goldeneye.cfg`. There is also a texture-override path that reads replacement
images out of a pack directory by content hash, off by default. It is written and reasoned
through but has not been run against a real pack yet, so treat it as untested rather than as
a feature.

**Is multiplayer online?** Not yet, and it is worth being exact about how far along it is.
Split-screen multiplayer works. For network play the lockstep transport, the peer discovery
parser and the launcher page are all written, and the discovery parser has unit tests. What is
missing is the last connection: nothing in the game loop calls into it yet, so selecting Host
or Join sets the variables and no session starts. The input seam it plugs into is the same one
the bots already use, which is why that piece is the one left.

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

Every stage runs twice, once with the scripted stick and once without, and the verdict is
whether the two disagree. That is the whole point: a single run cannot tell "the player walked"
from "the level teleported him to his start pad", and the earlier version of this tool could
not either. It reported Dam as 17,232 units moved with the stick held forward, and 17,232 with
no input at all, to the unit, and passed both.

Its current result: **all 21 solo missions reach gameplay, and 20 of the 21 respond to the
stick**, with the input moving the player between 22 and 5,368 units over a 900-frame run and
objective counts matching the missions. No objective advanced, which is expected when the input
is "walk forward" and nothing else. So the port is further than "renders" and well short of
"plays": reaching a playable state is measured across every mission, responding to a
controller is measured across every mission that has one, and completing a mission is not.

Two readings to know about. Cuba is the credits sequence rather than a mission, so there is no
player to steer and the tool exempts it by name; it also reports no objectives, which makes
`objectiveIsAllComplete()` trivially true and prints `complete=yes` over an empty set.

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
mac game: 167 built, 0 failed
mac assets: 746 built, 0 failed
mac audio: 40 built, 0 failed
mac port layer: 60 built, 0 failed
```

Ten N64-hardware and SGI-dev-host files are excluded by name in the build script:
`tlb_manage.c` programs the MIPS R4300 translation lookaside buffer, and `usb.c`, `rmon.c`,
`sched.c`, `ramrom.c`, `ramromreplay.c`, `audi.c`, `init.c`, `indy_comms.c` and
`indy_commands.c` all talk to hardware or a development host that is not here. Nothing links
against any of them. They are named rather than stubbed, because a stub that compiles is a file
somebody can call by accident.

Check all four counts rather than grepping for one. Compiles run with stderr suppressed, so a
broken file shows up only as a changed number, and a grep for two of the lines will happily miss
a broken audio file. Any name in a `FAILED:` line is a real problem.

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

- **The frame-quantised systems have not all been converted.** The clock itself is fixed and
  the measurements are in the frame timing section above: game time now runs at 60 fields a
  second whatever the renderer does. What is not fixed is the set of systems that count
  iterations rather than seconds. Only 13 of the 135 translation units under `src/game`
  reference `g_GlobalTimerDelta`, and those are the ones that were always right: recoil, sway,
  breathing, camera. Enemy automatic fire is the clearest of the rest --
  `chraction.c:6694` increments `firecount[hand]` once per tick and fires on
  `firecount % automaticFiringRate == 0`, which is a frame count and not a duration.
  Automatic fire is converted on both the player and the AI side and is time-based by default.
  The others are not yet, and each one has to be done individually and checked against retail
  behaviour rather than swept up in a single pass. The clock work is what makes their rate
  correct to fix; before it, there was nothing stable to fix them against.

- **Network play is not connected.** The transport, the discovery parser and the launcher page
  are written; the game loop does not call them, so no session starts. See the FAQ above.
- **Missing HMS MI5 crest on the multiplayer character select.** The same crest renders correctly on
  the file select screen, so the asset and its decode path are sound.
- **Select File background.** Renders flat black; the original has a faint circular watermark
  behind the folders.
- **Multiplayer edge cases.** Score caps are not enforced on the headless path, and `num_shots`
  disagrees with the fire path.
- **Texture packs work, and nobody has made one.** The override path has been run end to end:
  the 56 textures Dam decodes in its first 121 frames were dumped, replaced with flat colour
  PNGs of the same dimensions, and the frame changed in 91% of sampled pixels. What does not
  exist is an actual pack, or any height maps for the parallax path to displace.

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
