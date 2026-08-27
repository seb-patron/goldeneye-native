# Roadmap

One goal, two machines, divided by **what each can actually do** rather than by topic.

> **The objective:** a bot completes a mission objective on Train, and the same code does it on a
> second level. Everything below is ordered by what blocks that.

The measuring instrument is the CLI (`GETV_CLI=1`). If a person can play a level from the report,
the API is complete and the rest is policy. Today a person gets the objective from 14,649 to
13,315, holding 62% health and returning fire, and then wedges. We know where and why.

## Staying 1:1: check it, do not assume it

`tools/sync_surface.sh` verifies both halves and prints OK or MISMATCH:

```
== 1:1 CHECK ==
  commits   mac=cb0d534e0f  surface=cb0d534e0f  OK
  decomp    mac=5f4c9f41fee5  surface=5f4c9f41fee5  OK
```

Being in sync has **two independent parts and only one travels in a bundle**:

1. **Commits.** Carried by the bundle. Compared on FULL hashes, because git picks short-hash
   length per repo, so the two sides can abbreviate the same commit differently and a naive
   compare reports
   a tree mismatching itself. A check that cries wolf gets ignored.
2. **`vendor/**`.** **Gitignored, so decomp symbols do NOT travel.** The Surface lost a day to a
   build that compiled and would not link for exactly this. Compared by SHA-1 of
   `objective_status.c`, because a name check proves one symbol arrived and a hash proves the
   whole file did.

⚠️ **`git apply` reports "Skipped patch" on the Surface's vendor tree**: its ignore rules exclude
that path, so a patch that verifies clean can still land nothing while every signal says it
worked. **Deliver the file by scp and verify the hash.** Patches are for the record, not the
transport.

## The division, and why it is this one

**The Surface renders at roughly one frame per second.** It cannot measure anything at runtime,
which is why `dump_spawns.py` and the edge validator came back to the Mac. That is not a
judgement, it is a constraint, and the split follows from it:

| | |
|---|---|
| **Mac**, anything needing a running game | bots, co-op, the CLI, measurement, `vendor/**`, input, docs |
| **Surface**, anything offline | extraction, audits, tests, Windows, the launcher, netplay |

Work flows Surface → Mac → `main`; the Mac integrates file by file. Sync with
`tools/sync_surface.sh`. Read the diffstat before taking anything: four reverts were caught that
way today and none reached `main`.

🔑 **Whoever owns the CONSUMER keeps the code** when both sides write the same thing. We each
deleted our own walkability reader in the same hour in favour of the other's; for a few minutes
neither existed.

---

# SURFACE: offline work, in order

## S1. Prop extents: ✅ DONE, end to end

**Shipped in `b5d694e`.** `extrascale` was in the setup data all along — `propobj.c:78`,
`scale = extrascale * (1/256)`, per *placement* rather than per model, and `parse_props` was
already matching that word in its regex and discarding it. 4,207 of 4,871 props across 20 levels
carry `hx`/`hz`/`radius`; the 664 without are Guards, every one, because they have no model box.
Pack bumped to v3, `GeWorldProp` and the CLI carry it. Verified by decoding `train.gew` at the C
loader's own byte offsets — 5 of 5 records match.

⚠️ **`0` means unknown, not point-sized**, at every layer. The CLI prints `radius ?` rather than
`radius 0`, because a caller reading a Guard's 0 as zero-sized walks straight through people.

⚠️ **One outlier, not smoothed:** `hatchbolt` as a Collectable gives radius 2236 runtime in a
carriage ~739 wide. The pipeline is right — the same model as a StandardProp comes out 28 — so that
model's own box is simply large. 37 instances on Train. Do not trust that one.

The original statement of the item follows, for the reasoning behind it.

**Every position in the pack is a POINT and the world is made of solids.** A crate reported "278
away" is 278 units to its *centre*. A bot that still sees room has already walked into the corner
of it. That is how the CLI player wedges on Train while the report insists there is space, and no
policy fixes it because the information is not in the data.

