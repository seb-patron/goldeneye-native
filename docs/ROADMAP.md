# Roadmap

One list, both machines. Every item says what "done" means, because a task without a finish
condition gets reported as finished by whoever is tired.

Lane rules are in `docs/COLLABORATION.md`: work flows Surface → Mac → `main`, the Mac integrates
file by file. Sync with `tools/sync_surface.sh` — it pushes, pulls, prints the diffstat without
merging, and lists the other side's uncommitted work.

---

## The objective we are both working toward

**A bot completes a mission objective on Train, and the same code does it on a second level.**

Everything below is ordered by what blocks that. The CLI (`GETV_CLI=1`) is the measuring
instrument: if a person can play a level from the report, the API is complete and the rest is
policy. Today a person gets 754 units closer and then stalls, and we know exactly where.

---

## 🔴 1. PROP EXTENTS — Surface, and it is the current blocker

**Every position in the pack is a POINT and the world is made of solids.**

A crate reported as "278 away" is 278 units to its *centre*. A bot that still sees room has
already walked into the corner of it. That is precisely how the CLI player got trapped on Train
while the report insisted there was space, and no amount of policy fixes it — the information is
not there.

The engine knows: `chrobjGetBboxFromObjectRecord` plus `chrpropBBOXGetXmin`/`Zmin` give the model
bounding box, and `gePortPropExtent` (mine, landed, compiles) reads them. **It cannot be driven,**
because the engine exposes no enumerable prop list — props live behind the object handler and the
only global arrays are the tank's.

So it belongs offline, in the pack, from the same model data the extractor already reads.

**Do:** emit `hx`, `hz` and `radius` per prop in the level knowledge; `pack_world.py` gains three
floats per prop record; `GeWorldProp` gains them; the CLI reports `crate 278 away, radius 120`
so a reader knows the surface is at 158.

⚠️ **Scale them.** Extents are asset-space lengths and the pack is runtime space now — a radius
left unscaled is wrong by `1/levelscale`, which is 6.7× on Train.

⚠️ **The model box is unrotated.** A long crate at forty-five degrees occupies more width than its
half-extent suggests. Report the radius as well and say which is which; do not silently pick one.

**Done when:** the CLI's `near` lines carry a radius, and the Train player walks past the crate it
currently traps itself on.

## 🔴 2. NAVMESH NODES — Surface

Pads are prop markers, not places to walk. Measured with the teleport probe: **139 of Train's 180
nodes cannot be stood on**, and across the twenty solo levels it runs 33 to 240 each. A follower
given targets a body cannot occupy is short by however far the pad sits off the floor, always.

You already emit tile adjacency and 1,100 stairways. The tiles *are* the graph: a node per floor
tile, or per cluster, is standable by construction, has a real height, and needs no snapping.

**Done when:** routing Train produces a line through seven doors — the walkthrough says seven
brake units in a linear chain of carriages, and the CLI says every landmark is 90° to the right.
If the graph reproduces that shape it is right, and this is checkable without a bot.

## 3. THE DOOR THAT WILL NOT OPEN — either of us

The CLI player stalls against `wall door object 278 away` where `use` does nothing. Locked, needs
an objective first, or wants a closer approach? The walkthrough says Train's early doors open
normally on Agent, which points at approach distance or the use action not reaching the door.

**Done when:** a CLI player opens a door on Train and walks through it.

## 4. ENEMY FACING — Surface

`gePortEnemyFacing` refuses and `geSenseNoticedBy` falls back to line of sight, so a bot hides
from a guard facing away and strolls past one staring at it. `ChrRecord` has no facing field I
could find; `chr.c:2319` reads `chr->aimsideback` into a `yrot` when building the model matrix,
which is where the answer probably starts.

**Done when:** Train reports fewer watchers than it has guards with a clear line, and the
difference is guards facing away.

## 5. Objective completion — Mac

Nothing yet detects that an objective has been *done*. `objectiveregisters1` and the status table
exist; `GETV_OBJ_DEBUG` prints them. Without this a bot cannot know it succeeded.

**Done when:** a CLI player destroying a brake unit sees the objective flip to complete.

---

## After the objective is reached

**Bot personalities against real levels.** Eighteen archetypes tested against a placeholder.
Difficulty should mean reaction time, accuracy and whether a bot retreats.

**Netplay determinism over a long run.** Transports and discovery are in. Two peers staying
identical over thousands of ticks is unproven, and the seed fingerprint exists for exactly that.

**Horde mode.** The cheat system gives guards any weapon and the graph knows where they come
from. That is most of a wave spawner.

**Co-op verified with two humans.** `GETV_SCRIPT` moves nobody, so this needs the player API
driving slot 1 or a person.

**Frame timing step 3.** Fire rates and reaction stepping counted in time rather than iterations.

---

## Standing corrections — do not re-derive

- **`levelscale`**: runtime = asset / levelscale. The pack scales at the boundary. 19 of 20
  spawn "failures" were this.
- `player->pos` is not the world position; `prop->pos` is.
- `GETV_SCRIPT` does not move the player. Use `gePlayerPost`.
- Doors are not walls. `CDTYPE_DOORS` in a mask makes every room read as sealed.
- The tile argument to `bondviewTestLineUnobstructed` is an **input**; NULL reports everything
  obstructed. Seed it by standing at the node.
- The asker's own body sets `GE_SENSE_BODY`. Steer on `GE_SENSE_SOLID`.
- Positive stick DECREASES heading. The turn sign has been wrong three times.
- Commit to a manoeuvre. Re-deciding every tick has caused four separate oscillation stalls.
- An unset `lastknowntargetpos` is (0,0,0), a real coordinate. Report it absent.
- `vendor/` is gitignored: decomp symbols travel by patch, and `git apply` says "Skipped patch"
  on the Surface's tree, so deliver the file and verify by name.
- Run the control. Never sample a trace with `tail -1`.
