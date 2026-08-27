# Co-op

Two to four players share a single-player mission's own geometry, objectives and cutscenes,
split screen. It is bring-up quality: the mission is authored around one Bond, so extra players
are present rather than accounted for. `GETV_MP` is the separate, unrelated path that boots a
real multiplayer arena setup instead.

**Harness caveat, worth knowing before trusting any scripted-input measurement below:**
`GETV_SCRIPT` has never been shown to move a player, in any mode. Solo on Dam travels from
(16872, 9502) to (17795, 18011) over 570 frames identically with the script and without it --
that travel is the intro swirl camera animating, not input driving the player. So a comparison
like "solo moves 900 units, co-op moves nothing" is comparing a camera animation to a genuinely
stationary player, not proving anything about co-op input handling on its own. See
[`HARNESS.md`](HARNESS.md) for the working alternative (`gePlayerClaim`/`gePlayerPost`, the
player API).

## Fixed: per-player spawn and camera

**Root cause: a campaign mission has one start pad.** `bondview_r.c:546` picks a per-player
start pad, gated on `getPlayerCount() >= 2 && startpadcount > 0`. Multiplayer arenas carry five
to eight pads, so each MP player resolves to a different one. A campaign mission carries
**one** (`startpadcount = 1`, against 5 for MP on stage 27), so every co-op player used to
resolve to the identical pad -- and because the camera record was seeded from that same shared
position, player 1's camera showed player 0's position even once player 1's own `pos` had been
correctly offset. Confirmed with `GETV_MOVETRACE=1`: both players' cameras held the identical
value while both positions were still zero, before the first rendered frame -- so the camera
was never given the right value, not overwritten afterward. Real multiplayer, which has
distinct pads per player, already seeds each camera correctly (stage 27: p0 pos/cam both
`(0.0, -2388.6)`-ish, matching to within the render offset; stage 31 the same for both
players) -- the multiplayer spawn path initialises each player's camera record and co-op did
not.

The fix fans out spawn positions at `start_pos`, **before** the camera is seeded, and only when
there is a single pad to share -- a real per-player spawn set is left alone. The floor is
resampled under the offset position, keeping the pad's own tile if the query finds nothing,
since a wrong tile drops the player through the ground.

```
p=0  pos=(-1381.4, 2279.9)  cam=(-1381.4, 2284.4)
p=1  pos=(-1181.4, 2278.4)  cam=(-1181.4, 2284.4)
```

Each camera now matches its own player.

## Fixed: co-op reaches first person

Separately, co-op never advanced its camera to first person at all. `bondview_r.c:885` picks
the camera path by player count -- one player takes `CAMERAMODE_INTRO`, two or more take
`CAMERAMODE_MP` -- and `CAMERAMODE_MP` is a dead end outside real multiplayer:
`bondviewAdvanceCameraMode()` sets the mode to `NONE` and then skips the chain that reaches FP,
because retail expects the multiplayer match flow to advance it and nothing on the campaign
path does. Stuck at `NONE`, `bondviewFrozenCameraTick()` leaves every player pinned at
`g_DefaultFrozenPlayerPos` while rendering carries on normally -- which is also why real
multiplayer freezes under the same scripted-input harness (stage 27, `GETV_MP=2`: both ports'
positions unchanged over 570 frames, against 900 units for solo with identical input).

Ruled out first, with the measurement: the mission clock (`locked=0 paused=0 clock=1`, so
`lvlManageMpGame()` is not zeroing it); the camera mode itself (co-op runs `CAMERAMODE_MP` then
`NONE` for 113 then 787 frames, identically to real MP); and input reaching the player
(`analogWalk=63`, `speedforwards=0.900` for both).

Fix: `bondviewAdvanceCameraMode()` now hands `CAMERAMODE_MP` straight to FP when co-op is on
(gated on `gePortCoopPlayers() >= 2`), running the same three setup lines the solo `SWIRL` arm
runs -- drop the player's own body, fade the characters in, set FP -- without running the swirl
animation itself, which is a single-player camera path that would otherwise drag every co-op
player along with it. Real multiplayer is untouched by the gate.

```
before   MP 113 frames, NONE 787      camera frozen at the default position
after    MP 113 frames, FP 787        first person, positions sane and stable
```

An alternative was tried and rejected: `GETV_COOP_CAMERA=1` (off by default) routes co-op
through the solo `CAMERAMODE_INTRO` path instead. It does reach a moving camera, but that
motion is the single-player intro swirl dragging every player through its own scripted path
(all ports move identically, Y running -52723 to -139906 over 570 frames) rather than anything
player-driven, and the campaign intro assumes exactly one player, so a second can fall through
the level. Kept as a diagnostic knob, not a fix.

## Still open: movement

With both camera bugs fixed, positions are correct and stable rather than frozen or falling
through the ground -- `property_pos` moves and `g_CurrentPlayer->pos` follows it one frame
later, so the pipeline demonstrably works. **Nobody walks yet.**

The furthest a trace has gotten: with the camera fix in place, `dispatch cammode=4
timeractive=1 stick=(0,68) branch=MoveBond lockctl=0` shows the injected stick reaching the
real movement dispatch (not the frozen one), `bondviewProcessInput` produces
`speedforwards=0.972`, and `bondview2.c:7781` builds a `move_offset` from speed and camera
rotation that grows every frame (`-0.762 -> -1.447 -> -2.063`). That offset reaches
`bondviewCalcUpdatePlayerCollision`, and the position does not change. Not the other player:
at `GETV_COOP_SPREAD=6000` the players are 6000 units apart and player 0 is equally stuck, so
mutual collision is excluded. The next step is inside `bondviewCalcUpdatePlayerCollision`
(`bondview2.c:2861`) -- find which of its guards refuses the offset specifically when a second
player exists.

Treat that trace as provisional rather than conclusive, given the harness caveat up top: at the
same dispatch site (`bondview2.c:8615`), a separate measurement under plain `GETV_SCRIPT` found
both branches receiving `stick=(0,0)`:

```c
if (mode == NONE || (mode == FP && is_timer_active) || mode == FADE_TO_TITLE)
    MoveBond(stick_x, stick_y, buttons, ...);      /* real movement */
else
    bondviewFrozenMoveBond(...);                   /* input discarded */
```

If the injected stick is not reliably reaching this function, the collision-rejection lead
needs re-confirming with a working input path -- the player API in
[`HARNESS.md`](HARNESS.md) -- before it can be trusted as the actual blocker. The next real
test is a person playing co-op, not another scripted run.

## Reproducing

```bash
GETV_COOP=2 GETV_PADS=2 GETV_COOP_SPREAD=6000 GETV_MOVETRACE=1 \
GETV_STAGE=9 GETV_EXIT_FRAME=400 ./getv/build-mac/goldeneye 2>&1 | grep 'getv..who'
```

`GETV_MOVETRACE=1` prints the player pointer, position and camera position together, per
player. The camera column matching the position column for both players is the whole
story -- when it does, the spawn/camera half of this document is confirmed fixed.