**Why it is yours:** `gePortPropExtent` is written, compiles, and is on your box, and it reads
`chrobjGetBboxFromObjectRecord` plus `chrpropBBOXGetXmin`/`Zmin`. **It cannot be driven.** The
engine exposes no enumerable prop list; props live behind the object handler and the only global
prop arrays are the tank's. So the answer has to come from the model data offline, which is your
extractor's lane.

**Do:**
1. Emit `hx`, `hz`, `radius` per prop into the level knowledge, from the model bounding box.
2. `pack_world.py`: three more floats per prop record. Bump the pack version; the loader checks it.
3. `GeWorldProp` gains the three fields; `geWorldNearestProp` and friends carry them through.
4. The CLI's `near` lines become `crate 278 away, radius 120` so a reader knows the surface is at
   158, not 278.

⚠️ **Scale them.** Extents are asset-space lengths and the pack is runtime space now. An unscaled
radius is wrong by `1/levelscale`: 6.7× on Train, 4.3× on Dam.

⚠️ **The model box is unrotated.** A long crate at forty-five degrees occupies more width than its
half-extent suggests. Emit the radius as well and label which is which; do not silently pick one.

⚠️ **Round-trip tolerance is RELATIVE now.** f32 keeps seven significant digits, not seven decimal
places, and runtime coordinates reach 86,000. A flat `1e-3` fails seven levels.

**Done when:** the CLI's `near` lines carry a radius, and the Train player walks past the crate it
currently traps itself on.

## S2. Navmesh nodes: ✅ DONE, and the acceptance test passed

Pads are prop markers, not places to walk. Measured with the teleport probe: **139 of Train's 180
nodes cannot be stood on**, and across the twenty solo levels it runs 33 to 240 each. A follower
given targets a body cannot occupy is short by however far the pad sits off the floor, always.
That is not a tuning problem.

You already emit tile adjacency, floor tiles and 1,100 stairways. **The tiles are the graph.** A
node per floor tile, or per cluster, is standable by construction, has a real height, and needs no
snapping to place. Pads go back to being what they are: markers for props and spawns.

⚠️ **Declare which coordinate space the extractor emits.** The pack scales at the boundary today
and that is fine, but it must be stated rather than assumed. `runtime = asset / levelscale`.

**✅ Passed.** `tools/route_doors.py` routes the TILE graph from the measured spawn:

```
level      reachable   ratio    monotonic
train           100%   53.4:1        100%     <- 117 of 117 steps advancing along X
facility         85%    5.6:1         90%
bunker1          84%    0.8:1         69%
```

30 distinct rooms, zero revisited, 682 tiles reachable. The graph reproduces the linear chain of
carriages **and does so distinctively**: Train is an outlier against every other level, which is
what makes it a test rather than a coincidence.

🔑 They flagged their own "linear chain: yes" check as WEAK because every level passes it,
including open ones. A check that cannot fail is not a check, and saying so is worth more than
the check was.

