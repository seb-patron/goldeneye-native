# Co-op: resolved, and two wrong baselines on the way

**Co-op movement works.** Measured with a control on Train: player 1 travels 1,779 units with
input and 0 units without it, while player 0 is unaffected at 330 units in both runs. Splitscreen
co-op in the single-player campaign is playable.

It took two false conclusions to get here, and both are worth keeping because both were confident.

## The first wrong baseline: a camera, not a player

`GETV_SCRIPT` never moved a player in any mode. Solo on Dam travels from (16872, 9502) to
(17795, 18011) over 570 frames **with the script and without it, identically** -- that travel is
the intro swirl camera animating, not input driving anyone.

So every "solo moves 900 units, co-op moves nothing" comparison in the original version of this
document compared a camera animation against a stationary player. The conclusion that co-op
movement was broken was never demonstrated by it.

## The second wrong baseline: the accessor was reading the wrong field

The position readout used to report `player->pos`, which is zeroed at spawn (`player.c:160`,
`bondview.c:1423`) and never updated. The engine maintains the position on the object it owns:
`prop->pos`. On Bunker 1 the collision position moved 530 units while the accessor returned the
same coordinate 900 times.

That is why co-op looked frozen. The players were moving; the instrument was not.

**The value lives on the object the engine maintains, not on the record that appears to own it.**
The same shape has now caught us three times -- `prop->pos` over `player->pos`,
`getsubroty(chr->model)` over `chr->aimsideback`. When a readout is suspiciously constant,
suspect the field before the feature.

## What was genuinely fixed along the way

What survives, because it was measured directly rather than through that comparison:

