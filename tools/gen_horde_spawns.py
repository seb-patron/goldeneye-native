#!/usr/bin/env python3
"""Where can a horde wave legitimately arrive from?

The roadmap notes that the cheat system already arms guards and the graph knows where they can come
from, so this is most of a wave spawner. The part that is a graph question -- and therefore ours --
is SELECTING the arrival points. Getting it wrong is very visible: enemies materialising in front
of the player, or behind a wall they can never get out from behind.

FOUR REQUIREMENTS, and every one is measurable from data we already extract:

  STANDABLE   a floor tile, which is standable by construction. Pads are not: 139 of Train's 180
              cannot be stood on, which is why S2 moved the graph onto tiles.
  REACHABLE   connected to the player through the tile graph. A spawn in a sealed pocket produces
              an enemy that never arrives, and the player never learns why.
  OUT OF SIGHT the player must not watch it happen. Checked by ray-casting the derived wall set
              between the candidate and the player, not by distance -- a corridor gives line of
              sight at 3,000 units while a corner breaks it at 200.
  IN BAND     far enough not to be startling, near enough to matter. Expressed in units of the
              level's own measured sightline, not as a constant: 800 units is across the room on
              Bunker and barely down the carriage on Train.

OUT OF SIGHT IS THE ONE THAT NEEDS REAL GEOMETRY, and the reason this waited for the wall
ray-caster. The obvious proxy -- "far away" or "different room" -- fails in both directions on the
levels we measured: Train's median sightline is 4,000 units, so a distant spawn is still watched;
Bunker's is 262, so an adjacent room is already hidden.

AND THE PLAYER REFERENCE IS THE MEASURED SPAWN, in asset space. docs/captures/spawns.json is
runtime; asset = runtime * levelscale. Train's spawn reads x=779 where the tile map ends at 213,
so using it unconverted puts the reference outside the level and every candidate "out of sight".
"""
import argparse
import collections
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

