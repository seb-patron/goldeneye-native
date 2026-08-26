# Injecting input: which path actually works

There are two ways to drive a player without a human, and only one of them works. This is
written down because the broken one looks like it works, and both agents have measured against
it and drawn conclusions from it.

## The player API works

`gePlayerClaim(slot, GE_SLOT_INJECTED)` plus `gePlayerPost(...)` writes `OSContPad` straight into
the controller sample ring, one sample per main-loop pass, immediately before the game consumes
it. Measured on Bunker 1 with the route follower on slot 0: the player turns from 360 to 144
degrees, walks 155 units, and its distance to the target closes 2387 -> 2250. With the bot off,
`MoveBond`'s walk block does not execute at all.

**It needs the companion pad.** Under the 2.x control styles -- and this port defaults to 2.2
Galore -- the engine reads movement from a second controller at `playernum + getPlayerCount()`.
`ge_playback` routes the walk axis there in a second pass. In solo that pad is index 1, so bot
runs need `GETV_PADS=2` or `joyGetStickY(1)` returns 0 whatever was written.

## `GETV_SCRIPT` does not

**It has never been shown to move any player, and it does not.** Measured on Bunker 1, 1021
frames, `GETV_STATEAPI` sampled at the same two frames in every run:

```
control          pos=(-1376 280 2297) -> (-1364 327 2305)  ang 360.0 -> 332.0
SY=70            pos=(-1376 280 2297) -> (-1364 327 2305)  ang 360.0 -> 332.0
SX=70            identical
turn then walk   identical
```

Byte-identical to the control, including the angle. And at the input layer, `SY=70`, `SX=70`,
`SY=-70` and **no script at all** all produce the same `analogTurn=75 analogStrafe=0
analogWalk=0`. A sign flip that changes nothing is not an input reaching the game.

**Two traps in the measurements that were done against it before this was known.**

**Always run the control.** On Dam a scripted run travels 36 units and looks like proof --
until the no-script control travels the same 36 units, because Dam's opening walks the player
itself. Every "the script moved someone" result so far was the level's own intro.

**Never sample with `tail -1`.** `GETV_SCRIPT="400:SY=70:600"` stops holding at frame 1000, so a
run ending at 1201 spends its last 200 frames with no input, and the last trace line shows
`spd=0.000`. That reads as "the walk never worked" and is really "you looked after it ended".
The same trap as the `tris submitted` bimodality in the project's measurement notes, in a
different costume.

## What to do about it

Use the player API. It is the seam bots, an external agent and netplay all need anyway, and it
is the one with evidence behind it.

`GETV_SCRIPT` should be reimplemented on top of `gePlayerPost` rather than debugged where it is.
It currently synthesises a *gamepad* state and hands it to `port_os` for mapping, which is two
translation layers away from the pad the game reads; the API writes the pad directly. Until then
treat scripted runs as testing the menu and the boot path, not movement.
