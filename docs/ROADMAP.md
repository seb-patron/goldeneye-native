# Roadmap

Current state, known issues, and planned work. Updated 2026-08-24.

## Where the port is

All 26 loadable stages boot, reach gameplay, render, and exit cleanly. That was verified
across 78 headless runs against a single frozen binary, with three runs per stage. There
are no known crashes, hangs, or stages that fail to start.

Multiplayer works, including split screen, the radar, and all 64 selectable characters.
The pause watch renders all five pages. Saves persist. The game runs at 60 fps with
configurable resolution and supersampling.

What has not been verified is a full playthrough: combat, AI behaviour over time,
objective completion, and finishing a level end to end. The port renders and reaches a
playable state everywhere; whether it can be played through from start to finish is
untested.

## Platforms

**macOS (arm64 and x86_64)** and **Linux (x86_64)** are at feature parity as of 2026-08-24.
Both build the same counts -- game 167/1, assets 746/0, audio 40/0, port layer 28/0 -- and
both carry Lua mod scripting, the Dear ImGui dev overlay, rulesets, horde mode and the
launcher. The applied-ruleset values are byte-identical across the two hosts, which is the
check worth repeating whenever either platform's build changes.

⚠️ **The Linux test box is not a performance reference.** It renders at roughly one frame per
second (`GL_RENDERER=NVAF`, nouveau on old NVIDIA hardware), so anything measured in frames
needs a reachable frame number there: a run asking for frame 600 inside a 250-second timeout
simply stops at 241 and reports nothing, which reads exactly like a broken feature. That
cost one debugging cycle before it was recognised.

**macOS (arm64 and x86_64)** and **Linux (x86_64)** both build and run. Linux was brought up
on 2026-08-24 against gcc 13 on Linux Mint 22.3 and produces a 20M ELF; the game batch builds
167/1, identical to macOS, and the one failure (`src/tlb_manage.c`) is a deliberately stubbed
N64 hardware file that fails on both. Fifteen stages ran there with no signals.

Three defects had to be fixed to get there, and all three were invisible to Clang on Darwin:

- `bondtypes.h` decided endianness from `__LITTLE_ENDIAN__` alone, which is a Clang predefine.
  GCC reports byte order through `__BYTE_ORDER__`, so a GCC build silently took the big-endian
  branch, left the `GE_SUBWORD` field groups unreversed, and mismatched every struct overlaid
  on setup-file data. The `_Static_assert`s in `propobj.c` turned that into six compile errors
  rather than corrupt objects at runtime, which is what they exist for.
- `include/stdarg.h` shadows the compiler's own header and defined `va_list` without
  `__gnuc_va_list`, which glibc's `stdio.h` requires. 27 translation units failed on it.
- `get_ptr_item_statistics()` indexes `gitem_structs[]` unchecked, and the guard AI arrives
  with `item = -1`. Reading in front of the array is undefined behaviour and the hosts
  disagreed: Darwin returned the safe defaults by luck, glibc returned a null pointer and the
  caller faulted, so a guard opening fire killed the process on Linux only.

The build script also could not fail: it took its exit status from `head` rather than the
compiler and tested an output binary that a previous run had left behind, so a link with 27
missing translation units still reported success. Both are fixed.

### Windows - builds and runs

Verified 2026-08-24 on a Surface Pro 3, Windows 11, mingw-w64 gcc 16.2: **1801 frames of
Bunker 1, no exceptions, clean exit.**

```
GL_VENDOR=Intel | GL_RENDERER=Intel(R) HD Graphics 4400 | GL_VERSION=4.3.0
frame 1801: tris submitted=4188 drawn=1931 fog=1843
```

⚠️ **Run it from a desktop session, not over SSH.** A remote session gets
`GL_RENDERER=GDI Generic, GL_VERSION=1.1.0` -- Microsoft's software rasteriser -- and GLEW
cannot resolve modern entry points there, so Fast3D calls through a null function pointer
and faults at PC 0x0. That is an environment property, not a port bug, and it is worth
knowing because the crash looks catastrophic and means only "no GPU driver in this session".

The historical detail below is kept because most of it applies to any new toolchain, not
just this one.

### Windows - how it was brought up

A native Win32 build is underway on a Surface Pro 3. Where it stands: the toolchain works,
**165 of 168 game translation units compile** (macOS and Linux build 167/1), and the port
layer's Windows branches are written and compile clean. It does not link yet.

