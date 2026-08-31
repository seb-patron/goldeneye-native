# Configuration

Every key, which are implemented and which are reserved, is in
[`docs/CONFIGURATION.md`](https://github.com/seb-patron/goldeneye-native/blob/main/docs/CONFIGURATION.md).
This page covers where the file lives, how precedence works, and the keys most people change.

## Where it lives

On first run, with no configuration file present anywhere, the game writes a commented template
and immediately reads it back:

```
~/Library/Application Support/GoldenEye/goldeneye.cfg
```

That write is not a convenience. Several of the port's tuned defaults exist only in that
template, `invert_look` being the case in point, and a default living in a file nobody has
generated is not a default. Regenerate it any time with `--write-config`.

Save data is separate, and lands in
`~/Library/Application Support/Goldeneye-Native/eeprom.bin`. It is 512 bytes, because GoldenEye
saves to the cartridge's serial EEPROM, and writes are atomic.

## Precedence

```
command line  >  GETV_* environment  >  the config file  >  built-in defaults
```

An environment variable always beats the file, which is what keeps measurement harnesses
working unchanged regardless of what a person has in their config.

## The keys worth knowing

| Key | Values | Default |
|---|---|---|
| `resolution` | `WIDTHxHEIGHT`, `fullscreen`, `native` | `1280x960` |
| `widescreen` | `0`, `1` | `1` |
| `supersample` | `1`, `2` | `1` |
| `filtering` | `point`, `bilinear`, `three-point` | `three-point` |
| `framerate` | `20`-`480`, or `off` | `60` |
| `controls` | any of Rare's eight styles, by number or name | `2.2 galore` |
| `roster` | `8`, `64` | `8` |
| `invert_look` | `0`, `1` | `1` |
| `crosshair_color` | `RRGGBB` hex | `FFFFFF` |
| `audio` | `0`, `1` | `1` |

**`framerate` accepts 20 to 480.** GoldenEye counts in whole video frames, so rendering twice
as often used to run the world twice as fast. Above 60 the simulation tick divider is now
chosen from this value so the game stays near the 30Hz it was authored for, with the skipped
frames' elapsed fields handed to the tick that runs and the camera interpolated between ticks.
60 and below are unchanged. See [Frame timing](Frame-timing).

**`invert_look = 1` is a measured default, not a preference.** The game's own default options
omit the invert-look flag, which makes stick-up drive pitch down at full rate and pin at the
clamp in about a second and a half. The camera ends up staring at the floor with nothing to
recentre it. Comment the line out for retail behaviour.

**`crosshair_color`** is RRGGBB hex, and the launcher has a colour picker for it. The RDP
already multiplied the sight texture by a primitive colour at that draw call and retail passed
white, meaning "show the texture unmodified", so this sets that value rather than adding
anything. How cleanly a given colour reads depends on the baked asset underneath, which is
easiest to judge by just trying it.

**`controls = 2.2 galore`** is a two-controller style in the original. On one modern gamepad it
is the dual-analog layout, so one physical pad is presented as N64 ports 0 and 1: right stick
looks, left stick moves. Set `1.1 honey` for Rare's shipped single-controller scheme.

## Bindings

Binding values are positional rather than label-based. `a` always means the bottom face button,
whatever it is printed as, because SDL maps the physically-bottom button to `BUTTON_A` on every
controller it knows, Nintendo pads included, where that same button reads "B".

`gamepad` picks which glyphs get printed in prompts. It never changes what a binding does.

`weapon_prev` defaults to none on purpose. GoldenEye has no back-cycle button; the retail
gesture is hold-inventory and tap-fire. The synthesised single-button version is faithful to
that gesture but unverified on hardware, so it stays opt-in.

## Live keys

Three settings can be changed while the game is running, on the same F-row as the existing F11
fullscreen toggle:

| Key | Effect |
|---|---|
| `F11` | Fullscreen |
| `F9` | Toggle vsync |
| `F5` / `F6` | Field of view, -10% and +10%, clamped to 50-160% |

These are keybindings rather than an in-game options page for a specific reason. The Watch menu
would have been the natural home, but `options.h`'s `WATCH_NUMBER_SCREENS` carries an explicit
warning from the decomp itself not to change it until the player struct is fully shiftable,
because the per-page selector rectangles are sized off it inside `struct player`. A keybinding
needs none of that: no new page, no touched vendor struct.

## Gates

Roughly 275 `GETV_*` environment gates are the practical extension surface, and each defaults to
preserving stock behaviour. They exist so a behaviour change can be A/B tested against the same
binary rather than argued about.

A few that come up often:

| Gate | What it does |
|---|---|
| `GETV_STAGE=<n>` | boot straight into a stage |
| `GETV_EXIT_FRAME=<n>` | end the run after n rendered frames |
| `GETV_SHOTFRAME` / `GETV_SHOTPATH` | write one frame to a file at an exact frame index |
| `GETV_SIMDIV=<n>` | tick the simulation one frame in n |
| `GETV_INTERP=0` | turn off camera interpolation |
| `GETV_VSYNC=0` | release the swap interval |
| `GETV_SHOWTEX=1|2` | draw raw texel0, or raw shade, bypassing the combiner |

`GETV_EXIT_FRAME` and `GETV_SHOTFRAME` are the pair that make rendering comparable between two
builds. Both end on an exact frame rather than an exact wall-clock time, which is the difference
between a measurement and a coin toss on a loaded machine.

Any gate can also be set by its real name in the config file, for example `GETV_STAGE = 34`.
