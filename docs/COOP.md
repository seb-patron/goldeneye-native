# Co-op: what is actually wrong

Two players in a single-player mission. They spawn, and they appear to be standing inside each
other and unable to move. Neither of those is quite what is happening.

## The finding

**Player 1's camera holds player 0's position.**

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
