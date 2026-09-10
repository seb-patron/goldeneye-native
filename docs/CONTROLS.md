# Controls

GoldenEye-Native accepts keyboard, mouse, and SDL2-compatible gamepads at the same time. A
connected gamepad does not disable keyboard or mouse input.

**Everything is rebindable** — keys, mouse buttons, the wheel, and gamepad buttons, each
independently of the others. Bindings can be saved to `goldeneye.cfg` from the launcher so they
survive quitting.

## Presets

A preset supplies the defaults for both devices. An explicit binding always beats it, so pick the
one closest to what you want and change only what you care about.

```ini
input_preset = modern     # or: n64
```

**`modern`** (the default) is the layout a player coming from any shooter of the last fifteen
years expects. **`n64`** reproduces exactly what this port defaulted to before remapping existed,
so it is a true revert rather than an approximation — use it if you preferred the old keys.

Changing the preset in either launcher clears every binding you have set, because a preset is only
useful if it describes the whole layout.

## Keyboard and mouse

| Input | `modern` | `n64` |
|---|---|---|
| `W` `A` `S` `D` | Move | Move |
| Arrow keys | Look | Look |
| Left mouse button | Fire | Fire |
| Right mouse button | Aim | Aim |
| `Space` | Fire | Fire (also `Left Ctrl`) |
| `Q` | Next weapon | Aim |
| `E` or `F` | Use / interact | Use / interact |
| `R` | **Reload** | Next weapon (with `Return`) |
| `C` or `Left Ctrl` | Crouch | Crouch (`C` or `Left Shift`) |
| `V` | Stand | Stand |
| Mouse wheel up / down | Next / previous weapon | — |
| `Return` | Next weapon; also confirms menu items | same |
| `Tab` or keypad `Enter` | Start / pause / watch | same |
| `Escape` | Release or recapture the mouse cursor | same |

`Z` / `X` are the N64 left and right shoulder inputs and `I` `J` `K` `L` are the d-pad. Those are
not actions and are not bindable — nothing in the game reads them on their own.

`Return` stays on next weapon in both presets on purpose: it drives the N64 A button, which is
what confirms a `front.c` menu item. Unbinding it would leave a keyboard player unable to start a
mission.

After releasing the cursor or switching to another app, left-click inside the game to resume mouse
control. The resume click does not fire; release the mouse buttons before clicking to fire or aim
again. Pressing `Escape` again also recaptures the cursor. The developer console keeps control of
clicks and wheel notches while it is open.

### Rebinding a key

Each action takes a comma-separated list; any one of them fires it.

```ini
key.reload      = R
key.crouch      = C,Left Ctrl
key.aim         = mouse2
key.weapon_next = Q,wheelup,Return
key.weapon_prev = wheeldown
key.forward     = W
```

Names are SDL's own — `Left Ctrl`, `Space`, `Keypad Enter`, `F1` — and are case-insensitive. This
port adds `mouse1` through `mouse5`, `wheelup` and `wheeldown`, and accepts the short forms
`lctrl`, `rctrl`, `lshift`, `rshift`, `lalt`, `ralt`, `esc`, `enter`, `pgup`, `pgdn`, `kpenter`.
Use `none` to unbind. The resolved list is printed at startup, so a binding that failed to apply
is visible rather than silent.

The bindable actions are `fire`, `aim`, `use`, `reload`, `crouch`, `stand`, `weapon_next`,
`weapon_prev` and `pause`; the movement axes are `forward`, `backward`, `strafe_left`,
`strafe_right`, `look_up`, `look_down`, `look_left` and `look_right`.

`look_*` drive the right stick, which is also how a keyboard player moves the front-end menu
cursor — worth leaving bound even with the mouse on.

### Hold or toggle

```ini
aim_mode    = hold     # or: toggle
crouch_mode = hold     # or: toggle
```

Aim toggle sets GoldenEye's own per-player aim-control option, which the engine reads as a press
rather than a hold. It is the retail setting, not something bolted on top. Crouch toggle is the
port's, because the engine has no crouch button to latch.

In **hold** mode, releasing crouch now stands you up. Before remapping, crouch was momentary but
nothing watched the release, so a tap left you squatting until you found the separate stand key.

### Mouse

Mouse look defaults to **Modern** response: mouse distance directly controls the camera angle.
Choose **Classic N64** in the launcher's Controls page, or set `mouse_mode = classic`, for the
original stick acceleration and turn-speed limit. Controller response is unchanged. See
[MOUSE.md](MOUSE.md) for vehicle and network fallback behavior.

Mouse and keyboard are both enabled by default:

```ini
GETV_MOUSE = 1
GETV_MOUSE_SENS = 100
GETV_MOUSE_INVERT = 0
GETV_KEYBOARD = 1
```