⚠️ Two spaces reconciled explicitly on the way: the JSONs are asset space and the measured spawn
is runtime, so Train's spawn reads x=779 where the tile map ends at x=213. `asset = runtime *
levelscale`.

**Now the bot must ROUTE ON THIS GRAPH** rather than the pad graph. That is M1's real content and
it is unblocked.

## S3. Enemy facing: ✅ DONE

`gePortEnemyFacing` refuses and `geSenseNoticedBy` falls back to line of sight, so a bot hides
from a guard facing the other way and strolls past one staring at it. Train reports 17–19 watchers
of 40 guards, which is what an unobstructed line down a row of carriages looks like, **not**
seventeen guards watching.

`ChrRecord` has no facing field I could find. `chr.c:2319` reads `chr->aimsideback` into a `yrot`
when building the model matrix; that is where the answer probably starts.

⚠️ **Do not tune the line test to shrink the watcher count.** Add the cone. And keep both
questions: "could see me if it turned" is different from "is looking at me", and both are useful.

**✅ Done.** The heading is `getsubroty(chr->model)` (model.c:698), **not** `chr->aimsideback`,
which was my hint and was wrong. aimsideback is an AIM OFFSET applied to a body part while the
model is drawn, so a cone built on it swings with where a guard POINTS ITS GUN rather than which
way it faces, and would have read as roughly right most of the time.

🔑 Same shape as `prop->pos` versus `player->pos`, which has now caught us three times: **the
value lives on the object the engine maintains, not the record that looks like it owns it.**

They proved the convention from `chraction.c:9629` rather than assuming it: the game subtracts
`getsubroty` directly from `atan2f(dx, dz)`, so it is radians in exactly our convention.

## S4. Audit the line-versus-body substitution: ✅ DONE

**`a44c500`.** Audited every caller in the Surface lane. The split was clean: sites that *list*
every bit set (`mod.lua` 107-110, `ge_cli.c` 187-189, `ge_bot_route.c` 550-552) are honest
reporting and stay. Only sites that **act** on one bit were wrong — the atlas printed "that is a
DOOR, an opportunity to a bot" from the single bit, so a mod trusting the atlas would have believed
it. It now claims a door only when the bit arrives *alone* and reports door-plus-wall as a grazed
edge in those words.

`geSenseAheadForBody` also stopped sampling three parallel lines — the withdrawn 1c approach — and
now steps `gePortCanStandAt` along the heading. ⚠️ I wrote its forward vector as `-sin` and the unit
test caught it: the sweep walked *away* from the obstacle and reported clear every time. Fourth
inverted direction sign on this project, so the vector is now copied from `geSenseAhead` rather
than re-derived.

The original statement of the item follows.

The wedge was a **lying sensor, not a bad policy**: `geSenseClearestHeading` is a line test and a
line has no width, so a gap narrower than the player reads as the clearest heading available and
the router commits to the one direction it cannot fit through. Every trace says it chose
correctly.

Fixed in the router and the CLI. **Check your own callers for the same substitution.** The line
version is still right for questions genuinely about a line, whether a shot or a sightline
reaches.

## S5. Netplay determinism: ✅ DONE — 3,000 frames, identical

**`22a825e`.** `GETV_FPTRACE=1` emits the per-frame simulation fingerprint;
`tools/audit_lockstep.ps1` runs the binary twice and compares. **3,000 frames identical, 3,000 of
3,000 distinct fingerprints.**

The long-run test needs no second machine: two peers fed identical inputs are, for the determinism
question, one binary run twice. Delivery is `netsim.py`'s half; reproducibility *given* the inputs
was untested until now.

⚠️ **Necessary, not sufficient** — it cannot see a divergence only a different CPU or compiler
would produce. A failure would have been decisive.

⚠️ **The pass is guarded against being vacuous.** A fingerprint that never moved would make two
runs match trivially. My first attempt did exactly that: instrumenting `ge_playback` (which only
runs when input is *posted*) gave **two samples** across 3,000 frames, and both runs "agreed". The
distinct-value count is now reported for that reason.

## S6. Node table: ✅ DONE

**`2df5236`.** `build/levels/<level>.nodes.json` — 27 levels, 34,967 nodes: engine waypoints, tiles
and doors, each with position, kind, room and links.

🔑 **A bare node id is ambiguous.** Train has 104 engine waypoints numbered from 0 *and* 682 tiles
numbered from 0; a route's `path: [74, 75]` means waypoint 74. Every node therefore carries a
namespace and a `uid` = base + id, bases a million apart.

🔑 **`pathwaypoints` is `-1` terminated and the terminator was being emitted as a waypoint on every
level** — Train 105 against the engine's 104, Dam 206 against 205. Caught because two independent
counts differed by exactly one on *every* level. The field is `u32`, so `-1` arrives as
4294967295; testing only for `-1` would have left it everywhere.

✅ **The engine graph and our tile mesh agree.** Median distance from each of the 104 engine
waypoints to our nearest tile centre: **5.7 units**, 90th percentile 19.8, one beyond 60. Zero
rooms hold engine waypoints our mesh lacks tiles for.

## S7. Wall-set validation: ✅ RESOLVED — the premise was wrong, not the walls

🔑 **A waypoint link promises REACHABILITY, not a clear straight line.** Traced through the
decompilation rather than waiting for an answer:

- `padhalllv.c` — `waypointFindNextStepToward` returns a **neighbour** of the current waypoint
  (`waypointFindRandomByDist(pointa->neighbours, ...)`), not a bearing to the goal.
- `chraction.c:10495` — the caller stores it as `self->padpreset1`, a **target pad**.
- `chr.c:1468` — the guard then moves toward that pad through a collision-tested step:
  `stanTestLineUnobstructed` **and** `stanTestVolume`, before every move it commits to.

**If an edge guaranteed a clear straight line, that per-step collision test would be pointless.**
The engine tests because it does not assume, and the guard plots a course around what it finds.

So the **76.5%** measured below is real but does not mean what it appears to: straight segments
between graph nodes clipping interior geometry is *expected*, and `gen_level_walls.py` is not
indicted by it. `tools/audit_wall_routes.py` remains the right instrument for a different question
— which links have obstructions between their endpoints, useful for a follower that *does* move in
straight lines — but the figure is not a wall-set defect rate.

⚠️ **The one genuine defect stands:** the wall set contains **duplicate segments**, a shared tile
edge emitted once from each side. Dam's `(3772,4481)-(3922,4481)` appears twice in a single hit
list, so the reported segment counts are inflated.

The measurement and its three refuted hypotheses follow.

**`4e00ba7`.** `tools/audit_wall_routes.py` scores `gen_level_walls.py`'s segments against every
link in `g_CurrentSetup.pathwaypoints`. **3,198 of 4,180 engine links crossed by a wall — 76.5%.**

🔴 **That number is too large to believe and I have not established whose fault it is.** Three
hypotheses killed with measurements: 2D-versus-3D (fixed the height filter; 76.51% → 76.0%, not the
cause), room boundaries (same-room links block at 96.4% against 98.6% across rooms —
indistinguishable), and space mismatch (ranges agree on every level).

**The open question is the premise: does a `padhalllv.c` waypoint link promise a clear STRAIGHT
line, or only reachability?** Dam's wp0→wp5 is crossed by fragments of 45, 15 and 9 units — the
mesh boundary around a pillar. If guards follow the mesh and steer around interior obstacles, a
straight segment clipping a pillar is expected and this audit measures something the engine never
claimed.

⚠️ **One defect regardless: the wall set contains duplicates.** Dam's `(3772,4481)-(3922,4481)`
appears twice in one hit list — a per-tile-edge derivation emits a shared edge once from each side.

## S8. Engine facts: ✅ DONE

**`94abaa2`, `064d84b`.** `tools/gen_engine_facts.py` emits name/value/unit/source:line from the
engine documents; `tools/gen_level_facts.py` mines the 199 structured blocks across 23 per-level
buckets that nothing had consumed.

🔴 **Every record is `status="unverified"`.** The sources are community-written and the ingester's
own note says "not ground truth". A tidy JSON table reads as authoritative in a way the paragraph
did not.

🔴 **And the dimensional claims do not survive checking.** Train's document says 239 m; we measure
35,786 runtime units → 149.73 units/m, against the engine's own 102.78 (`chrheight` 185 at
`chr.c:1936`, a person ≈1.8 m). **Use its topology and tactics, never its distances.**

⚠️ **32 facts from 240 KB, and the low yield *is* the finding** — 1,084 numeric tokens in 11,170
lines, the commonest units being `fps`, `ms`, `kbps`. These are prose narratives, not parameter
tables.

⚠️ **A name collision in `ingest_walkthroughs.py`, still unfixed (mac's):**
`Goldeneye64CameraAndControlsVerbose.txt` is filed under the **`control` level** because the
filename contains "Control". With the multiplayer doc that is ~55 KB, a quarter of the engine
reference, outside `_engine`. The extractors read engine documents *by name* so they are correct
either way.

The original statement of S5 follows.

Transports and discovery are in. Two peers staying identical over thousands of ticks is unproven,
and `gePlayerSeedFingerprint` exists for exactly that. This is entirely offline-testable and does
not touch the bot path.

---

# MAC: runtime work, in order

## M1. Get past the crate and out of the first carriage

Body-aware sensing is in both the router and the CLI. With S1's extents the player can plan around
furniture instead of discovering it. Until then, exhaust what the current data allows.

**Done when:** the CLI player leaves the second carriage without wedging.

## M2. The door that will not open

The player stalls against `wall door object 96 away` where `use` does nothing. Locked, needs an
objective first, or wants a closer approach? The walkthrough says Train's early doors open
normally on Agent, which points at approach distance or the use action not reaching the door.

**What is already ruled out**, and do not re-test these:

- **The button mapping is right.** `GE_IN_USE` maps to `B_BUTTON`, which is GoldenEye's action
  button, and `ge_playback` already copies the button word to the companion pad, so the two-pad
  split is not eating it either.
- **It is not only distance.** The player reached 96 units from a door and `use` still did
  nothing, so "walk closer" is not the whole answer.

**✅ ANSWERED: `doorTestForInteract`, propobj.c:14411.** Three conditions, and we were breaking
two of them:

```c
(door->flags & PROPFLAG_CANNOT_ACTIVATE) == 0
&& door->maxFrac > 0
&& (prop->flags & PROPFLAG_ONSCREEN)        // must be LOOKING at it
...
xdiff*xdiff + zdiff*zdiff < 40000.0f        // 200 units, not 278
&& ydiff < 200.0f && ydiff > -200.0f        // and within 200 vertically
```

🔑 **The door must be ON SCREEN.** Walking past with use held does nothing however close you are;
the bot has to square up to it. That is why 96 units still failed. The bot now turns onto a door
before pressing, and only marks it used once it has actually faced it, since otherwise it
marks doors used that it never opened and walks away from every one.

⚠️ There is a second path when you are further out: same-room plus
`chrpropTestPointInPaddedBoundPad(pos, 150, boundpads)`. So a door has a bound pad you can stand
in, which is a better target than its centre and is already in the setup data.

**Done when:** a CLI player opens a door on Train and walks through it.

## M3. Objective completion detection

Nothing detects that an objective has been *done*. `objectiveregisters1` and the status table
exist and `GETV_OBJ_DEBUG` prints them. Without this a bot cannot know it succeeded, and neither
can a test.

**Done when:** destroying a brake unit flips the objective to complete in the CLI report.

## M3b. An ABSOLUTE frame per level, not just bearings relative to the player

Every spatial answer the API gives is **relative to where the player is looking**: "door 278
away, turn +89". That is the right form for acting on one thing, and it is the wrong form for
building a picture. A bot told only relative bearings has to re-derive the whole world every time
it turns, and it cannot hold a belief like "the objective is at the east end and I have been
working west", which is exactly the belief that would stop it doubling back, as it currently
does on Train.

The engine has an absolute frame already; we are throwing it away at the report boundary.

**Do:**
1. Report absolute world position for every listed thing, alongside the relative bearing. Both,
   not one: the relative form is what you act on, the absolute form is what you remember.
2. Give each level a stated frame: which world axis is north, and the extent of the playable
   volume in each direction. `+x`, `+z` and `+y` are already consistent across the engine; what
   is missing is saying so once, per level, so a consumer never has to infer it.
3. A compass line in the CLI: the player's absolute facing as a cardinal plus the level's bounds,
   so a reader knows where in the MAP they are rather than only what is in front of them.

⚠️ Cardinals are a convenience over the axes, not a new coordinate system. Do not introduce a
second frame that can disagree with the first. Name the axes and derive the compass from them.

**Done when:** the CLI report says where the player is in the level ("west third, facing north")
and every listed object carries an absolute position, and a bot can answer "have I been here
before" without re-deriving it from bearings.

## M4. Co-op: ✅ MOVEMENT VERIFIED, with a control

Player 2 driven through the player API, Train, 6001 frames, against a no-input control:

| | p0 path | p1 path | p1 net |
|---|---|---|---|
| control (no input) | 330 | **0** | 0 |
| bot driving slot 1 | 330 | **1,779** | 1,641 |

Player 2 moves **only** with input, and player 1 is unaffected: 330 in both runs, so the slots do
not bleed into each other. Both spawn apart and both cameras render, which the split-screen
capture already showed.

🔑 This closes the question that opened the week. Co-op movement was never broken: `gePortPlayerPos`
read `player->pos`, which is zeroed at spawn and rarely written, so both players *were* walking
while the accessor returned the same coordinate. The bug was in the measurement, and the measuring
tool was mine.

⚠️ The control is what makes this worth stating. A scripted run on Dam travels 36 units and looks
like proof until the no-script control travels the same 36: the level's own opening walks the
player. Every earlier "co-op moved" claim on this project was that intro, and I withdrew one.

**Remaining:** two *humans* at two pads, which needs a person and a second controller. The API
half is done.

## M6. Z-fighting: a steel plate clipping through the curved carriage wall

Captured on Train: a riveted steel-plate texture punches through the lower half of a curved wall,
appearing in front of geometry it should be behind. Classic depth-precision failure: two
surfaces close enough together that the depth buffer cannot order them, so which one wins varies
with view angle and distance.

⚠️ **Do not "fix" this by nudging geometry.** The assets are the game's own and are correct on
hardware; if a surface has to move, the port is wrong somewhere else. The N64 uses a 15.3
fixed-point depth encoding with a specific near/far arrangement, and `N64_RCP_GRAPHICS.md` §O–§P
carries the current far-plane truth, and a depth range mapped differently in the port would produce
exactly this, and would produce it on flat walls elsewhere too.

**First questions, cheapest first:** does it move with distance or with angle? Does
`GETV_SUPERSAMPLE` change it, which would implicate precision rather than ordering? Does the
plate belong to the room display list or the prop path: the two go through different converters,
and one of them has been wrong before.

**Done when:** the plate renders behind the wall from every angle, and a flat-wall control
elsewhere on Train is unchanged.

## M5. Frame timing, step 3

Fire rates and reaction stepping counted in time rather than iterations. Open-ended; each
conversion needs checking against retail.

---

---

# PHASES: what comes after the bot works

Ordered by dependency, not appetite. Nothing in a later phase should start while an earlier one
is blocking, because every one of them is easier once a bot can be pointed at a level and left
to run.

## 🔑 M0. THE ATTRACT-MODE DEMOS: recorded human play, already in the ROM

Evan asked whether the title-screen gameplay is a bot or a video. **Neither: it is fourteen
recorded input streams**, in `assets/ramrom/`, decoded by `tools/decode_ramrom.py`:

```
bunker1 x2   dam x2   facility x3   frigate x2   runway x2   silo x2   train
32,469 input records across 6,695 blocks, one demo on 00 Agent
```

`ramrom_Train.bin` is **3,418 input records in 485 blocks of a person playing the level
we cannot finish**. The earlier 3,957 was a ceiling: it divided the file by 4, which counts
the interleaved seed records as input. Same correction applies to the totals above.

🔑 **The input record is `{s8 stick_x, s8 stick_y, u8 button_low, u8 button_high}`.** It is
**NOT** the same shape as `GePlayerInput` and does **not** feed straight through
`gePlayerPost`. `GePlayerInput` is `{unsigned int buttons; int stick_x; int stick_y}` -- 12
bytes carrying a `GE_IN_*` bitmask, against the record's 4 bytes of raw N64 pad bits. The
sticks do carry over directly; the buttons need translating.

**The pad word is `(button_high << 8) | button_low`** -- low byte FIRST in memory, which is
not what a big-endian target suggests. Derived, not assumed, two ways: bits `0x0040` and
`0x0080` are assigned to no button on a real N64 pad, and under this order they are clear in
all 32,469 records while the other order sets one in 173 (physically impossible); and under
this order Z is the most-held button at 11.6% with A, B and START each under 0.5%, which is
what firing a gun looks like.

**`L_TRIG` is the AIM button in these demos**, and the decomp settles it rather than the
frequencies alone. `bondview2.c:5546-5558` assigns the single-controller styles two ways:
KISSY and GOODNIGHT get `shoot=A, aim=Z, inv=L_TRIG|R_TRIG`; every other style gets
`shoot=Z, aim=L_TRIG|R_TRIG, inv=A`. The corpus matches the second exactly -- Z most-held at
11.6%, A at 0.1%, `L_TRIG` at 2.8% -- so these were recorded on a default style.

**The port already handles this correctly and needs no change.** `ge_player_api.c:126` sets
`aim = swapped ? Z_TRIG : (L_TRIG | R_TRIG)`, so a default single-controller style gets
`L_TRIG|R_TRIG` exactly as the game expects. The `Z_TRIG` assignment further down is inside
the `GE_STYLE_IS_TWO_PAD` branch and applies only to the 2.x styles, where fire and aim sit
on separate pads. A decoded demo can therefore go through the existing mapping.

`R_TRIG` is never pressed in any of the fourteen, so the `L_TRIG|R_TRIG` pair only ever
appears as `L_TRIG` alone. Every bit in the corpus now translates; nothing is dropped.

**Two things this gives us that we were building by hand:**

1. **Ground truth for navigation.** Every path question we have been guessing at (which way out
   of the first carriage, how close you stand to a door, when to fight rather than walk) was
   answered by someone who could see the screen, and the game kept their answer.
2. **A determinism test, already written.** The stream interleaves
   `{speedframes, count, randseed, check}` and `ramromreplay` aborts when the running RNG
   disagrees. That is precisely what netplay needs, with fourteen recorded cases to run it
   against, and `gePlayerSeedFingerprint` already exposes our half.

**Do:** ~~find the exact header length so the input and seed records can be separated~~ DONE,
header is **232 bytes** and the block walk is confirmed (see below); (the offset
is derived, not assumed: a guessed one yields plausible sticks and garbage buttons); replay
Train through the player API; compare our seed fingerprint against the recorded one each block.

**HEADER AND STREAM STRUCTURE: RESOLVED.** `tools/audit_ramrom_header.py` is the regression check.

`ramromreplay.c:453` advances the cursor by `sizeof(struct ramromfilestructure)`, so the header
length IS that struct's size: 228 padded to **232** under MIPS alignment, with `s32 filesize` at
offset 128 and `enum LEVELID stagenum` at 16. Hand arithmetic over twenty fields and a nested
`save_data` is not evidence, so both offsets were tested against the data instead of asserted:

  * offset 128 holds the file size in **all 14 demos**, once ROM padding is modelled. The stored
    value is the demo's own length and the file is padded up to a 16-byte boundary, so exact
    equality holds in only 3 of 14 -- searching for it alone makes the right offset look like a
    coincidence.
  * offset 16 holds the correct level id in **all 14**, but only against the `LEVELID` enum, not
    `LEVEL_SOLO_SEQUENCE`. Both enums name every level and `SP_LEVEL_TRAIN` is 13 while
    `LEVELID_TRAIN` is 25. Using the wrong one made the offset look unfindable and briefly
    implicated the arithmetic, which was right all along.
  * a third confirmation arrived unlooked-for: `difficulty` at offset 20 reads 0 for every demo
    except one, which reads 2. Exactly one of the fourteen was played on 00 Agent.

**The block stream:** a 4-byte `ramrom_seed` {speedframes, count, randseed, check}, then
`size_cmds * 4 * count` bytes of input, repeating until a block with `count` and `speedframes`
both zero. The advance is written as `align_addr_even(size_cmds * 4 * count + 5)`, and
`align_addr_even` rounds DOWN to even, so on a multiple of 4 the `+5` is simply `+4`, the seed
record. The decomp's "5 is ??" comment resolves to nothing mysterious.

**This walk lands on the stored filesize in all 14 files**, which is what confirms the structure
rather than merely fitting it.

**Still to do:** replay Train through the player API and compare our seed fingerprint against the
recorded `randseed` each block.

**Done when:** `ramrom_Train.bin` replays and the player follows the recorded path, at which
point we have a working reference route AND a determinism check in one artefact.

⚠️ ROM-derived. Decoded output stays out of git like every other asset.

## Phase 2: the API becomes a platform

**Bot personalities against real levels.** Eighteen archetypes exist and were only ever tested
against a placeholder. With measured routes, difficulty can mean something: reaction time,
accuracy, whether a bot breaks contact when hurt.

**Horde mode.** The cheat system already gives guards any weapon and the graph knows where they
can come from. That is most of a wave spawner, and it is the first thing that is *fun* rather
than correct.

**Co-op verified with two humans.** M4. `GETV_SCRIPT` moves nobody, so this needs the player API
driving slot 1 or a person at the second pad.

**Netplay determinism over a long run.** S5. Two peers staying identical over thousands of ticks
is unproven and `gePlayerSeedFingerprint` exists for exactly that.

## Phase 3: presentation and platforms

**The Metal backend, ported from akratch/mgb64.** MIT, archived, and the one capability they have
that we lack; see the prior-art note below.

🔑 **This is the tvOS unlock, and tvOS was this project's original goal.** GL ES is deprecated on
Apple platforms and Metal is the supported path; the OpenGL renderer we run today is a dead end
there however well it works on desktop.

⚠️ **Port it, do not copy it.** Theirs is written against their platform layer. The
sm64ex-versus-libultraship lesson has already cost this project five separate bugs: a reference
implementation written for a different tree is actively misleading even when it descends from the
same code. Budget it as a rewrite with a working reference, not a transplant.

**Then tvOS itself.** The build/sign/deploy loop already exists from the earlier work and rendered
frames on the Apple TV. What killed it was the renderer, which is what Metal fixes.

**Frame timing step 3.** M5. Fire rates and reaction stepping counted in time rather than
iterations. Open-ended, and each conversion needs checking against retail.

**Z-fighting and the visual bugs.** M6 and the colour-decode family in `COLOUR_BUGS.md`.

## Phase 4: the thing this is actually for

**A mod surface people outside this repo can use.** The world API, sensing, the event bus and Lua
already exist. What is missing is documentation aimed at someone who has never read the decomp,
and a mod that does something a person would want rather than something that proves a seam works.

**An agent that plays the game.** The CLI is the interface and it already works. Everything past
that is policy, and policy is cheap once perception is honest.

## Prior art: akratch/mgb64, worth mining, and MIT

Another native GoldenEye port from the same `n64decomp/007` base. **687 commits, archived
2026-08-18**, read-only. Its own notice says "other community projects have since surpassed this
one".

**First-party code is MIT**, which means we may actually use it with attribution, unlike
GoldenRecomp (Windows-only binaries) or GoldenPad (not reproducible from its own repo), both of
which looked more useful than they were.

🔑 **The one thing it has that we do not: a METAL rendering backend**, in `src/platform/`. That
matters more than it sounds, because tvOS was this project's original goal and Metal is the
supported path there: GL ES is deprecated. We are OpenGL-only today.

Its shape is otherwise close to ours: SDL2, an in-process ImGui launcher, a libultra shim,
assets read from the user's own ROM at runtime, and a faithful-versus-remaster split.

⚠️ **It has no co-op, no bots, no player or world API, no netplay, and no mod surface.** That is
worth stating because it tells us what this project is actually for: the port itself is no longer
the differentiator, and the API layer is.

**If we take anything, take the Metal backend**, and take it as a port, not a copy. Their
renderer is written against their platform layer, not ours, and the SM64-versus-libultraship
lesson applies: a reference implementation written for a different tree is actively misleading
even when it descends from the same code. Credit them in `README.md` alongside Rare and the
decomp team.

## Standing corrections: do not re-derive these

- **`levelscale`**: `runtime = asset / levelscale`. 19 of 20 spawn "failures" were this.
- `player->pos` is not the world position; `prop->pos` is.
- `GETV_SCRIPT` does not move the player. Use `gePlayerPost`.
- Doors are not walls. `CDTYPE_DOORS` in a mask makes every room read as sealed.
- The tile argument to `bondviewTestLineUnobstructed` is an **input**; NULL reports everything
  obstructed. Seed it by standing at the node.
- The asker's own body sets `GE_SENSE_BODY`. Steer on `GE_SENSE_SOLID`.
- Positive stick DECREASES heading. **The turn sign has been wrong three times.**
- Commit to a manoeuvre. Re-deciding every tick has caused **five** oscillation stalls.
- An always-true condition at the top of a policy is a deadlock wearing a sensible face: at a
  2,000-unit engagement range there is always a guard with line of sight, so the player stood at
  a door trading shots for a whole run.
- An unset `lastknowntargetpos` is (0,0,0), a real coordinate. Report it absent.
- `vendor/` is gitignored: decomp symbols travel by patch, `git apply` says "Skipped patch" on the
  Surface's tree, so deliver the file and **verify by name**.
- Run the control. Never sample a trace with `tail -1`.
- ⚠️ **A third writer edits this repo** under the same git identity. Check `git log` before
  assuming your tree is yours.
