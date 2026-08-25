# Roadmap

One goal, two machines, divided by **what each can actually do** rather than by topic.

> **The objective:** a bot completes a mission objective on Train, and the same code does it on a
> second level. Everything below is ordered by what blocks that.

The measuring instrument is the CLI (`GETV_CLI=1`). If a person can play a level from the report,
the API is complete and the rest is policy. Today a person gets the objective from 14,649 to
13,315, holding 62% health and returning fire, and then wedges. We know where and why.

## Staying 1:1 — check it, do not assume it

`tools/sync_surface.sh` verifies both halves and prints OK or MISMATCH:

```
== 1:1 CHECK ==
  commits   mac=cb0d534e0f  surface=cb0d534e0f  OK
  decomp    mac=5f4c9f41fee5  surface=5f4c9f41fee5  OK
```

Being in sync has **two independent parts and only one travels in a bundle**:

1. **Commits.** Carried by the bundle. Compared on FULL hashes — git picks short-hash length per
   repo, so the two sides can abbreviate the same commit differently and a naive compare reports
   a tree mismatching itself. A check that cries wolf gets ignored.
2. **`vendor/**`.** **Gitignored, so decomp symbols do NOT travel.** The Surface lost a day to a
   build that compiled and would not link for exactly this. Compared by SHA-1 of
   `objective_status.c`, because a name check proves one symbol arrived and a hash proves the
   whole file did.

⚠️ **`git apply` reports "Skipped patch" on the Surface's vendor tree** — its ignore rules exclude
that path — so a patch that verifies clean can still land nothing while every signal says it
worked. **Deliver the file by scp and verify the hash.** Patches are for the record, not the
transport.

## The division, and why it is this one

**The Surface renders at roughly one frame per second.** It cannot measure anything at runtime,
which is why `dump_spawns.py` and the edge validator came back to the Mac. That is not a
judgement, it is a constraint, and the split follows from it:

| | |
|---|---|
| **Mac** — anything needing a running game | bots, co-op, the CLI, measurement, `vendor/**`, input, docs |
| **Surface** — anything offline | extraction, audits, tests, Windows, the launcher, netplay |

Work flows Surface → Mac → `main`; the Mac integrates file by file. Sync with
`tools/sync_surface.sh`. Read the diffstat before taking anything — four reverts were caught that
way today and none reached `main`.

🔑 **Whoever owns the CONSUMER keeps the code** when both sides write the same thing. We each
deleted our own walkability reader in the same hour in favour of the other's; for a few minutes
neither existed.

---

# SURFACE — offline work, in order

## S1. Prop extents 🔴 the current blocker, and nothing else unblocks it

**Every position in the pack is a POINT and the world is made of solids.** A crate reported "278
away" is 278 units to its *centre*. A bot that still sees room has already walked into the corner
of it. That is how the CLI player wedges on Train while the report insists there is space, and no
policy fixes it because the information is not in the data.

**Why it is yours:** `gePortPropExtent` is written, compiles, and is on your box — it reads
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
radius is wrong by `1/levelscale` — 6.7× on Train, 4.3× on Dam.

⚠️ **The model box is unrotated.** A long crate at forty-five degrees occupies more width than its
half-extent suggests. Emit the radius as well and label which is which; do not silently pick one.

⚠️ **Round-trip tolerance is RELATIVE now.** f32 keeps seven significant digits, not seven decimal
places, and runtime coordinates reach 86,000. A flat `1e-3` fails seven levels.

**Done when:** the CLI's `near` lines carry a radius, and the Train player walks past the crate it
currently traps itself on.

## S2. Navmesh nodes — the structural fix behind most of our failures

Pads are prop markers, not places to walk. Measured with the teleport probe: **139 of Train's 180
nodes cannot be stood on**, and across the twenty solo levels it runs 33 to 240 each. A follower
given targets a body cannot occupy is short by however far the pad sits off the floor, always —
that is not a tuning problem.

You already emit tile adjacency, floor tiles and 1,100 stairways. **The tiles are the graph.** A
node per floor tile, or per cluster, is standable by construction, has a real height, and needs no
snapping to place. Pads go back to being what they are: markers for props and spawns.

⚠️ **Declare which coordinate space the extractor emits.** The pack scales at the boundary today
and that is fine, but it must be stated rather than assumed. `runtime = asset / levelscale`.

**Done when:** routing Train produces a line through seven doors. The walkthrough says seven brake
units in a linear chain of carriages; the CLI says every landmark is 90° right. If the graph
reproduces that shape it is right — **and you can check that without a bot and without a running
game**, which is the point.

## S3. Enemy facing — turn sight into attention