**The toolchain is standalone mingw-w64, not MSYS2.** MSYS2 emulates `fork()` by copying an
address space, which failed outright on this host -- `dofork: child died unexpectedly, exit
code 0xC0000142` -- and the build forks once per translation unit. `autorebase` and
`rebaseall` did not fix it and left both bash and MSYS2's own gcc unable to spawn `cc1.exe`.
The fix was to stop depending on it: `getv/build_windows.ps1` drives mingw's `gcc.exe`
directly from PowerShell, and mingw-w64 from WinLibs plus SDL2's official mingw package need
no POSIX emulation at all. `getv/build_windows.sh` is kept for hosts where MSYS2 is healthy.

Three toolchain-era traps were found, none of them Windows-specific in principle:

- **GCC 15+ defaults to `gnu23`, where `bool` is a keyword.** `bondtypes.h:85` does
  `typedef s32 bool;`. `-std=gnu17` is now passed explicitly, and **this will bite the Linux
  build the moment that host's gcc updates** -- it is a compiler-version trap, not a platform
  one.
- **GCC 14 promoted five old-C warnings to errors** (incompatible-pointer-types,
  int-conversion, implicit-function-declaration, implicit-int, return-mismatch). The decomp
  is 1990s C and trips four constantly. They are demoted back to warnings;
  `-Werror=return-type` deliberately stays fatal.
- macOS AppleDouble `._*` files travelled in a tarball and were fed to the compiler.

**The real remaining problem is that Windows is LLP64.** Measured on the box: `long=4,
ptr=8, size_t=8`, against `long=8` on macOS and Linux. Every decomp struct field declared
`long` therefore changes size, which reintroduces exactly the 32-bit truncation class this
port spent its early life eliminating. Two files fail on it today and both failures are
informative rather than mysterious:

- `stan.c:1453` -- a `_Static_assert` fires: *"StandTile grew: &standTileStart[link] no
  longer equals base + (link << 3)"*. The guard did its job, the same way the endianness
  asserts did when GCC first saw this tree.
- `loadobjectmodel.c:15` -- a local `memcpy` prototype using `unsigned long` conflicts with
  the real one, because `unsigned long` is no longer `size_t`.

Fixing these means auditing `long` in the decomp's structs the way pointer width was audited
before, not adding a flag. `tlb_manage.c` is the third failure and is the deliberately
stubbed N64 hardware file that fails on every platform.

## Known issues

**The scripted-input harness cannot fire a weapon.** `GETV_SCRIPT` drives the stick and the
menus correctly -- the player walks (15542 -> 17442 over 240 frames), START advances the
front end, and `Z` engages the sight (`sightmode` 0 -> 2). But no button ever sets
`trigger_down`, and `shots` stays 0 through 1441 frames of gameplay with the PP7 in hand and
7 rounds in the magazine.

The likely reason is in the same line of output: `hinv=1/0`, so the right hand is invisible.
No weapon drawn, nothing to fire. Whether that is the harness failing to complete the
weapon-raise or a genuine defect in the port has not been established.

This matters more than it looks, because it is the common cause of two separate things that
could not be verified:

- **Weapon behaviour cannot be measured headlessly** -- fire rates, reload timing and the
  automatic-fire cadence all need a shot to be fired. That is exactly the evidence needed to
  back any claim about frame-rate-dependent fire rates.
- **Horde waves cannot be verified from a kill.** The spawn path is proven through
  `GETV_HORDE_SELFTEST`, but the kill -> wave counter needs a guard to die, and nothing can
  shoot one.

Both were previously filed as separate unknowns. They are one gap in the test harness.

**Co-op players do not move (alpha).** 2-4 players spawn, are separated correctly by
`GETV_COOP_SPREAD` (default 60 units, about two feet at this game's scale -- Bond's eye
height measures 167), and then stand still. Player 0 is affected too, which is the most
useful fact about the bug: whatever it is, it is not "the extra players get no input".

Measured on Dam, 900 frames, same scripted input in both runs:

| | solo | co-op |
|---|---|---|
| p0 position at f600 / f720 / f840 | 15542 -> 16628 -> 17442 | 20202.2 unchanged, all three |
| `locked` / `paused` / `clock` | 0 / 0 / 1 | 0 / 0 / 1 |
| scripted input fires | yes | yes |

Four explanations are eliminated: **spawn collision** (they are 60 units apart and still
frozen), **pad presence** (`GETV_PADS=2` changes nothing), **a global time freeze**
(`lvlManageMpGame` zeroes `g_ClockTimer` when controls are locked or the game is paused, and
all three read identical to solo), and **input delivery** (`GETV_SCRIPT` reports FIRE at the
right frames on port 0).

