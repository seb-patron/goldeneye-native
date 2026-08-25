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
        # Floor or wall, from the surface normal.
        #
        # This is the distinction that makes room assignment possible at all. A stan tile is not
        # a room-sized floor plate -- it is a triangle or quad with a median edge span of about
        # 45 units, and a large share of them are VERTICAL faces. Dam's first tile has three
        # vertices sharing one x,z and differing only in height: a wall. Its XZ footprint is a
        # line, so any containment test against it answers arbitrarily, which is precisely why
        # the previous attempt reported every waypoint as contained.
        #
        # Newell's method rather than a single cross product: it is stable on quads that are not
        # quite planar and on near-degenerate triangles, both of which are common here.
        nx = ny = nz = 0.0
        n = len(pts)
        for i in range(n):
            x1, y1, z1 = int(pts[i][0]), int(pts[i][1]), int(pts[i][2])
            x2, y2, z2 = int(pts[(i+1) % n][0]), int(pts[(i+1) % n][1]), int(pts[(i+1) % n][2])
            nx += (y1 - y2) * (z1 + z2)
            ny += (z1 - z2) * (x1 + x2)
            nz += (x1 - x2) * (y1 + y2)
        mag = (nx*nx + ny*ny + nz*nz) ** 0.5
        # |ny| dominant means the face points up or down: something a character stands on.
        # 0.5 is roughly 60 degrees from horizontal, which keeps ramps and stairs as floor.
        is_floor = bool(mag > 0.0 and abs(ny) / mag > 0.5)

        tiles.append({
            "tile": int(m.group(1)),
            "room": room,
            "is_floor": is_floor,
            "centroid": [sum(xs) / len(xs), sum(ys) / len(ys), sum(zs) / len(zs)],
            # The polygon itself, which nearest-centroid threw away. A point standing inside a
            # tile is in that tile's room no matter how the centroids happen to fall, and two
            # things standing near each other resolving to different rooms was exactly the bug
            # centroid distance produced.
            "poly": [(int(p[0]), int(p[2])) for p in pts],     # XZ footprint
            "poly3": [(int(p[0]), int(p[1]), int(p[2])) for p in pts],
            "ylo": min(ys), "yhi": max(ys),
            "links": [int(p[3], 16) for p in pts if int(p[3], 16) != 0],
        })
    return tiles


def point_in_poly(x, z, poly):
    """Standard crossing test on the tile's XZ footprint."""
    inside = False
    n = len(poly)
    for i in range(n):
        x1, z1 = poly[i]
        x2, z2 = poly[(i + 1) % n]
        if (z1 > z) != (z2 > z):
            xint = (x2 - x1) * (z - z1) / float(z2 - z1) + x1
            if x < xint:
                inside = not inside
    return inside


BG_DIR = os.path.join(ROOT, "vendor", "ge-decomp", "assets", "obseg", "bg")

# {&portal_N, roomA, roomB, flags}
PORTAL = re.compile(r"\{\s*&portal_(\d+)\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*,"
                    r"\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*\}")

# struct portal_N_point portal_M = { count, 0,0,0, x,y,z, x,y,z, ... };
PORTAL_GEOM = re.compile(
    r"struct\s+portal_\d+_point\s+portal_(\d+)\s*=\s*\{([^}]*)\}")


def parse_portal_geometry(text):
    """{portal index: [(x,y,z), ...]} -- the actual opening each portal describes.

    These are real openings, not stan's ledges: Dam's portal_0 spans nearly three thousand units
    vertically. That is the difference between geometry that can block a sightline and geometry
    that cannot, and it is why visibility has to come from here.
    """
    out = {}
    for m in PORTAL_GEOM.finditer(text):
        nums = [float(t) for t in re.findall(r"-?\d+(?:\.\d+)?", m.group(2))]
        if len(nums) < 4:
            continue
        count = int(nums[0])
        coords = nums[4:4 + count * 3]          # skip count and three pad words
        if len(coords) < count * 3 or count < 3:
            continue
        out[int(m.group(1))] = [(coords[i * 3], coords[i * 3 + 1], coords[i * 3 + 2])
                                for i in range(count)]
    return out