`gePortEnemyFacing` refuses and `geSenseNoticedBy` falls back to line of sight, so a bot hides
from a guard facing the other way and strolls past one staring at it. Train reports 17–19 watchers
of 40 guards, which is what an unobstructed line down a row of carriages looks like — **not**
seventeen guards watching.

`ChrRecord` has no facing field I could find. `chr.c:2319` reads `chr->aimsideback` into a `yrot`
when building the model matrix; that is where the answer probably starts.

⚠️ **Do not tune the line test to shrink the watcher count.** Add the cone. And keep both
questions: "could see me if it turned" is different from "is looking at me", and both are useful.

**Done when:** Train reports fewer watchers than it has guards with a clear line, and the
difference is guards facing away.

## S4. Audit the line-versus-body substitution across your callers

The wedge was a **lying sensor, not a bad policy**: `geSenseClearestHeading` is a line test and a
line has no width, so a gap narrower than the player reads as the clearest heading available and
the router commits to the one direction it cannot fit through. Every trace says it chose
correctly.

Fixed in the router and the CLI. **Check your own callers for the same substitution.** The line
version is still right for questions genuinely about a line — whether a shot or a sightline
reaches.

## S5. Netplay determinism over a long run

Transports and discovery are in. Two peers staying identical over thousands of ticks is unproven,
and `gePlayerSeedFingerprint` exists for exactly that. This is entirely offline-testable and does
not touch the bot path.

---

# MAC — runtime work, in order

## M1. Get past the crate and out of the first carriage

Body-aware sensing is in both the router and the CLI. With S1's extents the player can plan around
furniture instead of discovering it. Until then, exhaust what the current data allows.

**Done when:** the CLI player leaves the second carriage without wedging.

## M2. The door that will not open

The player stalls against `wall door object 96 away` where `use` does nothing. Locked, needs an
objective first, or wants a closer approach? The walkthrough says Train's early doors open
normally on Agent, which points at approach distance or the use action not reaching the door.

**What is already ruled out** — do not re-test these:

- **The button mapping is right.** `GE_IN_USE` maps to `B_BUTTON`, which is GoldenEye's action
  button, and `ge_playback` already copies the button word to the companion pad, so the two-pad
  split is not eating it either.
- **It is not only distance.** The player reached 96 units from a door and `use` still did
  nothing, so "walk closer" is not the whole answer.

**✅ ANSWERED — `doorTestForInteract`, propobj.c:14411.** Three conditions, and we were breaking
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
before pressing, and only marks it used once it has actually faced it — otherwise it marks doors
used that it never opened and walks away from every one.

⚠️ There is a second path when you are further out: same-room plus
`chrpropTestPointInPaddedBoundPad(pos, 150, boundpads)`. So a door has a bound pad you can stand
in, which is a better target than its centre and is already in the setup data.

**Done when:** a CLI player opens a door on Train and walks through it.

## M3. Objective completion detection

Nothing detects that an objective has been *done*. `objectiveregisters1` and the status table
exist and `GETV_OBJ_DEBUG` prints them. Without this a bot cannot know it succeeded — and neither
can a test.

**Done when:** destroying a brake unit flips the objective to complete in the CLI report.

## M3b. An ABSOLUTE frame per level, not just bearings relative to the player

Every spatial answer the API gives is **relative to where the player is looking**: "door 278
away, turn +89". That is the right form for acting on one thing, and it is the wrong form for
building a picture. A bot told only relative bearings has to re-derive the whole world every time
it turns, and it cannot hold a belief like "the objective is at the east end and I have been
working west" — which is exactly the belief that would stop it doubling back, as it currently
does on Train.

The engine has an absolute frame already; we are throwing it away at the report boundary.

**Do:**
1. Report absolute world position for every listed thing, alongside the relative bearing. Both,
   not one — the relative form is what you act on, the absolute form is what you remember.
2. Give each level a stated frame: which world axis is north, and the extent of the playable
   volume in each direction. `+x`, `+z` and `+y` are already consistent across the engine; what
   is missing is saying so once, per level, so a consumer never has to infer it.
3. A compass line in the CLI: the player's absolute facing as a cardinal plus the level's bounds,
   so a reader knows where in the MAP they are rather than only what is in front of them.

⚠️ Cardinals are a convenience over the axes, not a new coordinate system. Do not introduce a
second frame that can disagree with the first — name the axes and derive the compass from them.

**Done when:** the CLI report says where the player is in the level ("west third, facing north")
and every listed object carries an absolute position, and a bot can answer "have I been here
before" without re-deriving it from bearings.

## M4. Co-op — ✅ MOVEMENT VERIFIED, with a control

Player 2 driven through the player API, Train, 6001 frames, against a no-input control:

| | p0 path | p1 path | p1 net |
|---|---|---|---|
| control (no input) | 330 | **0** | 0 |
| bot driving slot 1 | 330 | **1,779** | 1,641 |

