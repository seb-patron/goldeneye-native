#!/usr/bin/env python3
"""Rooms, from the game's own standing-tile geometry.

WHY

The threat data in gen_level_routes.py is proximity only: a guard 200 units away counts the same
whether he is down the corridor or on the far side of a wall. That is the difference between a
bot that takes cover sensibly and one that panics at a guard it cannot see and could not be seen
by.

GoldenEye already carries the structure that answers it. stan -- the standing tiles the engine
walks characters over -- gives every tile a ROOM id, and the tiles carry links to their
neighbours. Same room means a clear line far more often than not; different, unlinked rooms mean
a wall. It is the game's own occlusion structure, sitting in assets/obseg/stan, and nothing has
been reading it.

WHAT THIS PRODUCES

  - room id per waypoint and per prop, by nearest tile
  - a room adjacency graph, built from tile links that cross a room boundary

WHAT IT IS NOT

Not true line of sight. Two points in one large room can still have a pillar between them, and
this will call that visible. It is a much better approximation than distance alone and it is
cheap; a real visibility test would need the tile polygons and ray casting, which is a different
piece of work.

Usage:
    python3 tools/gen_level_rooms.py --out build/levels
"""

import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAN_DIR = os.path.join(ROOT, "vendor", "ge-decomp", "assets", "obseg", "stan")

# A tile as emitted: id, room, mid, then the point count, three header indices, and the points.
TILE = re.compile(
    r"StandTile\s+\w*tile_(\d+)\s*=\s*\{\s*"
    r"(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,"      # id, room
    r"(.*?)\n\};", re.S)
POINT = re.compile(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}")


def parse_stan(path):
    """[{tile, room, centroid, links}] from one stan translation unit."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    tiles = []
    for m in TILE.finditer(text):
        room = int(m.group(3), 16)
        pts = POINT.findall(m.group(4))
        if not pts:
            continue
        xs = [int(p[0]) for p in pts]
        ys = [int(p[1]) for p in pts]
        zs = [int(p[2]) for p in pts]
        tiles.append({
            "tile": int(m.group(1)),
            "room": room,
            "centroid": [sum(xs) / len(xs), sum(ys) / len(ys), sum(zs) / len(zs)],
            "links": [int(p[3], 16) for p in pts if int(p[3], 16) != 0],
        })
    return tiles


def nearest_room(tiles, pos):
    best, bestd = None, None
    for t in tiles:
        c = t["centroid"]
        d = (c[0]-pos[0])**2 + (c[1]-pos[1])**2 + (c[2]-pos[2])**2
        if bestd is None or d < bestd:
            best, bestd = t, d
    return (best["room"], bestd ** 0.5) if best else (None, None)


# Stan files are named after the BACKGROUND, using the same short stems as the setup files
# rather than the level names. Most fall out of the setup stem with "Usetup" removed; these are
# the ones that do not.
#
# surface2 is the interesting entry: it has no stan of its own and uses Surface's. That is not a
# gap, it is the same terrain at night, which the arena nuance already records independently.
STAN_STEM = {
    "facility": "ark",   "bunker1": "sev",    "bunker2": "sevb",
    "statue":   "stat",  "control": "arec",   "streets": "pete",
    "caverns":  "cave",  "depot":   "depo",   "frigate": "dest",
    "surface":  "sevx",  "surface2": "sevx",  "archives": "arch",
    "train":    "tra",   "egypt":   "cryp",   "cradle":  "crad",
    "jungle":   "jun",   "aztec":   "azt",    "runway":  "run",
    "silo":     "silo",  "dam":     "dam",
    # Multiplayer-only arenas, by their own stems.
    "library":  "ame",   "stack":   "ash",    "temple":  "dish",
    "basement": "imp",   "caves":   "oat",    "complex": "ref",
}


def find_stan(level):
    stem = STAN_STEM.get(level, level)
    hits = glob.glob(os.path.join(STAN_DIR, "Tbg_%s_all_p_stanZ.c" % stem))
    return hits[0] if hits else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join("build", "levels"))
    ap.add_argument("--level", action="append", default=[])
    args = ap.parse_args()
    out_dir = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)

    levels = args.level or [os.path.basename(p)[:-len(".json")]
                            for p in sorted(glob.glob(os.path.join(out_dir, "*.json")))
                            if not any(p.endswith(s) for s in
                                       (".tactics.json", ".routes.json", ".nuance.json",
                                        ".rooms.json", ".mp.json", ".mp-nuance.json",
                                        "index.json"))]

    done = missing = 0
    for level in levels:
        kp = os.path.join(out_dir, level + ".json")
        sp = find_stan(level)
        if not os.path.exists(kp):
            continue
        if sp is None:
            print("%-10s no stan file found" % level)
            missing += 1
            continue

        with open(kp, encoding="utf-8") as fh:
            know = json.load(fh)
        tiles = parse_stan(sp)
        if not tiles:
            print("%-10s stan parsed to nothing" % level)
            missing += 1
            continue

        # Room adjacency: a tile link that lands in a different room is a way between them.
        by_tile = {t["tile"]: t for t in tiles}
        adj = {}
        for t in tiles:
            for l in t["links"]:
                o = by_tile.get(l)
                if o is None or o["room"] == t["room"]:
                    continue
                adj.setdefault(t["room"], set()).add(o["room"])
                adj.setdefault(o["room"], set()).add(t["room"])

        wp_rooms = {}
        for w in know.get("waypoints", []):
            if not w.get("pos"):
                continue
            room, dist = nearest_room(tiles, w["pos"])
            wp_rooms[str(w["index"])] = room

        prop_rooms = {}
        for p in know.get("props", []):
            if not p.get("pos"):
                continue
            room, _d = nearest_room(tiles, p["pos"])
            prop_rooms[str(p["propdef"])] = room

        rooms = sorted({t["room"] for t in tiles})
        doc = {
            "level": level,
            "stan": os.path.basename(sp),
            "counts": {"tiles": len(tiles), "rooms": len(rooms),
                       "room_edges": sum(len(v) for v in adj.values()) // 2,
                       "waypoints_placed": len(wp_rooms), "props_placed": len(prop_rooms)},
            "room_graph": {str(k): sorted(v) for k, v in sorted(adj.items())},
            "waypoint_room": wp_rooms,
            "prop_room": prop_rooms,
        }
        with open(os.path.join(out_dir, level + ".rooms.json"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1)
        print("%-10s tiles=%-5d rooms=%-4d edges=%-4d waypoints=%-4d props=%d"
              % (level, len(tiles), len(rooms), doc["counts"]["room_edges"],
                 len(wp_rooms), len(prop_rooms)))
        done += 1

    print("\n%d levels with room data, %d without" % (done, missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