def parse_portals(level, stem):
    """Room adjacency from the background model's portal table -- the game's own answer.

    Every previous version of this inferred adjacency: first from stan tile links crossing a
    room boundary, then from the navigation graph. Both were guesses at something the data
    states outright. GoldenEye's renderer is portal-based, and assets/obseg/bg carries a
    portal_data_table whose every entry names the two rooms that portal joins.

    That is the authoritative adjacency, and it is what decides whether two rooms can see into
    each other -- which is exactly the question the threat exposure is asking.
    """
    for name in ("bg_%s_all_p.c" % stem, "bg_%s_all_p.c" % level):
        path = os.path.join(BG_DIR, name)
        if os.path.exists(path):
            break
    else:
        return None, None, None
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = re.search(r"portal_data_table\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return None, os.path.basename(path), None
    geom = parse_portal_geometry(text)
    adj, openings = {}, []
    for pm in PORTAL.finditer(m.group(1)):
        idx = int(pm.group(1))
        a, b = int(pm.group(2), 0), int(pm.group(3), 0)
        if a == b:
            continue
        adj.setdefault(a, set()).add(b)
        adj.setdefault(b, set()).add(a)
        poly = geom.get(idx)
        if poly:
            openings.append({"portal": idx, "rooms": [a, b], "poly": poly})
    return adj, os.path.basename(path), openings


def nearest_room(tiles, pos):
    """Room for a world position: the tile standing under it, or the nearest tile if none.

    Containment first, height second. Levels are multi-storey, so several tiles can cover the
    same XZ footprint and only the vertical distance separates the catwalk from the floor
    beneath it. Falling back to nearest centroid keeps a point outside every tile -- a prop
    embedded in scenery, say -- from having no room at all.
    """
    # Containment against FLOOR tiles only. Testing every tile was the mistake first time round:
    # walls have a collapsed XZ footprint, so a crossing test against one answers arbitrarily and
    # reported all 205 of Dam's waypoints as contained, one of them 200 units from its match.
    #
    # Floor filtering makes room assignment measurably more coherent: adjacent waypoints share a
    # room 386 times against 144 crossings, up from 378/152 with nearest-centroid alone. 80% of
    # Dam's 2755 tiles are floor; the other 20% are walls that were previously being matched
    # against.
    x, y, z = pos[0], pos[1], pos[2]
    best, bestdy = None, None
    for t in tiles:
        if not t["is_floor"]:
            continue
        if not point_in_poly(x, z, t["poly"]):
            continue
        # Several floors can share an XZ footprint in a multi-storey level, and only height
        # separates the catwalk from the floor under it.
        dy = abs(y - (t["ylo"] + t["yhi"]) * 0.5)
        if bestdy is None or dy < bestdy:
            best, bestdy = t, dy
    if best is not None:
        return best["room"], bestdy

    # Nothing underfoot: a prop embedded in scenery, or a point just off the walkable mesh.
    # Nearest floor centroid rather than no room at all.
    best, bestd = None, None
    floors = [t for t in tiles if t["is_floor"]] or tiles
    for t in floors:
        c = t["centroid"]
        d = (c[0]-x)**2 + (c[1]-y)**2 + (c[2]-z)**2
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

        # Portal adjacency from the background model, if it can be found. Reported alongside the
        # tile-derived graph rather than silently replacing it: the two are built from different
        # data and disagreeing would mean one of them is wrong.
        portals, bgfile, openings = parse_portals(level, STAN_STEM.get(level, level))
        stan_rooms = {t["room"] for t in tiles}
        overlap = None
        if portals:
            prooms = set(portals)
            overlap = len(stan_rooms & prooms)

        rooms = sorted({t["room"] for t in tiles})
        doc = {
            "level": level,
            "stan": os.path.basename(sp),
            "counts": {"tiles": len(tiles), "rooms": len(rooms),
                       "room_edges": sum(len(v) for v in adj.values()) // 2,
                       "waypoints_placed": len(wp_rooms), "props_placed": len(prop_rooms)},
            "bg": bgfile,
            # The authoritative adjacency when the portal table is available; the tile-derived
            # one is kept beside it so the two can be compared rather than conflated.
            "portal_graph": ({str(k): sorted(v) for k, v in sorted(portals.items())}
                             if portals else None),
            # The openings themselves, so a caller can test whether a sightline actually passes
            # THROUGH the doorway rather than merely that two rooms are joined by one.
            "portals": openings,
            "room_graph": {str(k): sorted(v) for k, v in sorted(adj.items())},
            # The walls, as 3D polygons, so a caller can cast a ray and get real line of sight
            # rather than the room approximation. Kept here because classifying them is the
            # expensive part and it has already been done; testing a segment against them is
            # cheap by comparison.
            "walls": [{"poly": t["poly3"],
                       "bbox": [min(p[0] for p in t["poly3"]), min(p[1] for p in t["poly3"]),
                                min(p[2] for p in t["poly3"]), max(p[0] for p in t["poly3"]),
                                max(p[1] for p in t["poly3"]), max(p[2] for p in t["poly3"])]}
                      for t in tiles if not t["is_floor"]],
            # THE FLOOR TILES, WITH THEIR HEIGHTS. These were classified here and then thrown
            # away, which is how a navigation mesh derived from stan reached the router as a flat
            # drawing: every node carried a y and nothing could route on it.
            #
            # It shows up as a bot walking at a doorway it cannot reach. Bunker 1's spawn is at
            # y=340 and both portals out of its room are at y=93, so a planar graph joins them
            # with a straight line through 247 units of floor and the follower beelines into it.
            # Stairs and ramps are already kept as floor by the 0.5 normal threshold above, so the
            # descent is present in this data -- it simply was not emitted.
            #
            # Compact on purpose: 36,458 tiles across twenty levels, and a router needs where a
            # tile is, how high it is, and which room it belongs to. The full polygon is what
            # `walls` needs for ray casting; a floor is only ever asked "can a body stand here,
            # and at what height".
            #
            #   r  room id
            #   c  centroid, [x, y, z]
            #   bb XZ footprint, [minx, minz, maxx, maxz] -- adjacency and containment in plan
            #      view, which is what a walking body meets
            #
            # NOTE FOR CONSUMERS: this y is the FLOOR, not a body position. On Bunker 1 they
            # differ by 157 units (pos.y=329 against a floor at 172), so comparing a player
            # position to one of these directly reports a phantom cliff in every direction at
            # once. Measured by mac-getv, who lost a test cycle to it.
            "floors": [{"r": t["room"],
                        "c": [sum(p[0] for p in t["poly3"]) // len(t["poly3"]),
                              sum(p[1] for p in t["poly3"]) // len(t["poly3"]),
                              sum(p[2] for p in t["poly3"]) // len(t["poly3"])],
                        "bb": [min(p[0] for p in t["poly3"]), min(p[2] for p in t["poly3"]),
                               max(p[0] for p in t["poly3"]), max(p[2] for p in t["poly3"])]}
                       for t in tiles if t["is_floor"]],
            "waypoint_room": wp_rooms,
            "prop_room": prop_rooms,
        }
        with open(os.path.join(out_dir, level + ".rooms.json"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1)
        pe = sum(len(v) for v in portals.values()) // 2 if portals else 0
        print("%-10s tiles=%-5d rooms=%-4d tile_edges=%-4d portal_rooms=%-4s portal_edges=%-4s "
              "shared=%-4s waypoints=%-4d props=%d"
              % (level, len(tiles), len(rooms), doc["counts"]["room_edges"],
                 len(portals) if portals else "-", pe if portals else "-",
                 overlap if overlap is not None else "-",
                 len(wp_rooms), len(prop_rooms)))
        done += 1

    print("\n%d levels with room data, %d without" % (done, missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