Player 2 moves **only** with input, and player 1 is unaffected — 330 in both runs, so the slots do
not bleed into each other. Both spawn apart and both cameras render, which the split-screen
capture already showed.

🔑 This closes the question that opened the week. Co-op movement was never broken: `gePortPlayerPos`
read `player->pos`, which is zeroed at spawn and rarely written, so both players *were* walking
while the accessor returned the same coordinate. The bug was in the measurement, and the measuring
tool was mine.

⚠️ The control is what makes this worth stating. A scripted run on Dam travels 36 units and looks
like proof until the no-script control travels the same 36 — the level's own opening walks the
player. Every earlier "co-op moved" claim on this project was that intro, and I withdrew one.

**Remaining:** two *humans* at two pads, which needs a person and a second controller — the API
half is done.

## M6. Z-fighting: a steel plate clipping through the curved carriage wall

Captured on Train: a riveted steel-plate texture punches through the lower half of a curved wall,
appearing in front of geometry it should be behind. Classic depth-precision failure — two
surfaces close enough together that the depth buffer cannot order them, so which one wins varies
with view angle and distance.

⚠️ **Do not "fix" this by nudging geometry.** The assets are the game's own and are correct on
hardware; if a surface has to move, the port is wrong somewhere else. The N64 uses a 15.3
fixed-point depth encoding with a specific near/far arrangement, and `N64_RCP_GRAPHICS.md` §O–§P
carries the current far-plane truth — a depth range mapped differently in the port would produce
exactly this, and would produce it on flat walls elsewhere too.

**First questions, cheapest first:** does it move with distance or with angle? Does
`GETV_SUPERSAMPLE` change it, which would implicate precision rather than ordering? Does the
plate belong to the room display list or the prop path — the two go through different converters,
and one of them has been wrong before.

**Done when:** the plate renders behind the wall from every angle, and a flat-wall control
elsewhere on Train is unchanged.

## M5. Frame timing, step 3

Fire rates and reaction stepping counted in time rather than iterations. Open-ended; each
conversion needs checking against retail.

---

---

# PHASES — what comes after the bot works

Ordered by dependency, not appetite. Nothing in a later phase should start while an earlier one
is blocking, because every one of them is easier once a bot can be pointed at a level and left
to run.

## Phase 2 — the API becomes a platform

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

## Phase 3 — presentation and platforms

**The Metal backend, ported from akratch/mgb64.** MIT, archived, and the one capability they have
that we lack — see the prior-art note below.

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

## Phase 4 — the thing this is actually for

**A mod surface people outside this repo can use.** The world API, sensing, the event bus and Lua
already exist. What is missing is documentation aimed at someone who has never read the decomp,
and a mod that does something a person would want rather than something that proves a seam works.

**An agent that plays the game.** The CLI is the interface and it already works. Everything past
that is policy, and policy is cheap once perception is honest.

## Prior art: akratch/mgb64 — worth mining, and MIT

Another native GoldenEye port from the same `n64decomp/007` base. **687 commits, archived
2026-08-18**, read-only. Its own notice says "other community projects have since surpassed this
one".

**First-party code is MIT**, which means we may actually use it with attribution — unlike
GoldenRecomp (Windows-only binaries) or GoldenPad (not reproducible from its own repo), both of
which looked more useful than they were.

🔑 **The one thing it has that we do not: a METAL rendering backend**, in `src/platform/`. That
matters more than it sounds, because tvOS was this project's original goal and Metal is the
supported path there — GL ES is deprecated. We are OpenGL-only today.

Its shape is otherwise close to ours: SDL2, an in-process ImGui launcher, a libultra shim,
assets read from the user's own ROM at runtime, and a faithful-versus-remaster split.

⚠️ **It has no co-op, no bots, no player or world API, no netplay, and no mod surface.** That is
worth stating because it tells us what this project is actually for: the port itself is no longer
the differentiator, and the API layer is.

**If we take anything, take the Metal backend** — and take it as a port, not a copy. Their
renderer is written against their platform layer, not ours, and the SM64-versus-libultraship
lesson applies: a reference implementation written for a different tree is actively misleading
even when it descends from the same code. Credit them in `README.md` alongside Rare and the
decomp team.

## Standing corrections — do not re-derive these

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
- An unset `lastknowntargetpos` is (0,0,0) — a real coordinate. Report it absent.
- `vendor/` is gitignored: decomp symbols travel by patch, `git apply` says "Skipped patch" on the
  Surface's tree, so deliver the file and **verify by name**.
- Run the control. Never sample a trace with `tail -1`.
- ⚠️ **A third writer edits this repo** under the same git identity. Check `git log` before
  assuming your tree is yours.