- Each player now has their own camera (was: player 1 held player 0's).
- Player 2 spawns somewhere standable (was: inside a wall).
- Co-op reaches CAMERAMODE_FP (was: stuck at NONE).
- With FP reached, co-op takes the **MoveBond** dispatch branch, which is the real movement
  path. Solo, still in SWIRL at the same frame, takes the FROZEN branch that discards input.

That last point is worth sitting with: after the camera fix, **co-op is on the correct branch
and solo is not.** The next real test is a person playing co-op, not another scripted run.

## The movement is computed correctly and then rejected

This is the tightest the search has been. With the camera fix in place, in co-op:

```
[getv][start] dispatch cammode=4 timeractive=1 stick=(0,68) branch=MoveBond lockctl=0
[getv][walk]  p=0 spd=0.972 theta=(-1.000,-0.006) dt=1.00 off=(-0.762,-0.013)
[getv][walk]  p=0 spd=0.972 theta=(-1.000,-0.006) dt=1.00 off=(-2.063,-0.084)
```

Every link works:

1. The injected stick reaches `bondviewMovePlayerUpdateViewport` as `(0,68)`.
2. The dispatch sends it to **MoveBond**, the real movement path, with controls unlocked and
   the camera in FP.
3. `bondviewProcessInput` produces `speedforwards = 0.972`.
4. `bondview2.c:7781` builds a real `move_offset` from the speed and the camera rotation, and
   it grows frame over frame: -0.762, -1.447, -2.063.
5. `bondviewCalcUpdatePlayerCollision(&move_offset, ...)` receives it.
6. **The position does not change.**

So the movement is calculated correctly and rejected inside the collision update. Everything
upstream of that function is exonerated by measurement rather than by argument.

**Not the other player.** At `GETV_COOP_SPREAD=6000` the players are 6000 units apart and
player 0 is equally stuck, so mutual collision is excluded.

The next step is inside `bondviewCalcUpdatePlayerCollision` (`bondview2.c:2861`): find which
of its guards refuses the offset when a second player exists. That is one function, and the
inputs to it are known good.

## What the harness would need

`bondview2.c:8615` dispatches on the camera mode:

```c
if (mode == NONE || (mode == FP && is_timer_active) || mode == FADE_TO_TITLE)
    MoveBond(stick_x, stick_y, buttons, ...);      /* real movement */
else
    bondviewFrozenMoveBond(...);                   /* input discarded */
```

Both branches receive `stick=(0,0)` under GETV_SCRIPT, so the injected stick is not reaching
this function at all. Fixing the harness means finding where the script's stick is lost between
the pad and here -- and that is a harness bug, not a game one.

## Original write-up, kept for the measurements

Two players in a single-player mission. They spawn, and they appear to be standing inside each
other and unable to move. Neither of those is quite what is happening.

## Fixed: every player now has their own camera

**Root cause: a campaign mission has one start pad.**

`bondview_r.c:546` picks a per-player start pad, gated on `getPlayerCount() >= 2 &&
startpadcount > 0`. Multiplayer arenas carry five to eight pads, so each MP player resolves to
a different one. A campaign mission carries **one**, so in co-op every player resolves to the
same pad -- and `change_player_pos_to_target()` seeds that single position into each player's
own camera record.

```
co-op on a campaign mission   startpadcount = 1
multiplayer on stage 27       startpadcount = 5
```

The fan-out now happens at `start_pos`, before the camera is seeded, and only when there is a
single pad to share. With a real spawn set the game's own pad choice is left alone. The floor
is resampled under the offset position, keeping the pad's tile if the query finds nothing,
since a wrong tile drops the player through the ground.

After:

```
p=0  pos=(-1381.4, 2279.9)  cam=(-1381.4, 2284.4)
p=1  pos=(-1181.4, 2278.4)  cam=(-1181.4, 2284.4)
```

Each camera matches its own player, which is what multiplayer already looked like.

## Correction: this is not a co-op bug

**Retail multiplayer does not move either, under the same harness.**

```
GETV_MP=2, stage 27, scripted forward input on either port, 570 frames
  p0  0.0,-2388.6      unchanged
  p1  -2069.3,2882.6   unchanged
```

The same input carries a solo player 900 units. So the whole session's framing of this as "the
co-op players are stuck" was wrong: **every** multi-player mode is stuck under scripted input,
including the shipping multiplayer that has presumably always worked when a human plays it.

That moves the suspicion to the harness rather than the game. `GETV_SCRIPT` drives one N64 port
directly; a multi-player match may need something a headless run never supplies before it hands
control to the players.

What is already excluded as the cause:

- **The clock.** `objective_status.c` carries a written-up hypothesis that
  `lvlManageMpGame()` zeroes `g_ClockTimer` when controls are locked or the game is paused,
  freezing every player. Measured: `locked=0 paused=0 clock=1`. Not it.
- **The camera mode.** Co-op runs `CAMERAMODE_MP` then `CAMERAMODE_NONE`, and real MP does
  exactly the same, 113 frames then the rest. Normal for multiplayer.
- **Input and speed.** Both players get `analogWalk=63` and `speedforwards=0.900`.

So: input arrives, the speed is computed, the clock is running, the camera mode matches
retail, and position does not change -- in co-op *and* in retail multiplayer.

## The freeze is the camera mode

`bondview_r.c:885` picks the camera path by player count:

```c
if (getPlayerCount() == 1)  bondviewSetCameraMode(CAMERAMODE_INTRO);
else                        bondviewSetCameraMode(CAMERAMODE_MP);
```

Co-op is a campaign mission with two players, so it takes the arena path. That path is a dead
end here: `bondviewAdvanceCameraMode()` sets `g_CameraMode` to `NONE` and then skips the whole
chain that reaches FP, because its `else if (mode != CAMERAMODE_MP)` excludes it. Retail
expects the multiplayer match flow to set FP itself, and nothing on this path does.

With the mode stuck at `NONE`, `bondviewFrozenCameraTick()` leaves `property_pos` at
`g_DefaultFrozenPlayerPos`, so every player is frozen while rendering carries on normally.
That is the freeze, and it is why retail multiplayer freezes here too.

Measured:

```
Solo     intro 482 frames, fadeswirl 60, swirl 358      moves 900 units
co-op    MP 113 frames, NONE 787                        moves nothing
real MP  MP 113 frames, NONE 787                        moves nothing
```

### Fixed: co-op reaches first person

`bondviewAdvanceCameraMode()` now hands `CAMERAMODE_MP` straight to FP when co-op is on,
running the same three lines the SWIRL arm does -- drop the player's own body, fade the
characters in, set FP -- and skipping the swirl itself, which is a single-player animation
that drags every player along its path when two exist.

```
before   MP 113 frames, NONE 787      camera frozen at the default position
after    MP 113 frames, FP 787        first person, positions sane
```

Real multiplayer is untouched, since the branch is gated on `gePortCoopPlayers() >= 2`.

**This did not make them walk.** Positions are correct and stable rather than frozen at a
default or falling through the level, and the pipeline demonstrably works -- `property_pos`
moves and `g_CurrentPlayer->pos` follows it one frame later -- but scripted input still does
not drive it.

One measurement worth carrying forward: `speedforwards` reads 0.000 in **solo** as well, at a
frame where solo is visibly moving 6 units per frame. So `speedforwards` is not what carries a
walking player, and the earlier finding that it reaches 0.900 in co-op says less than it
appeared to. Whatever moves a solo player is something else, and finding it is the next step.

### What routing co-op through the campaign path does

`GETV_COOP_CAMERA=1`, **off by default**, sends co-op down `CAMERAMODE_INTRO` instead. The
mode then reaches SWIRL and the positions finally change. They also change identically
whichever port is driven, with Y running from -52723 to -139906 over 570 frames.

That is the intro swirl camera animating and dragging the players, not input moving them.
Falling through the level is worse to ship than standing still, so it stays off. The campaign
intro path assumes a single player somewhere inside it.

**What it proves is the useful part: the freeze IS the camera mode.** Whatever fixes co-op has
to get the mode to FP without running the single-player swirl.

## Still open: nobody moves

Separating the cameras did **not** make them walk. With the fix in place and input driven to
either port, both players stay exactly where they spawned across 570 frames while the same
input carries a solo player 900 units. So these were two bugs, not one, and this document's
original finding below covers the second.

**Player 1's camera held player 0's position.**

`GETV_MOVETRACE=1` prints the player pointer, the position and the camera position together.
With the spawn spread set to 6000 units:

```
p=0  cur=0x1051e9800  pos=(-1381.4, 2278.4)  cam=(-1381.4, 2284.4)
p=1  cur=0x1051ec3f8  pos=( 4618.6, 2279.9)  cam=(-1381.4, 2284.4)
```

Player 1's `pos` is correctly 6000 units away. Player 1's camera is at player **0's** position,
to the unit. `g_CurrentPlayer` is switching correctly -- the pointers differ and match
`g_playerPointers[0]` and `[1]` -- so this is not a case of one player struct being reused.

The camera is built from `field_488.pos` (`bondview2.c:8926`, `cam_pos.x = collision->pos.x`).

## What that explains

- **"They are inside each other."** They are not. At 6000 units apart both viewports render the
  same corridor from the same spot, and each shows the other player's body in front of the
  camera, because both cameras are at player 0 while both models are drawn at their own
  positions. It looks like overlap and it is two cameras in one place.
- **"They do not move."** You are watching player 1 through player 0's camera, so player 1's
  movement would not show even if it happened.

## What is already ruled out, with the measurement

| | |
|---|---|
| Input not reaching player 1 | Refuted. `GETV_SCRIPT_PORT=1` gives player 1 `analogWalk=63`, the same value player 0 gets. |
| The `canLookAhead` gate | Refuted. It is 1 for both players. |
| `speedforwards` never being set | Refuted. It is assigned 63/70 = 0.900 for player 1. |
| Spawn positions overlapping | Refuted. At 600 and at 6000 units apart, nothing changes. |
| A player-2-specific bug | Refuted. Player 0 is equally stuck once co-op is on, while the same input carries the solo player 900 units. |

## The comparison that names the fix

Real multiplayer does this correctly. Same build, same probe, `GETV_MP=2` instead of
`GETV_COOP=2`:

```
stage 27 MP   p0  pos=(    0.0, -2388.6)  cam=(    0.0, -2382.7)   matches
              p1  pos=(-2069.3,  2882.6)  cam=(-2069.3,  2887.0)   matches
stage 31 MP   p0  pos=(-1651.0,  -348.9)  cam=(-1645.0,  -348.9)   matches
              p1  pos=( -909.1,  2263.3)  cam=( -904.7,  2263.3)   matches
```

Against co-op on the same levels, where player 1's camera is player 0's every time. Stage 27
is both a campaign mission and an arena, so this is the same map, the same build and the same
two players -- only the spawn path differs.

**So the multiplayer spawn path initialises each player's camera record and the co-op path
does not.** The fix is to find what MP does at spawn that co-op skips, rather than to keep
offsetting fields by hand. That is a reference implementation sitting in the same tree.

## When it goes wrong: before the first frame

`GETV_MOVETRACE=1`, first occurrence per player on stage 27 co-op:

```
p=1  pos=(0.0, 0.0)  cam=(-3404.3, 4539.1)
p=0  pos=(0.0, 0.0)  cam=(-3404.3, 4539.1)
```

Both cameras already hold the same value while both positions are still zero. That value is
player 0's spawn point. So player 1's camera is **never** given player 1's position -- it is
not set correctly and then clobbered, it starts wrong.

`init_player_BONDdata` is not where this happens. It runs for both players in both modes and
leaves pos and cam at zero in both, so it is identical between co-op and MP and can be ruled
out.

## Where to look next

The writer that seeds `field_488.pos` from the spawn pad, between `init_player_BONDdata` and
the first rendered frame. In multiplayer it runs per player and gives each its own pad. In
co-op it produces player 0's value for both.

`field_488` is assigned wholesale in only two places, `bondview2.c:1220` and `:9953`, both
copying to and from `previous_collision_info` within one player, and neither is on the spawn
path -- 1220 is the death replay. So the seed is a field-by-field write somewhere on the spawn
path, or an alias through `RenderPosView *`, of which `bondview2.c:11046` takes one from
`bodyModel->render_pos`.

The measurement to confirm any candidate: make the camera column match the position column for
both players in the reproduce command below. Multiplayer already does, so a fix is correct
when co-op looks like MP.

## What was tried and did not work

Offsetting `field_488.pos`, `field_488.pos3` and `previous_collision_info` inside the co-op
spread block in `bondviewSetCurrentPlayerPosition`. The camera still reads player 0's position
afterwards, so **something rewrites that record between the spread and the first rendered
frame**. Finding that writer is the next step.

`field_488` is only assigned wholesale in two places (`bondview2.c:1220` and `:9953`), both
copying to and from `previous_collision_info` within the same player, so the overwrite is
either one of those running with the wrong player current, or the record being seeded from a
source that is not the player's own position.

## Reproducing

```bash
GETV_COOP=2 GETV_PADS=2 GETV_COOP_SPREAD=6000 GETV_MOVETRACE=1 \
GETV_STAGE=9 GETV_EXIT_FRAME=400 ./getv/build-mac/goldeneye 2>&1 | grep 'getv..who'
```

The camera column is the whole story: when it matches `pos` for both players, this is fixed.
