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

### `framerate`

`30`, `50`, `60`, or `off` (`0`, `uncapped` and `unlimited` are accepted for the last). Default
`60` on NTSC builds, `50` on PAL. `off` removes the frame cap; vsync still applies.

**Values above 60 are rejected.** GoldenEye's timestep is whole video frames, not seconds, and
each update asks how many fields have elapsed. Render twice as often without telling the game and
every frame-counted system runs at double speed, with nothing to clamp or complain.

**`30` is the more faithful setting for gameplay, and `60` the smoother one.** They differ in more
than frame rate. Only 13 of the 135 translation units under `src/game` scale by
`g_GlobalTimerDelta` - animation, recoil, sway, camera. The other 122 advance once per update, so
an enemy's rate of fire is a frame count rather than a duration (`chraction.c:6694` fires on
`firecount % automaticFiringRate`). Hardware ran those at the N64's real 20 to 30 fps; at a locked
60 they run about twice as fast, which shows as turrets and guards firing too quickly, ammunition
draining too quickly, and AI stepping faster than it was tuned for.

`framerate=30` therefore also sets `GETV_TICKFIELDS=2`, which makes each update report two elapsed
fields. Game time stays real - thirty updates a second times two fields is sixty fields a second,
so animation and the mission clock are unchanged - while the frame-counted systems drop to 30 Hz,
close to the cadence the game was built around. Earlier builds capped the renderer without this and
ran at half speed; that was a defect, not an inherent property, and the warning that described it
as inherent is gone.

Neither value is correct for everything. Sixty renders smoothly and runs frame-counted gameplay
fast; thirty runs gameplay at the right cadence and renders less smoothly. Having both right at
once needs a fixed simulation tick with interpolated presentation, which this build does not have.

The rejection lives in the configuration layer only. `GETV_FPS=120` in the environment still
reaches the pacing code untouched, as does `GETV_TICKFIELDS`, which overrides the pairing above.

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
anisotropic filtering and supersampling -- only things this port has implemented and
verified. It enables nothing from the reserved-and-inert list.

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

⚠️ **Spawning can be refused, and that is not an error.** `g_ChrSlots` is allocated with only
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

**Reserved, parsed but inert:**

All of them default off. That is deliberate: the N64 look is the product, and the project's
correctness checks are comparisons against real N64 captures, so anything that silently alters
output destroys the ability to check the port is right. Enhancements are options, never defaults.

| Key | Accepts | Intended effect |
|---|---|---|
| `preset` | `faithful` \| `enhanced` | One switch for the whole set below. |
| `mipmaps` | 0 \| 1 | Mipmapping and LOD bias. |
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
