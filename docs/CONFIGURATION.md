# Configuration

Every setting has the same name on the command line and in the configuration file. Source of
truth is `getv/port/src/ge_config.c`.

## Where the file lives

The first of these that exists is used, and the rest are ignored:

1. `$GETV_CONFIG`
2. `--config=PATH`
3. `goldeneye.cfg` in the same directory as the binary
4. `~/Library/Application Support/GoldenEye/goldeneye.cfg`

If none exists and none was asked for, the game writes the commented template to location 4 and
reads it back. That first-run write is how the port's tuned defaults actually reach you; a value
that only appears in a template nobody has generated does nothing.

Save data is not stored here. It goes to `~/Library/Application Support/Goldeneye-Native/eeprom.bin`,
or to `save_dir` if you set one.

## File format

```
# comment
; also a comment
key = value
```

Whitespace around the key and value is trimmed. `#` and `;` begin a comment anywhere on a line.
There are no sections, no quoting and no line continuations. An unrecognised key is reported on
stdout rather than silently ignored, as is a line with no `=`.

Values are lowercased before matching, except for `save_dir` and raw `GETV_*` passthrough, where
case is meaningful.

## Precedence

    command line  >  environment  >  config file  >  built-in default

This is implemented with one mechanism: the config file calls `setenv` with overwrite disabled,
so it can never displace a variable that is already set, while command-line flags call it with
overwrite enabled. Every consumer in the port reads the environment and needed no changes. An
environment variable always beats the file.

## Command-line flags

| Flag | Effect |
|---|---|
| `--help`, `-h` | Print the built-in usage summary and exit. |
| `--list-cheats` | Print every named cheat with its id and whether it is live. Exits. |
| `--config=PATH` | Read this file instead of searching. `--config PATH` also works. |
| `--write-config[=PATH]` | Write the commented default config and exit. With no path, writes to `~/Library/Application Support/GoldenEye/goldeneye.cfg`, creating the directory. |

Any other setting is passed as `--key=value`, for example `--resolution=1920x1440`. A bare
`--key value` pair is also accepted when the next argument does not begin with `-`.

## Display

### `resolution`

`WIDTHxHEIGHT`, or the literal `fullscreen` / `native`. Width must be at least 320 and height at
least 240; anything else is rejected with an error and the default is kept. `fullscreen` and
`native` are shorthand for `fullscreen = 1`.

Default `1280x960`. The built-in default is clamped down in whole 4:3 steps if it would not fit
the usable display area, because an oversized SDL window on macOS opens with its title bar under
the menu bar and cannot be moved.

### `aspect`

`4:3`, `16:9` or `auto`. `43` and `169` are accepted as aliases. No default.

Read the scope carefully. The renderer derives its aspect ratio from the actual framebuffer
dimensions, so it does not need an aspect setting - it needs a correctly shaped window. This key
therefore does two things and nothing else: if `resolution` is unset it picks a default window
shape (`1280x960` for 4:3, `1600x900` for 16:9), and if `resolution` is set it checks the window
shape against the requested aspect and prints a note if they disagree by more than 3%. An explicit
resolution always wins. `auto` is a no-op.

There is no second, independent aspect control, and there deliberately is not one: two of them
would aspect-correct twice.

### `fullscreen`

`0` or `1`; `on`/`off`, `true`/`false` and `yes`/`no` are accepted. Default `0`. macOS is the one
platform in this port that has a window, and starting windowed is the point of the target.

### `supersample`

`1` or `2`. Anything else is rejected. Default `1`.

`2` renders at double size and downsamples. Note that it changes the framebuffer size and
therefore the heap layout, so two runs at different supersample settings are not comparable to
each other for anything more precise than "it looks better".

### `filtering`

| Value | Aliases | Meaning |
|---|---|---|
| `point` | `nearest`, `n64` | Literal N64 point sampling. |
| `bilinear` | `linear` | Standard bilinear. |
| `three-point` | `threepoint`, `3point`, `default` | What the N64 RDP actually did. |

