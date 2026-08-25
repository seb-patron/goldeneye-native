# Surface queue

Ordered. Everything above the line is unblocked and has a clear finish condition; the rest is
real work with the reasoning attached so it can be picked up without asking.

Path locks and lane rules are in `docs/LACUNARI_SURFACE.md`. Fetch `C:\mac-work.bundle` before
touching a shared file — `ge_player_api.c` and `gen_level_routes.py` have each been reverted once
by a push that predated the other side's work.

---

## 1. Trustworthy seeds for the edge validator 🔴 the one that unblocks the bots

The validator measures every waypoint pair with the engine's own line test, and the answer
depends entirely on the seed tile. Same level, same 2926 pairs on Bunker 1:

| seed | walkable |
|---|---|
| player 0's tile | 98% |
| stan lookup at each line's start | 73% |
| snap-distance guard, any tolerance | 0% |

`sub_GAME_7F0AFB78` snaps to the nearest standable tile, and *nearest* can be through a wall — so
a snap-seeded test reports clear lines the player demonstrably cannot walk. The spawn measures a
clear 1109-unit line to a portal that a reachability map drawn from the player's own tile shows
is behind a wall.

**The only seed known to be correct is the tile a body is standing on.** So put a body at every
node: move the player to each waypoint during a validation run and test from there. It mutates
player state, which a dedicated validation mode can afford.

Finish condition: Bunker 1's spawn has no walkable edge to the portal at 1109 units, and does
have one along the corridor toward the door.

## 2. Heights — the graph is planar and levels are not

Every node carries `y` and nothing routes on it. Bunker 1's spawn is at y=340 and both of its
portals are at y=93, so the bot beelines at a doorway 247 units below it, through a floor. Your
`audit_route_heights.py` is the right start; what is missing is the descent being *in* the graph,
so a stairway is a chain of nodes rather than one impossible edge.

⚠️ The body position and the floor differ by ~157 units. Compare a pad height to a player
position and you get a phantom cliff in every direction at once.

## 3. Eight levels have no node in their spawn room

`aztec, cradle, dam, depot, runway, statue, surface, surface2` — no graph node within 4000 units
of where the player actually starts, and `dam` is 20,254 away. That is not a wrong pad, it is a
graph that does not cover the level's own start, and for Dam a different coordinate space
entirely. `docs/captures/spawns.json` has the measured truth for all twenty.

Do **not** fix this by widening the threshold — the refusal is deliberate and named per level.

## 4. Bot behaviour beyond following a line

The follower turns, walks, presses the action button at obstacles and holds for contested
waypoints. It cannot open a locked door, take a lift, shoot what blocks it, or pick up a key it
routes past. The objective data already names target tags and rooms; `Key` props are in the prop
export.

---

## Later, in no strict order

**Netplay on a measured tick.** Discovery and the transports are in. What is not proven is that
two peers stay identical over a long run — the seed fingerprint exists for exactly this and has
not been used in anger.

**Bot personalities against real levels.** Eighteen archetypes exist and were tested against a
placeholder. Now that routes are measured, difficulty should mean something: reaction time,
accuracy, whether a bot retreats.

**Horde mode.** The cheat system can give guards any weapon, and the route graph knows where they
can come from. That is most of a wave spawner.

**The launcher's mod page.** Mods are loadable and the API is a platform now; the launcher still
presents them as a checkbox list with no description of what any of them do.

**A second control style verified end to end.** Everything is measured on 2.2 Galore because it
is the default. 1.1 Honey is one pad and would exercise a genuinely different input path.

---

## Standing corrections — do not re-derive these

- `gePlayerSlotIsDrivable` must **not** return `!IS_TWO_PAD`. Every slot is 2.x by default, so
  that disables every bot on every level. (Taken and fixed — noted so it does not come back.)
- `GETV_SCRIPT` does not move the player at all. Verified against controls. Use `gePlayerPost`.
- `player->pos` is not the world position; `prop->pos` is.
- Doors are **not** walls. `CDTYPE_DOORS` in a walkability mask makes every room read as sealed.
- The tile argument to `bondviewTestLineUnobstructed` is an **input**. NULL reports everything
  obstructed.
- Run the control. Never sample a trace with `tail -1`.
