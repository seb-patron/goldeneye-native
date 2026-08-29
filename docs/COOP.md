# Co-op

Two to four players share a single-player mission's own geometry, objectives and cutscenes,
split screen. It is bring-up quality: the mission is authored around one Bond, so extra players
are present rather than accounted for. `GETV_MP` is the separate, unrelated path that boots a
real multiplayer arena setup instead.

**Use `GETV_MOVE_SELFTEST` for any input measurement here.** It holds the left stick forward on
every port from a given frame, which is what makes a co-op measurement mean anything: the other
paths each reach one player. `GETV_SCRIPT` drives port 0 and has never been shown to move anyone
in any mode -- solo on Dam travels identically with it and without it, and that travel is the
intro camera animating rather than input driving the player. The player API
(`gePlayerClaim`/`gePlayerPost`, see [`HARNESS.md`](HARNESS.md)) addresses a single slot.

Two knobs, both read once:

```
GETV_MOVE_SELFTEST=<frame>     hold forward on every port from this frame
GETV_MOVE_SELFTEST_Y=<counts>  the axis value; default -32000, positive walks backwards
```

Hold from a frame rather than from zero, for the same reason `GETV_AIM_SELFTEST` does: controls
are locked through the boot and the level intro, so anything held from frame 0 is already down
before the player has control. On BUNKER1 the dispatch reaches `cammode=4 branch=MoveBond` at
about frame 600, so 700 is a safe start and 1501 a run long enough to see travel.

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

## Resolved: both players walk

**Superseded. The section this replaces said "Nobody walks yet" and pointed at a guard inside
`bondviewCalcUpdatePlayerCollision` that refuses the movement offset "specifically when a second
player exists". Measured on macOS, that is wrong on both halves: the collision step applies the
offset, and the same symptom reproduces with one player.**

What was actually missing was a way to give both players a stick. Every injection path in the
tree reaches exactly one: `GETV_SCRIPT` drives port 0, and the player API addresses a single
slot. That matters more than it sounds, because the default control style is 2.2 Galore, a
TWO-CONTROLLER style, and `bondview2.c:5420` takes the walk axis from controller 2
(`moveData.analogWalk = tmpc2sticky`). Injecting on the wrong pad leaves `analogWalk` at 0 with
`canLookAhead` already 1, and a player who is never asked to walk is indistinguishable from a
player who cannot.

`GETV_MOVE_SELFTEST=<frame>` fills that gap. It holds the left stick forward on EVERY port from
that frame, applied around the whole of `gePortInputPollPort` rather than inside one of its
appliers -- necessary because with `GETV_PADS=2` the two players do not come down the same path:
port 0 is claimed by the keyboard applier and returns early, port 1 reaches the synth block.
`GETV_MOVE_SELFTEST_Y=<counts>` sets the axis, so a positive value walks backwards.

### The measurement

Co-op, two players, both held forward from frame 700, BUNKER1, `GETV_EXIT_FRAME=1501`, **n=3 and
byte-identical across all three**:

```
control, no input     no [coll] lines at all -- neither player moves
both held forward     p0 colpos (-1381.4,2284.4) -> (-1413.8,2486.7)   77 distinct
                      p1 colpos (-1369.0,2387.6) -> (-1361.3,2506.6)   68 distinct
```

Solo, the same knob, confirmed against the renderer rather than a position field:

```
standing still        tris submitted=908  drawn=326
walking forward       tris submitted=874  drawn=278
walking backward      tris submitted=1088 drawn=404
```

Three different scenes, so the world genuinely moves around the player.

### One trap worth keeping

**Do not judge movement by the `pos=` column in `[getv][who]`.** It prints
`g_CurrentPlayer->pos`, which is byte-identical between a forward run and a backward run whose
collision positions differ by 377 units. The field that tracks the player through the world is
`field_488.collision_position`, which `[getv][coll]` prints. Reading the wrong column is what
makes working movement look frozen, and it is most of why the previous version of this section
concluded what it did.

`[getv][walk]` is also conditional -- it only prints when `speedforwards` or the offset is
non-zero -- so its absence means the speed was never set, not that the code did not run. And
`[getv][move] ... GATE canLookAhead=` sits inside `if (g_PlayerIsInTank == 1)`; it is a tank
trace and will never fire on foot.

## Fixed: players cannot shoot each other

Found the way COOP.md said it would have to be: two people sat down and played it. Player 2
spawns behind player 1, both facing the same way, and the first trigger pull killed a team mate
instantly. No scripted run had produced that, and none was going to -- it needs two humans and
one of them pointing a gun at the other's back.

`record_damage_kills` (`bondview2.c`) takes `playerid`, the player who CAUSED the damage, or -1
for anything that is not a player: guards, unowned explosions, gas, falling. `g_CurrentPlayer` is
the victim, because the function runs in the victim's context. So

```
playerid >= 0 && playerid != get_cur_playernum()
```

is exactly one player hurting another, and under `gePortCoopPlayers() >= 2` that is co-op. The
damage returns early instead.

**Deathmatch is untouched.** `gePortCoopPlayers()` is only >= 2 under `GETV_COOP`; real
multiplayer boots through `GETV_MP`, where shooting each other is the entire point.