Default `three-point`.

Two independent mechanisms exist behind this key - the renderer's `configFiltering` and the
`GETV_POINT_FILTER` gate - and they do not mean the same thing. The key sets both consistently so
they cannot disagree.

### `widescreen`

`on` or `off` (`1`/`0`, `true`/`false`). Default `on`.

The console's own render path (`gfx_pc.c`'s `ge_scale()`/`gfx_adjust_x_for_aspect_ratio()`)
deliberately pillarboxes to the N64's native 4:3 rather than stretching a wider window - correct
for avoiding distortion, but with no way to ask for an actually-wider view until this key existed.
`on` fills the real window at its own aspect instead, with a wider field of view rather than a
stretched image; `off` restores the original letterboxed framing byte-for-byte. Single-player
only - split-screen's per-viewport aspect is untouched either way, since it has not been audited
against an arbitrary host window.

Sets `GETV_WIDESCREEN`, read by both the renderer (`configWidescreen`, `port_support.c`) and the
game layer (`lv.c`'s per-player aspect write).

### `hd_textures` / `texpack`

`hd_textures` is `on` or `off`, default **off**. `texpack` is a directory path, default
`hdtextures` (resolved next to the executable if no such folder exists relative to the working
directory - same `GETV_EXEDIR` fallback `moddir` uses).

When on, every N64 texture is looked up in the pack directory by content hash before upload - a
file named `<hash>.png` there replaces the console's own texture; anything not present in the pack
renders exactly as it did before this key existed. The hash is FNV-1a 64 over the raw N64 texel
bytes plus format/size, so it is stable across runs and does not depend on where the source data
sits in memory. `GETV_TEXPACK_DUMP=<dir>` (environment only, not a config key - it is a developer
tool, not a player setting; **not** `GETV_TEXDUMP`, which is [`image.c`'s own unrelated gate](MODDING.md)
- a byte count, not a path) writes a same-named `.ppm` baseline the first time each texture is
decoded, which is how a pack gets started: dump, convert the ones worth upscaling to `.png`, drop
them back in named by hash.

**Off by default, unlike `filtering` and `widescreen` above.** Those two were verified by tracing
the actual call order and checking the arithmetic by hand; this one has not had a compiler
available to run any verification against and is offered as written-and-reasoned-through rather
than measured. An empty pack directory is a no-op either way, so turning it on without a pack
installed costs nothing beyond one failed file lookup per unique texture - but "costs little if
wrong" and "verified correct" are different claims, and only the first one currently holds.

Sets `GETV_HD_TEXTURES` and `GETV_TEXPACK`, read by `configHDTextures` and the pack-directory
resolver in `port_support.c`.

### `framerate`

`30`, `50`, `60`, or `off` (`0`, `uncapped` and `unlimited` are accepted for the last). Default
`60` on NTSC builds, `50` on PAL.

**A frame cap above 60 is refused, and `off` is the high-refresh setting.** GoldenEye advances
its clock in whole video fields. On the default synthetic counter `osGetCount()` moves a fixed
amount per call, so one rendered frame is one video field by construction and the world runs
exactly as fast as the renderer. Measured on DAM, ten seconds each, reading the game's own
`currentFrameCounter` against a real millisecond clock:

| `framerate` | clock | fields/sec (60.0 is correct) | fps |
|---|---|---|---|
| `60` (default) | synthetic | **60.0** | 60 |
| `120` | synthetic | 117.6 | 118 |
| uncapped | synthetic | 811.9 | 812 |
| `120` | real | 60.3 | 60 |
| **`off`** | **real** | **60.5** | **456** |

`off` sets `GETV_REALCLOCK=1` as well as removing the cap, because uncapped on the synthetic
clock is the worst configuration available here and there is no reason to let someone reach it
by accident. On the real timebase a field is a unit of real time, and `waitForNextFrame`'s
free-run path stops blocking on the field boundary, so the renderer runs ahead while the world
keeps its own clock. `put()` does not overwrite, so `realclock = 0` alongside it still wins for
anyone deliberately measuring the synthetic behaviour.

A cap never free-runs, which is why `120` on the real clock still delivers 60 fps. So a capped
rate above 60 is either wrong or pointless depending on the clock, with no third case, which is
what the rejection message says.

**The tick divider must be 1 under free-run.** `gePortSimAlpha()` is `phase / divider`, so a
divider above 1 blends the camera against a frame phase rather than a fraction of elapsed time,
and under a real clock those are unrelated. It presents as flicker rather than as a wrong number.
Elapsed time already gates the simulation there. Fixed in `0009-freerun-divider.patch`.

**The cost of `off` is reproducibility.** Elapsed fields become load-dependent, so no two runs
are frame-for-frame comparable and measurement harnesses should stay on 60.

`framerate=30` additionally sets `GETV_TICKFIELDS=2`, so each update reports two elapsed fields.
Game time stays real, thirty updates a second times two fields being sixty, while the
frame-counted systems drop to 30 Hz.

**What is still frame-counted.** Only 13 of the 135 translation units under `src/game` scale by
`g_GlobalTimerDelta`. Automatic fire is converted and time-based by default on both the player
and the AI side (`GETV_TIMEFIRE`, `gunfire.c` and `chraction.c`); the rest still advance once per
update.

`GETV_FPS`, `GETV_REALCLOCK` and `GETV_TICKFIELDS` in the environment all reach the pacing code
directly and override the pairings above.

## Live keybindings

A handful of settings can change during a running game, no restart needed, bound directly in
`gfx_sdl2.c`'s `gfx_sdl_onkeydown()` rather than exposed through any menu. Everything else in this
file is read once at startup (a `getenv` call or an `__attribute__((constructor))`, see
`docs/PERFECT_DARK.md` section 6 row 17 for the full list of what cannot honestly be made live
without a restart) and stays fixed for the process lifetime.

| Key | Effect |
|---|---|
| `F11`, `Alt+Enter` (Windows), `Cmd+F` (macOS) | Toggle fullscreen |
| `F9` | Toggle vsync |
| `F5` / `F6` | FOV -10% / +10%, clamped to 50-160% |

Fullscreen and vsync both flip `configWindow` fields and set `configWindow.settings_changed`,
the same apply path a window resize already goes through. FOV bypasses its cached `GETV_FOV`
env read entirely once touched (`gePortSetFovScale()`, `fr.c`) - the projection is recomputed
from it fresh every frame per viewport regardless, so nothing downstream needs telling.

This is deliberately not a menu. An in-game options page was considered and scoped down: the
Watch's data-driven settings page (`options.c`) is the only reusable one in the codebase, and
adding a page to it is blocked by an explicit maintainer comment on `WATCH_NUMBER_SCREENS`
(`options.h`) not to change that constant until `struct player` is fully shiftable. See
`docs/PERFECT_DARK.md` section 6 row 17.

## Controls

### `controls`

Selects one of Rare's eight control styles, by number or by name:

| | | |
|---|---|---|
| `1.1` `honey` | `1.2` `solitaire` | one controller |
| `1.3` `kissy` | `1.4` `goodnight` | one controller |
| `2.1` `plenty` | `2.2` `galore` | two controllers |
| `2.3` `domino` | `2.4` `goodhead` | two controllers |

`2.2 galore` and `2.4 goodhead` are the true dual-analog layouts. The port's built-in default is
`2.2`, and the written template sets `2.2` as well - one physical gamepad is presented as N64
ports 0 and 1, left stick moving and right stick looking. Rare's own shipped default is `1.1`.

Selecting a two-controller style prints a note. With three or four players the game forces
everyone back to `1.1`.

### `gamepad`

`auto`, `xbox`, `playstation`, `switch` or `generic`. Default `auto`.

**This only changes which glyphs are printed for on-screen prompts.** It never changes what any
binding does. Set it when SDL misidentifies a third-party pad.

### Button bindings

`fire`, `aim`, `use`, `weapon_next`, `weapon_prev`, `pause`.

Each accepts one of: `a`, `b`, `x`, `y`, `lb`, `rb`, `lt`, `rt`, `start`, `back`, `none`.

| Key | Default |
|---|---|
| `fire` | `rt` |
| `aim` | `lt` |
| `use` | `b` |
| `weapon_next` | `a` |
| `weapon_prev` | `none` |
| `pause` | `start` |

**Button names are positional, not label-based.** `a` always means the physically bottom face
button on the pad, whatever that button is printed with - SDL maps the bottom face button to its
`A` slot on every controller it knows, including Nintendo's, where the same button is labelled
`B`. The `gamepad` profile above affects prompts only, so it cannot make `a` refer to a different
physical button.

`fire = rt` / `aim = lt` is the modern-shooter convention rather than a settled fact; GoldenEye's
retail scheme has neither. Swapping them is one line: `fire = lt`, `aim = rt`.

`weapon_prev` defaults to `none` deliberately. GoldenEye has no back-cycle button - the retail
gesture is hold-inventory plus tap-fire. The synthesised single-button version is faithful to that
gesture but has not been verified against real hardware, so it stays opt-in.

### Per-player bindings

Prefix any of the six with `p1.` to `p4.` to set it for one player only:

```
fire     = lt        # all four players
p2.fire  = rb        # except player 2
p3.aim   = x
```

Resolution is three steps, in order: `p<n>.<action>` if set, else the bare `<action>`, else the
default in the table above. So the plain keys still mean "all four players" and nothing that was
configured before this existed changes.

Split-screen is the reason. With one global table, moving fire off the right trigger for a player
on a Nintendo pad moved it for everyone, so a mixed set of controllers could not be accommodated
at all.

The environment spelling is `GETV_P2_BIND_FIRE`, alongside the existing `GETV_BIND_FIRE`.

What each player actually resolved to is printed at startup. Player 1 is always shown; the others
appear only when they differ from it, so an override is impossible to miss and the common case
stays one line:

```
[getv] input: bindings resolved, player 1 -- fire=lt aim=lt use=b weapon_next=a weapon_prev=none pause=start
[getv] input: bindings resolved, player 2 -- fire=rb aim=lt use=b weapon_next=a weapon_prev=none pause=start
```

### `deadzone`

Stick deadzone as a percentage of the raw SDL axis, `0` to `40`. Out-of-range values are clamped
rather than rejected, matching what the input layer does.

The built-in default is roughly 9.8% (3200 of 32767 counts). The written template sets `20`, so on
a fresh install the effective value is 20 unless you change it.

This is the port's deadzone on the raw axis. It is not the game's own aim and walk thresholds,
which are applied downstream in N64 counts and are left alone.

### `invert_look`

`0` or `1`. Also spelled `invertlook`.

**Unset is not the same as `0`.** When the key is absent nothing is written, and the game's own
Look Up/Down option in the save file decides - which is retail behaviour. Setting `0` is an
explicit override to non-inverted. Setting `1` forces inversion.

The written template sets `1`, and that is a measured decision rather than a preference. Retail's
default options omit the invert-look flag, which makes stick-up drive pitch down at full rate; the
camera pins at the -90 degree clamp in about a second and a half with nothing to recentre it. A
fresh install opened staring at the floor, and it was reported as a bug twice. Comment the line out
for retail behaviour.

## Gameplay and system

### `cheats`

A comma-separated list of named cheats. See [`CHEATS.md`](CHEATS.md).

### `roster`

`8` or `64`. Multiplayer character count. `8` is the shipped default and does nothing. `64`
unlocks the full character list.

`33` is refused rather than faked. The 33-character roster is derived from the save file - it
unlocks by completing Cradle on Agent - and the character-select screen recomputes it every frame
from that save for any value other than 64. Writing 33 would be overwritten on the first frame and
the setting would appear to do nothing.

### `unlock_all`

`0` or `1`. Also spelled `unlockall`. Default off. Shows every mission on the file-select screen.

### `audio`

`0` or `1`. Default on. `audio = 0` disables sound.

The underlying gate is presence-tested and inverted, so `audio = 0` sets it and `audio = 1` leaves
it unset. That detail matters only if you are setting the raw gate by hand.

### `save_dir`

A path. Overrides the save directory. Case is preserved. Default is
`~/Library/Application Support/Goldeneye-Native`; the EEPROM image is written as `eeprom.bin` inside
it. If the directory cannot be created, persistence is disabled and the game says so.

### `realclock`

`0` or `1`. Also spelled `real_clock`. Switches the port's clock source. Diagnostic.

### `debug_position`

`0` or `1`. Also spelled `debugpos`. Turns on Rare's own left-in readout: room id, collision
X/Y/Z and a compass heading, drawn every frame. It works in a stock build and does not need the
debug menu.

### `debug_menu`

Present only to explain that it does not work as a runtime setting. The leftover debug menu is
already compiled into every binary, but its trigger is gated on a macro that changes code
generation in two places - one of which repurposes the Start button. Setting the key prints the
rebuild command instead:

```bash
GETV_DEBUGMENU=1 ./build_mac.sh lib && ./build_mac.sh app
```

Note `lib`, not `port`: it is a game-object flag. The menu's level select does not work either;
those entries are gutted no-ops. Use `GETV_STAGE=<n>` to pick a level.

## Enhancement keys

Two of these are implemented. The rest validate, export their gate, and are then consumed by
nothing; they exist so the option surface is stable before the features land, and so a
configuration written today keeps working. Turning an unimplemented one on prints a
not-implemented notice rather than silently doing nothing.

**Implemented:**

| Key | Accepts | Effect |
|---|---|---|
| `coop` | 0-4, clamped | Load a single-player mission with this many players sharing it, split screen. `0` or `1` is normal solo play. Bring-up only: the mission's objectives, AI and cutscenes are written around one Bond, so the extra players are present rather than accounted for. Distinct from multiplayer, which uses its own arena setups; co-op keeps the mission's own setup file. |
| `fov` | 50-160, clamped | Vertical field of view as a percentage of the original, 100 being unchanged. The game re-sets the field of view every frame from the player's zoom state, so this is applied on the way through rather than set once. It deliberately does not alter the value the game reads back: `bondview2.c` computes `viGetFovY() / FOV_Y_F` in three places to make aiming finer as you zoom, so scaling the stored value would widen the view and retune aim sensitivity at the same time. Only the projection matrix sees the multiplier, so the view widens and aim behaves exactly as before. |
| `depth_bits` | 16-32, clamped | Requested depth-buffer width. Note that the driver decides: on Apple silicon the context comes back 32-bit whatever is asked for, including 16, so this cannot currently be used to reproduce N64 z-fighting. The obtained width is printed at startup as `[getv][gl] depth buffer N-bit`. |
| `anisotropic` | 0-16, clamped | Anisotropic filtering, off by default. Clamped again at runtime to the driver's own maximum, since asking for more than the hardware offers is a GL error rather than a silent downgrade: on this machine 64 becomes 16. Applied only where the game already chose linear filtering, so the HUD, the watch faces and text keep point sampling and stay sharp. |
| `msaa` | 0-8, clamped | Multisampling, off by default. Verified working at 4 samples; the obtained sample count is printed at startup. The N64 had its own anti-aliasing and this port otherwise has none. |
| `mipmaps` | 0 \| 1 | Trilinear filtering, off by default. Distant textures blend toward a mip level instead of shimmering; `anisotropic` is what sharpens that back up at grazing angles, so the two are meant to be tuned together. Only affects minification -- GL has no magnification mipmap mode, so close-up textures are unaffected. |
| `fxaa` | 0 \| 1 | Edge antialiasing over the finished frame, off by default. An image-quality setting like `msaa` rather than a look, which is why it lives here and the CRT terms live in `mods/crt_screen`. |
| `parallax` | 0 \| 1 | On by default, and does nothing on its own. It decides whether a texture pack's `<hash>_h.png` height maps displace the diffuse UVs. There is no height data in the game's own assets, so with no pack this changes nothing either way. `97 Console` turns it off so the same installed pack means resolution only. |
| `crosshair_scale` | 0.25 to 2.0 | Default `1.0`, the retail sight size exactly. Alias `reticle_scale`. Applied after the 16:9 and PAL aspect corrections in `gunDrawSight()`, so the shape never changes and only the size does. The 1997 sight was 32 pixels against a 320x240 field of view on a CRT across a room; at 1280x960 on a desk it covers rather more of what you are aiming at. GoldenEye+ asks for `0.6`. Out-of-range values are refused rather than clamped, so a typo is reported instead of silently becoming something else. |
| `crosshair_color` | RRGGBB hex | Default `FFFFFF`, retail's own hardcoded value -- `gunfire.c`'s `gunDrawSight()` multiplies the sight texture by this RDP primitive colour instead of always white. How cleanly a non-white choice recolours it depends on the baked N64 asset, which this port has not independently confirmed; see `port_support.c`'s `GETV_CROSSHAIR_COLOR` comment. |



## The launcher

`./getv/build-mac/goldeneye --launcher` (or `GETV_LAUNCHER=1`) opens a window for choosing a
level, a ruleset, cheats and video settings before the game starts. On macOS the desktop
script `GoldenEye.command` uses it.

It is a user interface over the existing surface, not new capability: every control resolves
to a `GETV_*` gate that already worked from a shell, and each one opens showing the value the
config layer just resolved, so the launcher reflects `goldeneye.cfg` rather than competing
with it. It does not write the config file.

**Why it restarts the game rather than applying settings in place.** 76 of the `GETV_` gates
are read once into a `static` on first use, so a setting changed after the game has started
does nothing for most of the surface -- silently. The launcher therefore sets the environment
and re-executes the binary with `--launcher` removed, so the game begins in a process where
nothing has been read yet. `GETV_LAUNCHER` and `GETV_LAUNCHER_AUTOPLAY` are cleared before
that exec, or a `launcher = 1` left in a config would reopen the launcher forever.

Cheats cross that boundary through **`GETV_CHEATS`**, a comma-separated list using the same
names as the `cheats` key. It exists because cheats are the one part of the config that is not
a gate: they are written straight into the game's cheat array at parse time, which a new
process would otherwise lose. Cheats whose effect lives in the game's turn-on switch are
marked "(in-game)" in the launcher, because they need a player context that does not exist at
startup and a checkbox that silently does nothing is worse than one that says so.

**Profiles.** *Faithful* is the default and clears the enhancements rather than merely not
setting them, so switching back cannot leave one behind. *GoldenEye+* raises FOV, MSAA,
anisotropic filtering, mipmapping and supersampling -- only things this port has implemented
and verified. It enables nothing from the reserved-and-inert list.

Two testing gates, both off by default:

| gate | what it does |
|---|---|
| `GETV_LAUNCHER_AUTOPLAY=1` | takes the launcher's path without opening a window: read the environment, write it back, re-exec. The same code the Play button runs, for checking that settings survive the exec. |
| `GETV_LAUNCHER_PROBE=<frames>` | draws that many frames, counts pixels differing from the clear colour, reports and closes. Distinguishes a drawn UI from an empty window that merely failed to error. |

## Rulesets

A ruleset scales values the game already reads. No level, model, setup file or asset is
involved, which is why these cost almost nothing to add and can be combined freely.

`ruleset = classic | hardcore | survival | chaos | horde`

| preset | what it does |
|---|---|
| `classic` | the game as shipped. The default, and completely silent. |
| `hardcore` | enemy health 200%, damage 150%, accuracy 130%; player health 50%; ammo 50% |
| `survival` | hardcore-lite (150/125/115, player 75%, ammo 75%) with endless waves |
| `chaos` | everything turned up: enemies 300/200/150, player 200%, ammo 300% |
| `horde` | stock difficulty, double ammo, endless waves |

Individual keys override whatever the preset chose, so `ruleset = hardcore` plus
`ammo = 200` is a hardcore run with generous ammo. All are percentages, where 100 is
unmodified:

`enemy_health` · `enemy_damage` · `enemy_accuracy` · `enemy_reaction` ·
`player_health` · `player_armour` · `ammo` · `explosion_damage` · `turret_damage`

Horde: `horde = 0 | 1`, tuned with the gates `GETV_HORDE_PER_KILL` (default 1),
`GETV_HORDE_PER_KILL_CAP` (3), `GETV_HORDE_MAX_ALIVE` (12), `GETV_HORDE_WAVE_KILLS` (10)
and `GETV_HORDE_GROWTH` (1). When a guard dies, replacements spawn where it fell using the
engine's own `chrSpawnAtCoord`, inheriting the dead guard's body and AI list; the wave
number rises every `wave_kills` kills and adds `growth` to the spawn count, up to the cap.

**Spawning can be refused, and that is not an error.** `g_ChrSlots` is allocated with only
`(guard count + 10)` entries and the engine declines to spawn with fewer than three free, so
the real ceiling belongs to the level. A refused spawn leaves the wave smaller rather than
failing.

**Two of these are inverted internally**, and the implementation compensates so the
user-facing name means what it says. `enemy_health` divides `g_AiHealthModifier`, because
that global scales damage dealt *to* a guard; `player_health` multiplies `actual_health`,
because `bondhealth` falls by `damage / actual_health`. `enemy_reaction` is documented
without a difficulty claim: it scales the upper bound of a randomised AI timer, and which
direction feels harder has not been measured.

**Verifying a ruleset took effect.** Any non-stock ruleset prints once at level load, both
what was requested and what the engine ended up holding:

```
[getv][ruleset] "hardcore" -- tougher guards, less ammo, half the player health
[getv][ruleset]   enemy: health 200% damage 150% accuracy 130% reaction 100%
[getv][ruleset]   player: health 50% armour 100% | ammo 50% explosion 100% turret 100%
[getv][ruleset] applied: aiHealth=1.000 aiDamage=0.750 aiAccuracy=0.780 ... ammo=1.000
```

The second line is the claim and the `applied:` line is the measurement. On Agent, stock
`aiHealth` is 2.000, so hardcore's 1.000 is guards taking half the damage they used to.

`GETV_HORDE_SELFTEST=<frame>` spawns one replacement from a live guard at that tick without
a kill having happened. It exists because combat cannot be driven reliably from a headless
run, and it exercises the same spawn path a real death does.

### `preset` -- the GoldenEye+ profile

`preset = plus` is one switch for everything this port has added and verified. It accepts
`faithful` (aliases `97`, `console`) and `plus` (aliases `enhanced`, `goldeneye+`, `ge+`).

| It turns on | Value |
|---|---|
| `supersample` | 2 |
| `msaa` | 4 |
| `anisotropic` | 8 |
| `mipmaps` | on |
| `hd_textures` | on |
| `parallax` | on |
| `fxaa` | on |
| `crosshair_scale` | 0.6 |
| `framerate` | off, with the real clock |

The profile fills gaps and displaces nothing:

```
command line  >  environment  >  your own config lines  >  preset
```

So `preset = plus` followed by `fxaa = 0` gives the whole profile without FXAA, wherever the
two lines sit relative to each other. Anything the profile wanted but found already set is
named on stdout at startup, rather than passed over quietly, because a preset that silently
declined to uncap the frame rate looks exactly like a preset that did not work.

The generated config ships `supersample` and `framerate` commented out for that reason. A
config written before this existed has them as live lines, and the startup message will say
so; comment them out to let the profile have them.

Faithful is and stays the default. The N64 look is the product, and the way correctness gets
checked here is comparison against real N64 captures, so anything that alters output has to be
something you asked for.

**Reserved, parsed but inert:**

These are the ones that still do nothing. They parse and validate so the option surface is
stable before the features land.

| Key | Accepts | Intended effect |
|---|---|---|
| `fog_per_pixel` | 0 \| 1 | Per-pixel fog. N64 fog is per-vertex. |
| `muzzle_lights` | 0 \| 1 | Dynamic lighting on muzzle flashes. |
| `audio_3d` | 0 \| 1 | Positional audio / HRTF. Alias `hrtf`. |
| `ssao` | 0 \| 1 | Screen-space ambient occlusion. |
| `shadows` | 0 \| 1 | Real-time shadow maps. The game ships blob shadows. |
| `per_pixel_lighting` | 0 \| 1 | Per-pixel lighting. N64 lighting is per-vertex Gouraud, so this changes the look the most of anything on the list. |

Integer keys are clamped to their range rather than rejected.

## Raw gates

Any of the port's development gates can be set by its real name, in the file or on the command
line:

```
GETV_STAGE = 34
GETV_EXIT_FRAME = 61
```

```bash
./build-mac/goldeneye --GETV_STAGE=34
```

Raw names are matched before friendly ones, so a friendly key can never shadow a gate. There are
around 250 of them; [`MODDING.md`](MODDING.md) covers the useful ones.

### `GETV_RGBA16BE` -- explosion colour

Default 1, and you should not need to touch it. RGBA16 textures were being decoded in the wrong
byte order, which turned explosions magenta and read on screen as confetti. Mode 1 corrects it,
mode 0 is the old behaviour, mode 2 is a control kept for comparison.

It is listed here because the symptom was reported often enough to be worth naming: coloured
confetti on crate and barrel explosions is this, and it is not the paintball cheat. See
[`COLOUR_BUGS.md`](COLOUR_BUGS.md) for the measurements.

### `GETV_REAL_FONTS` -- the real-font text overlay

Off by default, and a raw gate rather than a friendly key because it is not finished enough to
promote. `GETV_REAL_FONTS=1` draws `textRender`/`textRenderOutlined` strings through a
stb_truetype atlas baked from `getv/port/assets/fonts/RobotoCondensed-VF.ttf` instead of the
game's own bitmap glyphs, which are 24-pixel N64 assets being stretched at desktop resolutions.

It prints what it did at startup, so you can tell the difference between off and broken:

```
[getv][text] real-font overlay ready: .../RobotoCondensed-VF.ttf, 95 chars baked at 24px (46 atlas rows)
```

If the font is missing it says so and falls back to the bitmap glyphs rather than drawing
nothing. Rotated text (the file-select folder tabs) is deliberately exempt and still draws
through the original path.

Two known differences, both cosmetic and both written up in
`getv/port/src/ge_text_overlay.c`:

- Menu highlight boxes are positioned from the bitmap font's metrics, so by the
  difficulty-select screen (`GETV_MENU=8`) the box has visibly drifted from its label.
- Some strings change case. The Q Watch pause screen reads `Q WATCH V2.01 BETA` under the
  bitmap font and `q watch v2.01 beta` under the overlay, because the string in the game's data
  is already lowercase and the bitmap glyphs render it case-insensitively. Real-hardware
  captures show the uppercase form, so here the overlay is the one that differs from retail.
