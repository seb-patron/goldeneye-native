# Roadmap

One goal, two machines, divided by **what each can actually do** rather than by topic.

> **The objective:** a bot completes a mission objective on Train, and the same code does it on a
> second level. Everything below is ordered by what blocks that.

The measuring instrument is the CLI (`GETV_CLI=1`). If a person can play a level from the report,
the API is complete and the rest is policy. Today a person gets the objective from 14,649 to
13,315, holding 62% health and returning fire, and then wedges. We know where and why.

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

**Done when:** a CLI player opens a door on Train and walks through it.

## M3. Objective completion detection

Nothing detects that an objective has been *done*. `objectiveregisters1` and the status table
exist and `GETV_OBJ_DEBUG` prints them. Without this a bot cannot know it succeeded — and neither
can a test.

**Done when:** destroying a brake unit flips the objective to complete in the CLI report.

## M4. Co-op with two humans

Both players spawn apart, both cameras work, both walk. `GETV_SCRIPT` moves nobody, so this needs
the player API driving slot 1 or a person at the second pad.

## M5. Frame timing, step 3

Fire rates and reaction stepping counted in time rather than iterations. Open-ended; each
conversion needs checking against retail.

---

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
