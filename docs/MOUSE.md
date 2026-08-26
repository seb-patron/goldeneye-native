# Mouse look: why it was unusable, and the numbers behind the fix

Reported as "I have to move the mouse so far to move it barely an inch". That was accurate
and understated. At a gentle 0.9 in/sec on an 800 DPI mouse, a 180 degree turn took roughly
**306 metres of desk**. It now takes **6 cm**.

Three separate faults, each measured with `GETV_MOUSE_SELFTEST`, which injects a fixed number
of mouse counts per frame so the whole path can be measured without a hand on the mouse. It
runs ahead of the idle gate and never captures the pointer, so a measurement run cannot steal
the cursor from whoever is using the machine.

## 1. The deadzone, which was the big one

`port_os.c` discards any axis under `GE_STICK_DEADZONE`, 20% of the axis. That is right for a
spring-loaded stick that rests near centre and drifts, and wrong for a mouse, which has
neither a rest position nor drift.

At the old gain a gentle movement produced 6241 against a threshold of 6553, so it was thrown
away **entirely**. Not attenuated -- discarded. Every movement below the threshold did nothing
at all, which is why sweeping further was the only thing that appeared to work.

```
counts/frame 2 6 12 30 63
before ~0 ~0 ~0 0.35 3.50 deg/frame
```

Magnitude is now remapped from [1, max] onto [deadzone, max], so full scale stays full scale
and the smallest real movement produces the smallest real turn. **Applied only to the mouse**;
a physical stick keeps its deadzone, which it needs.

## 2. A displacement fed to a velocity control

The N64 stick is a velocity control. A mouse reports displacement. A flick therefore produced
full deflection for exactly one frame, and one frame of full stick is 3.54 degrees, so
everything beyond the first frame's worth of travel was clamped off and discarded.

Motion the stick cannot express this frame is now carried in an accumulator and emitted over
following frames, capped at four frames of full deflection so a hitch or an alt-tab return
cannot leave the view gliding after the hand has stopped.

## 3. A gain set against nothing

220 counts per full deflection, with no measurement behind it. Now 21, read off this sweep:

| `GETV_MOUSE_SENS` | slow (0.5 in/s) | gentle (0.9 in/s) | brisk (2.2 in/s) |
|---|---|---|---|
| 100 (old default) | 1208 cm | 96 cm | 22 cm |
| 200 | 48 cm | 15 cm | 4.9 cm |
| **300 → the new 100%** | **17 cm** | **6.0 cm** | **4.9 cm** |
| 450 | 5.7 cm | 2.4 cm | 4.9 cm |

300 was chosen because it holds up across speeds rather than being fastest at one. 450 is
twitchy, 200 leaves a 10x spread between slow and fast.

## What it measures at now

| counts/frame | in/sec @800dpi | deg/sec | cm of desk for 180 |
|---|---|---|---|
| 3 | 0.2 | 1.5 | 67.0 |
| 6 | 0.5 | 12.4 | 16.6 |
| 12 | 0.9 | 68.6 | 6.0 |
| 30 | 2.2 | 210.0 | 4.9 |
| 63 | 4.7 | 210.0 | 10.3 |

**The remaining spread is the game's own acceleration curve, not a defect.** GoldenEye ramps
turn speed with deflection, so slow movement stays slow -- which is what gives fine aim at 0.2
in/sec and a fast whip at 2.2. Flattening it would mean overriding the game's aim model, which
is a much larger decision than a sensitivity constant and is not being made here.

**210 deg/sec is a hard ceiling** -- it is the game's maximum turn rate at full deflection,
measured at 3.54 deg/frame. Past 30 counts/frame the accumulator spreads the surplus over
following frames rather than losing it, and that is why 63 reads slower per count than 30: the
turn is not lost, it is delivered over more frames.

## Both axes, and both at once

Checked rather than assumed, because the first sweep only injected horizontal movement and
"X works so Y works" is exactly the assumption that hides a bug. `GETV_MOUSE_SELFTEST_Y`
injects the vertical twin.

**Yaw and pitch turn at the same angular rate.** At 6 counts/frame: 0.210 deg/frame
horizontal, 0.206 vertical. That equality is the thing worth checking -- one axis quietly
geared differently from the other is the usual way this goes wrong.

**Pitch clamps at straight up and straight down, symmetrically**: +89.91 looking up, -89.90
looking down. That is the game's own aim limit and is correct. It is also why vertical *feels*
more sensitive than horizontal at the same setting: pitch spends its whole range in 180
degrees while yaw has 360 to cover, so the same rate crosses proportionally more of it.

**Movement and look run at once.** They are separate paths -- WASD drives the left stick,
which dual-analog routes to N64 port 1 as movement, while the mouse drives the right stick on
port 0 as look -- so neither can starve the other. Measured together over 260 frames:
**309.7 units travelled and 201.6 degrees turned in the same window.**

## Tuning

`GETV_MOUSE_SENS` is a percentage of the above, 1..1000. `GETV_MOUSE_INVERT=1` inverts Y.
`GETV_MOUSE=0` disables mouse look. ESC releases and recaptures the pointer.
