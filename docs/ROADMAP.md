# Roadmap

Where this actually stands, and what each side does next. Numbers here are measured, not
estimated; anything unverified says so.

`docs/COLLABORATION.md` has the lane rules — the short version is that work flows
Surface → Mac → `main`, and the Mac integrates file by file.

---

## What exists, measured

**The port runs on three platforms.** macOS 167/1 game objects and 41 port objects, Windows
977 objects, Linux building. FXAA, CRT and supersampling on all three.

**The extraction is the asset nobody else has.** Across 28 levels:

| | |
|---|---|
| floor tiles | 31,616 |
| pads | 5,388 |
| props | 4,871 |
| walls | 4,842 |
| waypoints | 3,390 |
| portals | 1,749 |
| in-game messages | 739 |
| objectives | 80 |

Props resolve by type — 664 Guards, 459 Doors, 976 Collectables, 26 Keys, 122 AmmoBoxes, 41
Drones — each with a position, a pad, and the nav node nearest to it. Objectives carry their
target tags, rooms, difficulty and completion flags. **This is the part that outlives us**: a
modder or an agent can ask where the keys are, which door leads where, and what a mission wants,
without touching the ROM.

**The player API is complete.** All thirteen state fields fill — position, angle, room, health,
armour, dead, weapon, ammo, kills, deaths, shots — behind a flags word so "absent" and "zero"
stay different. Input injection is tick-accurate. Seven test suites, all green.

**Walkability is measured, not assumed.** Every waypoint pair within 1600 units, on all 20 solo
levels, tested with the engine's own line test and written to `<level>.walkable.json`.

**Bots move.** On Train the follower joins its route, turns, and walks 472 units of path for 317
net displacement, closing 986 → 670 on its first waypoint, with no threat-hold jams.

## What does not work yet

**No bot has reached a waypoint.** Closest approach is 670 units on Train against an arrive
radius of 120.

**15 of 20 levels have no walkable edge out of the spawn at all.** Ranked by walkable edges from
the spawn node:

```
train 23   silo 19   bunker1 14   facility 9   jungle 2
archives aztec bunker2 caverns control cradle dam depot egypt
frigate runway statue streets surface surface2 -- all 0
```

⚠️ **Test on Train, not Bunker 1.** Bunker 1 starts in a corridor 120 units wide whose nearest
walkable edge is 938 units away through a door, which makes every bot change look like it failed.
Train's nearest is 664 with 23 options and 19 route steps.

**The graph is planar.** Nodes carry `y` and nothing routes on it.

**Room numbering does not agree between the engine and the extractor.** The engine puts Bunker
1's spawn in stan room 29; the extractor's room 29 is 6 floor tiles 1,108 units away. No floor
tile contains the spawn's XZ at all. Until that is settled, any lookup keyed by room and checked
against a position is comparing two different things.

---

## Surface — in order

### 1. Reconcile the room spaces 🔴 blocks everything below it

Find whether `floors` is keyed to stan rooms or bg rooms, and whether a mapping exists.

**Finish condition:** for Bunker 1's spawn XZ (−1381, 2284) you can name the floor tile the
engine is standing on at y=172. Today no tile contains that point, which points at a coverage
hole where the player starts rather than a numbering mismatch — and that is checkable either way.

### 2. `waypoint_floor`, keyed to whichever space survives step 1

Height of the floor directly beneath every node. Hand over the function so the Mac can apply the
same rule to the synthetic spawn, door and portal nodes.

**Finish condition:** every node in `bunker1.json` and `train.json` has a floor height, and none
of them disagrees with a runtime `gePortProbeStandable` at the same XZ by more than a step.

### 3. Trustworthy seeds for the edge validator

The verdicts change with the seed: 98% walkable seeded from the player's tile, 73% from a stan
lookup per line, 0% with any snap guard. Put a body at every node and test from there.

⚠️ Verify the move landed. A clamped teleport seeds from the old position and produces confident,
plausible, wrong verdicts while the run still reports a percentage.

**Finish condition:** Bunker 1's spawn has no walkable edge to the portal at 1109 units, and does
have one along the corridor toward the door.

### 4. The eight levels with no node in their spawn room

`aztec cradle dam depot runway statue surface surface2`. Dam's nearest node is 20,254 units from
its spawn, which is a different coordinate space, not a bad pad. Do not widen the threshold.

### 5. Netplay determinism over a long run

Transports and discovery are in. Two peers staying identical over thousands of ticks is not
proven, and the seed fingerprint exists for exactly that.

---

## Mac — in order

### 1. Make the data reachable from a mod and an agent

The extraction is the most valuable thing here and almost none of it is exposed at runtime. The
world API serves waypoints, guards and route steps; it does not serve **objectives, keys, doors,
collectables, or the 739 in-game messages**.

**Finish condition:** a Lua mod can ask "where is the nearest key", "which door leads to room N",
"what does objective 2 want" and get an answer, on any of the 20 levels.

### 2. Heights in the graph

Once `waypoint_floor` lands, route on it: a stairway becomes a chain of nodes rather than one
impossible edge, and an edge whose endpoints differ by more than a step is refused.

### 3. Get one bot to an objective, end to end, on Train

The remaining gap after heights. Then the same on a second level, which is what turns it from a
demonstration into a feature.

### 4. Co-op verified with two humans

Both players spawn apart, both cameras work, both walk. The harness cannot prove this —
`GETV_SCRIPT` moves nobody — so it needs either the player API driving slot 1 or a person.

### 5. Frame timing, step 3

Fire rates and reaction stepping converted from counting iterations to counting time. Open-ended,
and each conversion needs checking against retail.

---

## Standing corrections — do not re-derive

- `player->pos` is not the world position; `prop->pos` is.
- `GETV_SCRIPT` does not move the player. Use `gePlayerPost`.
- Doors are not walls. `CDTYPE_DOORS` in a mask makes every room read as sealed.
- The tile argument to `bondviewTestLineUnobstructed` is an **input**; NULL reports everything
  obstructed.
- `gePlayerSlotIsDrivable` must not return `!IS_TWO_PAD` — every slot is 2.x by default.
- An unset `lastknowntargetpos` is (0,0,0), which is a real coordinate. Report it as absent, or
  every unalerted guard on the map appears to be heading for the origin.
- `vendor/` is gitignored. A symbol the port layer calls belongs in `getv/patches/` the same day.
- Run the control. Never sample a trace with `tail -1`.
