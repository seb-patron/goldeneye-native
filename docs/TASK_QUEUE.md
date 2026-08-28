# Task queue: Windows tree

Ordered. Each item says what "done" means, because a task without a finish condition gets
reported as finished by whoever is tired.

This list crosses two machines before it reaches `main` -- see `docs/DEVELOPMENT.md` for how
that integration works.

---

## 0. The accessor link failure - resolved, nothing to do

Kept as a numbered entry rather than deleted, because the instruction it used to carry is
still repeated from memory and it now names a file that does not exist.

`0003-port-accessors.patch` is gone. The player accessors, the control-style helpers, both
navigation probes, `gePortTeleportProbe` and the two sensing primitives were folded into
`0001-source.patch` on a later refresh, and applying 0003 on top of 0001 fails with `patch does
not apply` because every line it adds is already there. `getv/patches/README.md` has the full
account of why 0003 to 0005 are gone and how it was verified before removing them.

`tools/install.ps1` applies every patch in `getv/patches` in numeric order, so there is no
manual `git apply` step on Windows any more.

The underlying point still stands and is worth keeping: `vendor/` is gitignored, so decomp
symbols never travel in a bundle. If you add a symbol the port layer calls, it belongs in a
patch the same day, or the other machine gets a tree that compiles and will not link.

---

## 1. THE SENSING API IS THE PRIORITY: build out what a bot can perceive

This is the half of the platform that was missing, and it matters more than routing. Waypoints
say where things are. **Interaction** is knowing you are against a wall rather than a crate,
whether the thing ahead can be opened or must be shot, and whether anyone can see you.

`ge_sense_api.[ch]` is the seam and the first primitives are in and measured:

| | |
|---|---|
| `geSenseLine` | what blocks a line, as a bitmask: WALL / DOOR / OBJECT / BODY |
| `geSenseAhead` | walks a ray in samples: what, how far, and the last clear point |
| `geSenseClearestHeading` | the smallest turn that opens up |
| `geSenseVisibleTo` | does this character have a clear line to this player |
| `geSenseWatchers` | how many living enemies do |

Live on Train: `clear at 300u`, then `WALL DOOR OBJECT BODY at 300u`, then `OBJECT BODY at 50u,
clearest +40 deg`.

**What is missing, and it is yours:**

**1a. Facing, so sight means attention.** `geSenseVisibleTo` is line of sight only. A guard facing
away has a clear line and is not looking. GoldenEye's AI uses a facing cone plus distance
attenuation and `hearingscale` for sound. Add `geSenseNoticedBy(enemy, player)` that combines
line, cone and alertness, and keep the two separate rather than replacing one with the other,
because "could see me if it turned" is a different and useful question.

Train currently reports 17-19 watchers of 40 guards. That is what an unobstructed line down a
row of carriages looks like, **not** seventeen guards watching. Do not tune the line test to make
that number smaller; add the cone.

**1b. Contact, not just prediction.** Everything so far predicts along a ray. A bot also needs to
know it is *touching* something right now: the difference between "there is a wall ahead" and
"I am pressed against it and my last four moves did nothing".

**1c. Reachability with a body, not a line. THIS IS THE CURRENT BLOCKER, and do not build it
the way this item originally said.** A line test passes through a gap narrower than the player.
Evan has a capture of exactly that: the bot trying to fit between a crate and a wall.

The original suggestion here was to sweep the player's collision radius or sample parallel lines
either side. **Don't.** The engine already ships the exact test, and the guard AI runs it before
every step it takes:

```c
s32 stanTestVolume(StandTile **tile, f32 x, f32 z, f32 width,
                   s32 cdtypes, f32 ymin, f32 ymax);          /* stan.c:2073 */
s32 stanTestLineUnobstructed(StandTile **tile, f32 x0, f32 z0, f32 x1, f32 z1,
                             s32 cdtypes, f32 height, f32 a, f32 b, f32 c);  /* stan.c:1686 */
```

`chr.c:1468` calls the line test, then the volume test at the destination, and treats a
**negative** `stanTestVolume` return as "a body of `width` fits here". Non-negative is the index
of what is in the way. The width is `chrwidth`, `20.0f` at `chr.c:1936`, and `chraction.c:4119`
shows the engine's own margin at `chrwidth * 1.2f`. Use the mask `chraction.c:3448` uses:
`CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PLAYERS | CDTYPE_CHRS | CDTYPE_PATHBLOCKER`.

The tile argument is an **input**, same trap as `bondviewTestLineUnobstructed` in the standing
corrections. Seed it from the querying body's current tile or everything reads obstructed.

**Do:** `gePortCanStandAt(x, z)` and `gePortPathClear(x0, z0, x1, z1)` over those two. Parallel
line sampling would have approximated this and missed doors, characters and path blockers, all of
which `stanTestVolume` already accounts for.

**Done when:** `gePortCanStandAt` returns false for the point between the crate and the wall in
Evan's capture, and the Train CLI player walks past the crate it currently traps itself on.

**1d. Interaction verbs.** `geSenseUsable(x, z)`: is there a door, switch or pickup within reach
of this spot, and what is it. The prop API knows where they are; nothing says "you can act on
this from here".

**Done when:** `mods/level_atlas` can print, for the player's current position: what is ahead and
how far, whether it can be opened, which way is clear, how many enemies could see it and how many
actually are, and what it could interact with without moving.

---

## 2. Heights: the graph is planar and levels are not

Every node carries `y` and nothing routes on it. Bunker 1's spawn is at y=340 and both of its
portals are at y=93, so the bot beelines at a doorway 247 units below it, through a floor. Your
`audit_route_heights.py` is the right start; what is missing is the descent being *in* the graph,
so a stairway is a chain of nodes rather than one impossible edge.

The body position and the floor differ by ~157 units. Compare a pad height to a player
position and you get a phantom cliff in every direction at once.

## 3. Eight levels have no node in their spawn room

`aztec, cradle, dam, depot, runway, statue, surface, surface2`: no graph node within 4000 units
of where the player actually starts, and `dam` is 20,254 away. That is not a wrong pad, it is a
graph that does not cover the level's own start, and for Dam a different coordinate space
entirely. `docs/captures/spawns.json` has the measured truth for all twenty.

Do **not** fix this by widening the threshold. The refusal is deliberate and named per level.

## 4. Bot behaviour beyond following a line

The follower turns, walks, presses the action button at obstacles and holds for contested
waypoints. It cannot open a locked door, take a lift, shoot what blocks it, or pick up a key it
routes past. The objective data already names target tags and rooms; `Key` props are in the prop
export.

---

## Later, in no strict order

**Netplay on a measured tick.** Discovery and the transports are in. What is not proven is that
two peers stay identical over a long run: the seed fingerprint exists for exactly this and has
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

## Standing corrections: do not re-derive these

- `gePlayerSlotIsDrivable` must **not** return `!IS_TWO_PAD`. Every slot is 2.x by default, so
  that disables every bot on every level. (Taken and fixed; noted so it does not come back.)
- `GETV_SCRIPT` does not move the player at all. Verified against controls. Use `gePlayerPost`.
- `player->pos` is not the world position; `prop->pos` is.
- Doors are **not** walls. `CDTYPE_DOORS` in a walkability mask makes every room read as sealed.
- The tile argument to `bondviewTestLineUnobstructed` is an **input**. NULL reports everything
  obstructed.
- Run the control. Never sample a trace with `tail -1`.