# Below this many hidden, reachable arrival points a level cannot stage waves without enemies
# appearing in front of the player or stacking on one tile. gen_horde_waves.py imports this rather
# than keeping its own copy, so the selector and the scheduler cannot drift apart on what counts as
# too few -- a scheduler that accepts three points the selector considers unusable would build a
# schedule around arrivals that should never have been offered.
MIN_SPAWNS = 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    ap.add_argument("--level", default="train")
    ap.add_argument("--want", type=int, default=0,
                    help="spawn points to select; 0 (default) derives it from the level")
    ap.add_argument("--near", type=float, default=0.15,
                    help="minimum distance, as a fraction of the level's long axis")
    ap.add_argument("--far", type=float, default=0.75, help="maximum, same units")
    a = ap.parse_args()

    from gen_level_visibility import load_walls, Grid, ray_hit
    from pack_world import load_level_scales

    lv = a.level
    rooms_p = os.path.join(a.levels, "%s.rooms.json" % lv)
    walls_p = os.path.join(a.levels, "%s.walls.json" % lv)
    if not (os.path.isfile(rooms_p) and os.path.isfile(walls_p)):
        sys.exit("need %s.rooms.json and %s.walls.json" % (lv, lv))

    floors = json.load(open(rooms_p, encoding="utf-8")).get("floors") or []
    walls = load_walls(walls_p)
    scale = load_level_scales(ROOT).get(lv)
    caps = json.load(open(os.path.join(ROOT, "docs", "captures", "spawns.json"),
                          encoding="utf-8")).get("spawns", {})
    if lv not in caps or not scale:
        sys.exit("no measured spawn or levelscale for %s" % lv)

    # asset = runtime * levelscale. See the module note: unconverted, the reference lands outside
    # the level and every candidate trivially passes the sight test.
    px, py, pz = [c * scale for c in caps[lv]["pos"]]

    pos = {f["t"]: f["c"] for f in floors}
    adj = {f["t"]: [n for n in f.get("l", []) if n in pos] for f in floors}
    room = {f["t"]: f.get("r") for f in floors}

    # snap the player into the level'S playable body, not merely to the nearest tile.
    #
    # These tile graphs are fragmented -- frigate has 178 connected components, facility 156 -- and
    # a nearest-in-XZ snap lands on whatever scrap of floor happens to be closest. On frigate that
    # was a detached TWO-TILE ISLAND 7 units away, so the reachability search returned 2 tiles of
    # 1,240, the level reported zero arrival points, and the scheduler refused it as "unsuitable".
    # The main body it should have snapped to (768 tiles) sat 39 units away and lost on distance.
    #
    # A height band does not fix this, recorded because it was tried first and did nothing. The
    # island sits at a body-to-floor offset of 75 and the correct tiles at 25; BOTH are inside the
    # measured 12-202 offset range, so any band admitting real spawns admits the island too, and the
    # island still wins on horizontal distance.
    #
    # The constraint that actually holds is semantic, not metric: A player cannot spawn on A two-TILE
    # ISLAND. The playable body of a level is its largest connected component, so the snap is
    # restricted to that component and the fragments drop out by construction. This also removes the
    # luck from the levels that already worked -- facility passed only because its spawn happened to
    # land in the big component, not because anything made sure it did.
    comp_id, comps = {}, []
    for t0 in pos:
        if t0 in comp_id:
            continue
        c, st = [], [t0]
        comp_id[t0] = len(comps)
        while st:
            u = st.pop()
            c.append(u)
            for v in adj.get(u, ()):
                if v not in comp_id:
                    comp_id[v] = len(comps)
                    st.append(v)
        comps.append(c)
    main = max(comps, key=len)
    start = min(main, key=lambda t: (pos[t][0] - px) ** 2 + (pos[t][2] - pz) ** 2)

    # cast at floor height, not body height. The wall set carries the floor y of the tile that
    # produced each wall; the captured spawn is a BODY position, standing a body-to-floor offset
    # above its floor. Comparing one against the other with a deck-separation band silently rejects
    # every wall on the player's OWN deck as soon as that offset exceeds the band.
    #
    # Facility is how this was found: player body y=474, every wall and floor in the level between
    # -577 and +321, so the ray was cast 891 units above the level's own median and matched nothing.
    # The spawn is not wrong -- facility's top floor is 321 and 474-321=153, inside the 12-202
    # offset range measured elsewhere on this project. It simply exceeded a band of 150 by THREE
    # UNITS, and the level reported zero arrival points, which read as "unsuitable for horde" for a
    # level with 2,219 floor tiles.
    #
    # The player's own tile carries a floor y, which is the same KIND of number the walls carry, so
    # the two are directly comparable and the band goes back to meaning what it says: which deck.
    #
    # Kept in its own name rather than assigned over py: py is the captured BODY position and is
    # recorded as player_ref in the output, so overwriting it would quietly change what that field
    # means for anyone reading the file later.
    ray_y = pos[start][1]

    # Reachability is the component itself: every tile in it reaches every other by definition, so a
    # second traversal would recompute what the component search already established.
    seen = set(main)

    xs = [c[0] for c in pos.values()]
    zs = [c[2] for c in pos.values()]
    span = max(max(xs) - min(xs), max(zs) - min(zs))
    lo, hi = a.near * span, a.far * span

    grid = Grid(walls)
    cand = []
    for t, c in pos.items():
        if t not in seen:
            continue                     # unreachable: the enemy would never arrive
        d = math.dist((c[0], c[2]), (px, pz))
        if not (lo <= d <= hi):
            continue
        # Line of sight along the real geometry: does a ray from the player reach the candidate,
        # or does a wall stop it first? Stopped short = hidden = a legitimate arrival point.
        dx, dz = (c[0] - px) / d, (c[2] - pz) / d
        hit = ray_hit(grid, px, pz, dx, dz, d, ray_y, 150.0)
        if hit >= d - 1.0:
            continue                     # visible: the player would watch it appear
        cand.append({"tile": t, "pos": [round(v, 1) for v in c], "room": room.get(t),
                     "dist": round(d, 1), "blocked_at": round(hit, 1)})

    # how many points A level gets must come from the level, or it is not grounded in anything.
    #
    # A fixed --want 12 made every one of the 20 playable levels return exactly 12 points and
    # therefore exactly 140 enemies over 20 waves -- bunker1, whose median sightline measures 262
    # units, got the identical horde to statue. That is the archetype-audit failure again: a measure
    # returning the same verdict for levels that are visibly different is not measuring them.
    #
    # rooms that can host an arrival is the discriminator, and it genuinely discriminates: runway
    # has 7, control has 73, a tenfold spread. Tile count does not work as well -- it counts floor
    # area, and a large open hall is one place to come from, not fifty.
    #
    # The ceiling is measured, not chosen: real levels ship 5 to 79 Guard records, median 36, so a
    # simultaneous wave of 16 stays under half the guard load the engine already runs on a typical
    # level. The floor is MIN_SPAWNS, below which the schedule refuses outright.
    want = a.want
    if want <= 0:
        n_rooms = len({c["room"] for c in cand})
        want = max(MIN_SPAWNS, min(16, int(round(n_rooms * 0.3))))

    # spread across the band, not nearest-FIRST. Sorting by distance and taking the first N
    # picks every arrival from the near edge: on Train that gave twelve spawns between 807 and 962
    # units of an 806-4031 band, all in one stretch of carriages. Two per room did not fix it,
    # because Train's rooms are the carriages and consecutive ones are adjacent -- a per-room cap
    # spreads across labels, not across space.
    #
    # So the sorted candidates are sampled at an even stride, which covers the whole band by
    # construction, and the per-room cap still prevents doubling up inside one space. Deterministic
    # rather than random: the same level yields the same spawn set every run, which matters when
    # someone is trying to reproduce a wave.
    cand.sort(key=lambda x: x["dist"])
    per_room, chosen = collections.Counter(), []
    if cand:
        stride = max(1, len(cand) // max(1, want))
        for i in range(0, len(cand), stride):
            c = cand[i]
            if per_room[c["room"]] >= 2:
                continue
            per_room[c["room"]] += 1
            chosen.append(c)
            if len(chosen) >= want:
                break
        # The stride can under-deliver when the room cap rejects many in a row; top up from
        # whatever is left rather than silently returning fewer than asked for.
        if len(chosen) < want:
            taken = {c["tile"] for c in chosen}
            for c in cand:
                if c["tile"] in taken or per_room[c["room"]] >= 2:
                    continue
                per_room[c["room"]] += 1
                chosen.append(c)
                if len(chosen) >= want:
                    break

    print("%s: %d floor tiles, %d reachable, span %.0f (asset)" % (lv, len(pos), len(seen), span))
    print("player spawn (asset): %.0f, %.0f    band %.0f..%.0f" % (px, pz, lo, hi))
    print("candidates hidden AND reachable AND in band: %d, across %d rooms"
          % (len(cand), len({c["room"] for c in cand})))
    print("\nselected %d spawn points (max 2 per room):" % len(chosen))
    for c in chosen:
        print("   tile %-5d room %-4s %6.0f away, sight blocked at %6.0f  (%.0f, %.0f)"
              % (c["tile"], c["room"], c["dist"], c["blocked_at"], c["pos"][0], c["pos"][2]))

    dest = os.path.join(a.levels, "%s.horde.json" % lv)
    with open(dest, "w", encoding="utf-8") as fh:
        json.dump({"level": lv, "space": "asset", "levelscale": scale,
                   "player_ref": [round(px, 1), round(py, 1), round(pz, 1)],
                   "band": [round(lo, 1), round(hi, 1)],
                   "candidates": len(cand), "spawns": chosen}, fh, indent=1)
    print("\n-> %s" % dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