So input arrives, time advances, controls are unlocked, and the player still does not move.
What remains is multiplayer-specific gating between the pad read and the movement apply --
player-to-controller mapping, or a pre-game state the MP path expects a real match to leave.
`GETV_STATE=<n>` now prints `locked`, `paused` and `clock` alongside the positions, which is
what made this narrowing cheap.

**Doors that never resolved a position.** `proplvreset` sets `door->prop = NULL` when
`getposstan()` cannot resolve a door's pad against the stan, and the rest of the game then
uses `door->prop` in 92 places without checking. Interacting with such a door faulted at
address 0x10 -- the offset of `pos` within `PropRecord` on this build. The three door sound
functions and `sub_GAME_7F053A3C` now return early, so the crash is gone, but **the failing
`getposstan()` is the real defect and is unfixed**. A door in that state is still not drawn
and still cannot be used properly. How many doors across the game are affected has not been
measured, and that measurement is the next step.

**Out-of-range `attack_item`.** The guard AI reaches `bondwalkItemGetAutomaticFiringRate()`
with `item = -1`. The lookup is now range-checked and returns the game's own defaults, so it
is no longer a crash, but a weapon id of -1 arriving there is a bug further upstream that has
not been traced. `GETV_ITEMSTATS=1` reports each rejected id once; it fires on most stages.

**Cannot advance from one level to the next.** Reported after completing level one: the
"next" control does not proceed to level two. Not yet reproduced under instrumentation, and
no root cause. The mission-complete path is `objective_status.c`; `GETV_STATE=<n>` prints
objective counts, individual statuses and a completion flag each n frames, which is the
intended way to bisect this.

**Depot ground colour.** The ground on Depot renders saturated blue-cyan where it should
be near-neutral dark asphalt. The texel pattern matches the original exactly, and the
palette indices, palette offset, palette contents and vertex shade have all been measured
correct. The colour is introduced after the texel fetch, in the combiner or one of its
non-shade inputs.

**Frigate sky.** The sky renders as flat dark navy rather than blue with cirrus cloud.
The cloud display list runs and emits more commands than any other stage's, so the path is
active. Whether the geometry is drawn and invisible, or not drawn, has not been
established.

**Missing gold crest on the multiplayer character select.** The same crest renders
correctly on the file select screen, so the asset and its decode path are sound. The fault
is in how the character select screen requests it.

**Surface 2 fog density, Cradle sky gradient, Facility vent colour.** Three stages show
colouring that may or may not be correct. No reference captures exist for them, so they
are unresolved rather than confirmed defects.

**Select File background.** Renders flat black; the original has a faint circular
watermark behind the folders.