`GETV_MOUSE_SENS` is a percentage clamped to `1` through `1000`.

The wheel is the one input the port cannot poll — SDL exposes wheel motion only as an event — so
notches are counted and released one per frame with a gap between them. The engine cycles weapons
on a rising edge, so three notches flicked inside a single frame would otherwise advance one
weapon instead of three. A backlog of more than eight notches is dropped, and the backlog is
discarded entirely whenever the game should not be reading input.

## Crouch, stand and reload

None of these three reaches the game through the N64 controller, because the engine has no button
for any of them. The port reads them back out of the binding table directly. The retail gestures
still work alongside them:

- **Crouch** — retail is aim mode plus stick down (`bondview2.c` requires `insightaimmode`), so
  crouching means holding aim, pushing down, then releasing aim while staying low.
- **Reload** — retail is the *use* button with nothing to use. `bond_interact_object()` returns
  true only when there is no prop in range, so `E` near a door opens the door and `E` near nothing
  reloads. A key bound to `reload` reloads wherever you are standing, which is what a key labelled
  R should do.

Set `crouch_key = 0` to remove the port's dedicated crouch and stand bindings entirely and keep
only the retail gesture.

## Gamepad

SDL2-recognized Xbox, PlayStation, Nintendo, MFi, and generic controllers are detected
automatically.

| Action | `modern` | `n64` |
|---|---|---|
| Move | Left stick | Left stick |
| Look | Right stick | Right stick |
| Fire | Right trigger (`rt`) | `rt` |
| Aim | Left trigger (`lt`) | `lt` |
| Use / interact | South face (`a`) | East face (`b`) |
| Reload | West face (`x`) | Unbound |
| Crouch | East face (`b`) | Unbound |
| Next weapon | North face (`y`) | South face (`a`) |
| Previous weapon | Unbound | Unbound |
| Pause / watch | `start` | `start` |

On a DualSense that makes `a` Cross, `b` Circle, `x` Square and `y` Triangle; on a Switch Pro, `a`
is the physically-bottom button, which is printed B.

Button names are positional, not label-based — SDL maps the physically-bottom face button to `a`
on every controller it knows. The accepted values are:

```text
a b x y lb rb lt rt start back dup ddown dleft dright lstick rstick none
```

The d-pad and the stick clicks (`lstick`, `rstick`) are bindable; they were not before.

`weapon_prev` is unbound on the pad on purpose. GoldenEye has no back-cycle button — the retail
gesture is hold-inventory plus tap-fire, which the port synthesises as a single `A`+`Z` frame.
That is faithful to the gesture but unverified on real hardware, so it stays opt-in on a face
button. The mouse wheel binds to it by default, where one notch is unambiguous.

The `gamepad` setting (`auto`, `xbox`, `playstation`, `switch`, `generic`) changes only the prompt
glyphs. It never changes what a binding does.

## Changing bindings

### In the launcher

Start with `--launcher` and open **Controls**. On macOS this is the SwiftUI launcher; elsewhere it
is the ImGui one. Both edit the same settings.

- **ImGui** — click a binding and press the key or mouse button you want. `Escape` cancels. The
  `x` button clears to the preset default, and clears again to unbound.
- **SwiftUI** — type the key name into the field. A binding can be a list, which a
  press-a-key capture cannot express, and the placeholder shows what the preset supplies.

**Press SAVE CONTROLS to keep them.** Everything else in the launcher lasts only until you quit —
settings are handed to the relaunched game as environment variables and nothing writes them down.
Save rewrites `goldeneye.cfg` in place: comments, ordering, settings from other pages, and any key
this build does not recognise are all left alone.

### In `goldeneye.cfg`

The exact config path is printed at startup.

```ini
input_preset = modern
aim_mode     = hold
crouch_mode  = hold

# gamepad
fire   = rt
reload = x
crouch = b

# keyboard and mouse
key.reload = R
key.crouch = C,Left Ctrl
```

Prefix a gamepad action with `p1.` through `p4.` to override one player while leaving the global
value as the fallback:

```ini
fire       = rt
p2.fire    = rb
p3.aim     = x
p4.pause   = back
```

Resolution order is per-player value, then global value, then the preset. The startup log prints
player 1's resolved bindings and prints any other player whose bindings differ. Keyboard bindings
are not per-player: a second keyboard is not something this port supports.

### On the command line

Every config key also works as a one-run command-line option:

```bash
./getv/build-mac/goldeneye --fire=lt --aim=rt --p2.fire=rb --key.reload=F
```

Environment variables are also accepted: `p2.fire` maps to `GETV_P2_BIND_FIRE`, and `key.reload`
to `GETV_KEY_RELOAD`.

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
