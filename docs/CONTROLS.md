# Controls

GoldenEye-Native accepts keyboard, mouse, and SDL2-compatible gamepads at the same time. A
connected gamepad does not disable keyboard or mouse input.

Gamepad actions can be rebound globally or per player. The physical keyboard layout is currently
fixed; there is no keyboard-key picker or config syntax for assigning an arbitrary key.

## Keyboard and mouse

| Input | Default action |
|---|---|
| `W` `A` `S` `D` | Move |
| Arrow keys | Look |
| `Space` or `Left Ctrl` | Fire |
| `Q` | Aim |
| `E` or `F` | Use, open, activate, or plant |
| `R` or `Return` | Inventory / next weapon; `Return` also confirms menu items |
| `Tab` or keypad `Enter` | Start / pause / watch |
| `Backspace` | Back |
| `I` `J` `K` `L` | D-pad up / left / down / right |
| `Z` / `X` | N64 left / right shoulder input |
| `C` or `Left Shift` | Crouch |
| `V` | Stand |
| Mouse movement | Look |
| Left mouse button | Fire |
| Right mouse button | Aim |
| `Escape` | Release or recapture the mouse cursor |

The keyboard map comes from `geKeyboardApply()` and the crouch helpers in
`getv/port/src/port_input.c`. It is also summarized in the launcher and printed at startup.

Mouse and keyboard are enabled by default. The launcher exposes mouse sensitivity, mouse Y
inversion, and an enable/disable switch for both devices. The equivalent raw settings are:

```ini
GETV_MOUSE = 1
GETV_MOUSE_SENS = 100
GETV_MOUSE_INVERT = 0
GETV_KEYBOARD = 1
```

`GETV_MOUSE_SENS` is a percentage and is clamped to `1` through `1000`. Set
`GETV_CROUCH_KEY = 0` to disable the dedicated crouch/stand keys and retain only the original
in-game crouch gesture.

### Can keyboard keys be rebound?

Not individually. The keyboard generates the same virtual gamepad inputs as a controller, so the
action bindings described below can change their meaning indirectly. For example, changing
`fire = lt` and `aim = rt` makes the fixed `Q`/right-mouse input fire and the fixed
`Space`/left-mouse input aim. That remap also affects gamepads and is not an arbitrary keyboard
binding system.

Adding true keyboard rebinding would require a key-to-virtual-input configuration layer in
`port_input.c` and corresponding launcher controls. Until then, the keys in the table above are
the supported physical layout.

## Gamepad defaults

SDL2-recognized Xbox, PlayStation, Nintendo, MFi, and generic controllers are detected
automatically. The default modern layout is:

| Action | Default physical input |
|---|---|
| Move | Left stick |
| Look | Right stick |
| Fire | Right trigger (`rt`) |
| Aim | Left trigger (`lt`) |
| Use | Right face button (`b`) |
| Next weapon | Bottom face button (`a`) |
| Previous weapon | Unbound |
| Pause / watch | Start |

Button names are positional, not label-based. SDL calls the bottom face button `a` even on a
Nintendo controller where the printed label is B. The accepted binding values are:

```text
a b x y lb rb lt rt start back none
```

The `gamepad` setting (`auto`, `xbox`, `playstation`, `switch`, or `generic`) changes only the
prompt glyphs. It does not change bindings.

## Change gamepad bindings

### In the launcher

Start the game with `--launcher`, open **Controls**, choose **ALL** or a player tab, and select a
source for each action. Applying the settings restarts the game so every input consumer sees the
new values from startup.

### In `goldeneye.cfg`

The exact config path is printed at startup. These are the default action bindings:

```ini
fire        = rt
aim         = lt
use         = b
weapon_next = a
weapon_prev = none
pause       = start
```

Prefix an action with `p1.` through `p4.` to override one player while leaving the global value as
the fallback:

```ini
fire       = rt
p2.fire    = rb
p3.aim     = x
p4.pause   = back
```

Resolution order is per-player value, then global value, then built-in default. The startup log
prints player 1's resolved bindings and prints any other player whose bindings differ.

### On the command line

Every config key also works as a one-run command-line option:

```bash
./getv/build-mac/goldeneye --fire=lt --aim=rt --p2.fire=rb
```

Environment variables are also accepted. For example, the config key `p2.fire` maps to
`GETV_P2_BIND_FIRE`.

## Original control styles

`controls` selects one of Rare's eight retail layouts:

| One N64 controller | Two N64 controllers |
|---|---|
| `1.1` / `honey` | `2.1` / `plenty` |
| `1.2` / `solitaire` | `2.2` / `galore` |
| `1.3` / `kissy` | `2.3` / `domino` |
| `1.4` / `goodnight` | `2.4` / `goodhead` |

The port defaults to `2.2 galore`. With one physical modern gamepad, the port presents its two
sticks as the two N64 controllers that layout expects: right stick/look on N64 port 0 and left
stick/move on port 1. `2.4 goodhead` is the other true dual-analog layout. Use:

```ini
controls = 1.1
```

for Rare's original one-controller default. Three- and four-player split-screen forces players
back to `1.1` because the two-controller layouts would require more N64 ports than exist.

## Stick tuning

```ini
deadzone    = 20
invert_look = 1
```

`deadzone` is a percentage from `0` through `40`; out-of-range values are clamped. It applies to
each raw SDL stick axis before conversion to the N64's `-80` through `80` range.

`invert_look` overrides the game's saved Look Up/Down choice. Leaving the key absent lets the save
file decide. The generated config template explicitly writes `1` because the retail default is a
poor fit for the port's modern dual-stick layout; see [`CONFIGURATION.md`](CONFIGURATION.md#invert_look)
for the measured rationale.

Mouse Y inversion is independent and uses `GETV_MOUSE_INVERT` or the launcher checkbox.

## Live desktop shortcuts

| Shortcut | Effect |
|---|---|
| `F5` / `F6` | Decrease / increase field of view by 10%, clamped to 50-160% |
| `F8` | Toggle the free camera when free camera is enabled |
| `F9` | Toggle vertical sync |
| `F11` | Toggle fullscreen |
| `Alt`+`Enter` | Toggle fullscreen |
| `Cmd`+`F` on macOS | Toggle fullscreen |

Free-camera movement uses `W` `A` `S` `D`, arrow keys to look, `R`/`F` to rise and descend, and
Shift or Ctrl to change speed. The game continues running, and those keys still feed their normal
gameplay inputs while the camera is active. Enable the feature before launch with
`GETV_FREECAM = 1` in the config file or `--GETV_FREECAM=1` on the command line; `F8` then toggles
it during play.

Most other settings are startup-only. Change them in the launcher, config, environment, or command
line and restart the process. The complete setting reference is
[`CONFIGURATION.md`](CONFIGURATION.md).