**Door interact NULL crash - FIXED 2026-08-23.** Activating certain doors
(`propdoorInteract` -> `doorActivateWrapper` -> `doorSetOpenState` -> `doorStartOpen` ->
`doorPlayOpenSound0`) segfaulted at fault address `0x10`. Root cause: `door->prop` was
NULL on the affected door, and `&door->prop->pos` computes a non-NULL garbage address
(`offsetof(PropRecord, pos)`, which is 0x10 at 64-bit vs 0x08 on the N64's 32-bit ABI) that
then gets dereferenced. Guarded all six call sites in `propobj.c` (`doorPlayOpenSound0/1`,
`doorPlayCloseSound0/1`, `sub_GAME_7F053A3C`) on `door->prop != NULL`; when NULL, the
position-dependent sound-distance call is skipped rather than crashing. This stops the
crash but does not explain *why* `door->prop` was NULL for that door in the first place -
see the corruption note below.

**Suspected BSS corruption: paintball cheat active with no user action, bullet impacts
render as paint splats.** `explosionCreateBulletImpact` forces `impact_type = 0x10`
(the paintball splat) whenever `cheatIsActive(CHEAT_PAINTBALL)` is true
(`src/game/explosion.c:2025`). A report of "confetti explosions" and "paintball mode on
by default" in the same session is almost certainly one symptom: the flag in
`g_CheatPlayerTextRelated[CHEAT_PAINTBALL]` (`src/game/cheat.c:26`) is set though nothing
in `goldeneye.cfg`, the CLI or an in-game menu set it. `cheatDisableAllCheats()` clears it
on stage unload, so it re-appearing across levels within one run points at something
writing that byte, not at the cheat system itself. First things to rule out: an existing
`goldeneye.cfg` with a stale `cheats = paintball` line from earlier testing (check
`~/Library/Application Support/Goldeneye-Native/goldeneye.cfg` and the pre-rename path);
a `GETV_CHEATS`-shaped variable left exported in the shell. If neither, this is the same
class of bug as the `g_Props` overrun that ate the memory-pool bank table - something is
writing past its own storage into `g_CheatPlayerTextRelated`. `gePortBootMark()` already
runs `gePortStubCheck()` and `gePortMempSane()` on every boot mark; reproduce with those
active and see whether either fires before the flag turns up set.

**Reported: one-shot kills.** Not yet reproduced or localised - needs which side (player
taking one-shot damage, or enemies dying in one hit), which weapon, which difficulty, and
whether `GETV_DIFFICULTY` was set. Filed rather than guessed at.

**Reported: after completing a solo mission, "next" does not advance to the next
level.** `interface_menu0D_missioncomplete()` (`src/game/front.c:7736`) only advances past
the mission-complete screen when `frontCompleteAllObjectivesAliveSuccess()` returns true;
otherwise it returns to `MENU_BRIEFING` for the same stage regardless of which tab was
selected. That function (`front.c:7619`) returns 0 immediately if `mission_failed_or_aborted`
or `g_isBondKIA` is set, even if every objective is actually complete. Both are plain BSS
flags with no obvious writer between a successful completion and the debrief screen, which
makes them a second candidate for the same corruption family as the two issues above.
Needs a repro with `GETV_STATE` running across the transition to confirm whether either
flag is set going into the mission-complete screen.

## Platform support

macOS on Apple silicon is the working target.

Windows and Linux are not yet buildable. The game layer itself is not platform-specific -
the portability gate means "not an N64" rather than "Apple" - so the work is confined to
the platform layer and the build script. `docs/PORTING.md` lists what remains, with file
references and estimates. Linux is the easier of the two.

tvOS was the original target and its build scripts remain, but it has not been exercised
recently and is not currently supported.

## Planned work

### Correctness first

The remaining known issues above are the priority.

The baseline that gates the enhancement work now exists. `tools/render_refs.py` reduces each
stage to an 8x6 grid of mean RGB and stores it in `tools/refs/render.txt`; `check` recaptures
and reports any stage that drifts. A fingerprint rather than a folder of captures, because a
screenshot of this game is ROM-derived and cannot be committed, and because 144 numbers per
stage is enough to catch a texture binding to the wrong surface, a palette going wrong or
geometry disappearing.

Run `check` before and after anything that touches the renderer. The tile-selection fix that
corrected Depot's ground is exactly the shape of change it is for: it had to be shown to alter
one stage and leave twenty-six alone, and that comparison was done by hand.

### Build order

Ordered by effort, not by appeal. Estimates are estimates; the Depot ground colour was filed as
a small rendering bug and took a full session, so treat anything involving the RDP with caution.

Each of the ten enhancement gates below already exists as a config key that parses, validates
and range-checks its value, then prints NOT-IMPLEMENTED. The naming and plumbing are done; the
work is the implementation behind them.

**A day or less each. Context and framebuffer settings, no engine changes.**

| item | notes |
|---|---|
| 24-bit depth buffer | `GETV_DEPTH_BITS`. An SDL GL attribute at context creation. The N64's z-fighting is a 16-bit depth artefact, so this is the highest ratio of visible improvement to code in the whole list. |
| MSAA | `GETV_MSAA`. Two SDL GL attributes and enabling multisample. |
| Anisotropic filtering | `GETV_ANISO`. One texture parameter where the sampler is configured. |
| Field-of-view | GoldenEye already has `viSetFovY`; Perfect Dark item 9. |

**Days. Confined to one subsystem.**

| item | notes |
|---|---|
| Per-pixel fog | `GETV_FOG_PERPIXEL`. A shader change in the Fast3D backend. |
| Positional audio | `GETV_AUDIO_3D`. The mixer exists; this is panning and attenuation from emitter positions. |
| Muzzle-flash lighting | `GETV_MUZZLE_LIGHTS`. Needs a light injected into the shade path, which is per-vertex here. |
| Mipmapping | `GETV_MIPMAPS`. Harder than it sounds: the game ships its own mip pyramid and selects LODs itself, so this has to cooperate with `TRILERP` rather than replace it. |
| State and control API | A socket exposing player, chr list and objectives, and accepting pad state. Frame-stepped so runs stay reproducible. Unlocks automated playthrough verification, which is what "full playthrough unverified" needs. |
| Two-player co-op bring-up | The engine is already N-player generic; the hook is one integer at `boss.c:489` and multiplayer spawns can be reused. Perfect Dark item 5, no Perfect Dark code required. |

**Weeks. Architecture, or breadth.**

| item | notes |
|---|---|
| VFS and asset override | Perfect Dark item 11. Without it every mod is a rebuild, so this is the mod-pack foundation rather than a mod. |
| External and HD texture packs | Perfect Dark item 14, and it depends on the VFS above. |
| Fixed simulation tick with interpolation | The real fix for frame-coupled gameplay. `framerate=30` is the current mitigation. This is the single largest correctness item left. |
| Widescreen | Perfect Dark item 16, rated large there. |
| In-game options menu | Perfect Dark item 17. Ends the config file being the only interface. |
| Multiplayer bots | Native work reusing `chraction.c`. Not a Perfect Dark import: simulants are welded to Perfect Dark's own structures. |

**Open-ended. Do not schedule these against a date.**

| item | notes |
|---|---|
| Remaining rendering defects | Frigate's sky, the multiplayer character-select crest, the Select File watermark. Each is a hunt, and the last one of these took a session. |
| SSAO, real-time shadows, per-pixel lighting | `GETV_SSAO`, `GETV_SHADOWS`, `GETV_PERPIXEL_LIGHT`. These change the look enough to need art direction, not just a flag. Per-pixel lighting departs furthest from the original, whose lighting is per-vertex. |
| Full co-op with AI, objectives and cutscenes | Perfect Dark item 18, rated very large. Bring-up above is the tractable part. |

Run `tools/render_refs.py check` before and after anything in the first three groups.

### Enhancements

Configuration keys for these are already parsed and validated. `depth_bits` and `msaa` are
now implemented; the rest currently do nothing.
Enabling one prints a notice saying so. See `docs/CONFIGURATION.md`.

The first group removes hardware limits without changing artistic intent: a wider depth
buffer (the N64's z-fighting is a 16-bit depth limitation), anisotropic filtering, MSAA,
mipmapping, and per-pixel fog. Together these are a modest amount of code and no new art.

The second group is small but visible: dynamic lighting on muzzle flashes, which currently
light nothing in dark levels, and positional audio.

The third group changes the look enough to need art direction rather than a flag:
screen-space ambient occlusion, real-time shadows, per-pixel lighting, and HD or upscaled
textures. Per-pixel lighting in particular departs furthest from the original, whose
lighting is per-vertex.

### Larger features

**Co-op campaign.** More tractable than it appears. The engine is already player-count
aware in 163 places because of split-screen multiplayer, and Perfect Dark - which shares
this engine's ancestry and is MIT licensed - shipped both co-op and counter-op. The
difficult part is level data: Perfect Dark's co-op needs second spawn points and co-op AI
lists authored per level, and GoldenEye's solo levels have exactly one spawn and none of
that. Budget it as content authoring with tool support, not as engine work.

**Online multiplayer.** The largest item here, and the only one that turns the project from
shipping software into operating a service. GoldenEye's multiplayer is local split screen
with no networking to extend, and Perfect Dark has none either, so this is new work rather
than adaptation. The architecture decision hinges on whether the simulation is
deterministic enough for lockstep; that experiment is cheap and should be run before any
netcode is designed. A direct-connect implementation with no hosted service is the variant
that keeps the project's existing shape.

**Third-person camera.** The camera system is already parameterised for several modes. The
open question is whether the third-person player model is drawn during gameplay at all;
establish that before estimating the rest.

**Keyboard and mouse.** Not yet supported.

**Launch window / front-end.** A native pre-launch window rather than a config file: pick
"classic" (stock N64 behaviour, every enhancement gate off) or an enhanced mode, then
toggle cheats, co-op, FPS cap, FOV, mod packs, HD textures and lighting from checkboxes and
sliders instead of hand-editing `goldeneye.cfg` or exporting `GETV_*` variables. This is a
UI shell over gates that mostly already exist (`ge_config.c`'s key table, the cheat list in
`cheat.c`) rather than new engine work; it depends on the in-game options menu and VFS/mod
pack items above for the toggles that aren't implemented yet. Filed from `futureideas`.

## Contributing

The roughly 250 `GETV_*` environment gates are the practical extension surface. Each
defaults to preserving stock behaviour and can be disabled to compare against it. See
`docs/MODDING.md`.