`GETV_COOP_FRIENDLYFIRE=1` puts it back. The first blocked shot prints

```
[getv][coop] friendly fire off (GETV_COOP_FRIENDLYFIRE=1 allows it)
```

once per session, because a shot that does nothing is otherwise indistinguishable from a shot
that missed.

The spawn spread itself was checked and left alone: `GETV_COOP_SPREAD` defaults to 120 units and
already tries eight directions across three rings, falling back closer only when the stan query
finds no tile. Players start close in a corridor because the corridor is narrow, not because the
placement is careless.

## Fixed: one player dying does not fail the mission

`g_isBondKIA` is a global, and `bondviewKillCurrentPlayer` set it from a single player's death.
Two places in `front.c` treat it as final: `frontCompleteAllObjectivesAliveSuccess()` returns 0
outright, and the end screen prints KILLED IN ACTION. In co-op that meant the moment anyone died,
the mission became unwinnable for everybody -- the survivor could go on to complete every
objective and still be told they failed.

It is now set only when nobody is left standing. Solo is unchanged by construction: with one
player there is never another survivor, so the new condition is the old behaviour written out.
Verified as a number rather than by reading: solo walking forward renders `tris submitted=874
drawn=278` both before and after, byte-identical.

The AI opcode that also sets the flag (`AI_BondKilledInAction`, `chrai.c`) is deliberately left
alone. That is a mission script declaring the run lost for a scripted reason -- an escort dying,
say -- and it should end the mission for the team.

See the respawn section below: the dead player now comes back rather than sitting out the rest
of the mission.

## Fixed: the dead player comes back

The respawn machinery already existed and co-op was already reaching it. `mp_respawn_handler()`
does the whole job -- `init_player_BONDdata()`, `bondviewPlayerBeginLife()`, clearing `bonddead`
and the death-animation flags, and placing the player on a start pad -- and the branch that calls
it is chosen on `getPlayerCount() >= 2`, which co-op satisfies.

What stopped it was the gate around the call, which is a deathmatch rule:

```c
if ((scenario != SCENARIO_YOLT) || (total < 2))
    if (joyGetButtons(get_cur_playernum(), 0xB000))
        mp_respawn_handler();
```

`SCENARIO_YOLT` is You Only Live Twice, and `total` counts how many times the other players have
killed this one. A campaign has no scenario and nobody is scoring kills, so both terms are
meaningless here and the `kill_counts` read is of data no co-op mission ever fills in.

Waiting for a button is a deathmatch habit too. A multiplayer player knows they respawn and is
holding a pad; someone who has just died halfway through Bunker does not necessarily know there
is anything to press, and a black screen that never ends reads as a hang. Co-op therefore comes
back on its own after a delay, with the button as a way to skip the wait rather than the only way
out.

| Setting | Value | What it does |
|---|---|---|
| `GETV_COOP_RESPAWN` | seconds, default `5` | How long a dead player waits. `0` turns the timer off and leaves only the button, which is the multiplayer behaviour. |

`mp_respawn_handler()` needs `startpadcount > 0`, and a campaign level always has at least the pad
the mission starts you on, so a dead player returns to where the level began rather than to their
team mate. Respawning next to the survivor would be better and is not what this does.

**A team wipe is still a lost mission.** If the last player standing goes down, the guard from the
section above sets `g_isBondKIA`, and the respawn checks that flag -- so anyone already counting
down does not come back into a run that is over. Without that check the mission would be
unloseable rather than merely survivable.

### Seen happen

`GETV_KILL_SELFTEST=<frame>` kills a player on a headless run, `GETV_KILL_SELFTEST_P=<n>` picking
which and a negative value killing everyone. It fires only on a player whose `bonddead` is FALSE,
so the number of kills in a run is the number of times that player was alive to be killed --
which is what makes the respawn measurable without watching it.

BUNKER1, co-op, player 1 killed at frame 900, run to 1801:

```
GETV_COOP_RESPAWN=0   1 kill    the timer is off, nobody presses a button, they stay down
GETV_COOP_RESPAWN=3   3 kills   they come back twice and are killed again each time
GETV_KILL_SELFTEST_P=-1, RESPAWN=3
                      2 kills   both players go down one frame apart and neither returns
```

The last row is the team-wipe rule: with nobody left standing `g_isBondKIA` is set, the respawn
checks it, and the run stays lost. The middle row against the first is the respawn itself -- same
kill, same frame, the only difference is the timer.

Friendly fire is the one rule still not measured directly, because making one player shoot
another needs more than a kill hook. Its guard is three lines in `record_damage_kills` and the
symptom it fixes was found by playing.

## Reproducing

```bash
GETV_COOP=2 GETV_PADS=2 GETV_COOP_SPREAD=6000 GETV_MOVETRACE=1 \
GETV_STAGE=9 GETV_EXIT_FRAME=400 ./getv/build-mac/goldeneye 2>&1 | grep 'getv..who'
```

`GETV_MOVETRACE=1` prints the player pointer, position and camera position together, per
player. The camera column matching the position column for both players is the whole
story -- when it does, the spawn/camera half of this document is confirmed fixed.
