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


def floor_under(tiles, pos):
    """Height of the floor a body at `pos` would stand on, or None if nothing is beneath it.

    WHY THIS IS EMITTED AT ALL: the runtime edge validator has to seed each line-of-sight
    test from a stan tile, and the engine's own lookup SNAPS to the nearest standable tile -- which
    can be on the far side of a wall. Seeded three different ways, the same 2926 Bunker 1 pairs
    came out 98%, 73% and 0% walkable. Those are not three estimates; they are one measurement and
    two artefacts.

    A node's own floor height is a cheap check on that. If the tile the engine snapped to sits at a
    materially different height from the floor directly under the node, the snap has left the
    node's surface -- detectable per edge, offline, before spending a run on it. It cannot prove an
    edge walkable; it says which seeds to distrust.

    HIGHEST CONTAINED TILE AT OR BELOW THE POINT. Not the lowest, and the distinction is the whole
    function. Taking the lowest reports a pad on cradle's antenna platform as standing 2078 units
    above its floor, because the tile under it in plan view is the ground far below. Real
    containment, wrong question. See tools/audit_route_heights.py, where that cost two corrections.
    """
    x, y, z = pos[0], pos[1], pos[2]
    best = None
    for t in tiles:
        if not t["is_floor"]:
            continue
        p3 = t["poly3"]
        xs = [p[0] for p in p3]
        zs = [p[2] for p in p3]
        if x < min(xs) or x > max(xs) or z < min(zs) or z > max(zs):
            continue
        ty = sum(p[1] for p in p3) // len(p3)
        if ty <= y and (best is None or ty > best):
            best = ty
    return best


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
        wp_floor = {}
        for w in know.get("waypoints", []):
            if not w.get("pos"):
                continue
            room, dist = nearest_room(tiles, w["pos"])
            wp_rooms[str(w["index"])] = room
            fy = floor_under(tiles, w["pos"])
            if fy is not None:
                wp_floor[str(w["index"])] = fy

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

        # ---------------------------------------------------------------- the navigation mesh
        #
        # Two floor tiles are neighbours if they SHARE AN EDGE. Derived from the geometry rather
        # than read from the format, and that is deliberate.
        #
        # The obvious route was the per-point field parse_stan calls "links". It is not a tile
        # link: a tile id is 0x1de322 and that field holds 0x0014 or 0x0000, and StandTilePoint's
        # own comment says the bit work is done by hand. Emitting it as adjacency produced 651
        # disconnected components across 866 Bunker 1 tiles, largest 17 -- a shattered graph that
        # would have looked like a navmesh in the JSON.
        #
        # A shared edge needs no format knowledge to be right. Two triangles that share two
        # vertices abut, and a body can cross between them. Matched on exact integer coordinates,
        # which is safe here because the tiles come from one table and share their vertices rather
        # than each rounding its own.
        edge_owner = {}
        for t in tiles:
            if not t["is_floor"]:
                continue
            p3 = t["poly3"]
            for i in range(len(p3)):
                a, b = p3[i], p3[(i + 1) % len(p3)]
                key = (a, b) if a <= b else (b, a)   # undirected
                edge_owner.setdefault(key, []).append(t["tile"])

        nav = {}
        nav_portal = 0      # edges added across doorways rather than by shared geometry
        for owners in edge_owner.values():
            # An edge with one owner is a boundary; with two it is a doorway between tiles. More
            # than two means coincident geometry, which is joined rather than dropped -- a body
            # can still cross it and refusing would carve holes in the mesh.
            for x in owners:
                for y in owners:
                    if x != y:
                        nav.setdefault(x, set()).add(y)

        # ---------------------------------------------------------------- steps and stairways
        #
        # Shared vertices join tiles that were authored as one surface. They do NOT join a stair
        # tread to the landing above it, or two floors of the same room: those abut in plan view
        # and are separated in height, sharing no vertex at all.
        #
        # That is why Bunker 1's room 30 came out as SIX components despite being one room -- it
        # spans y 197..483, and its storeys never touch in the vertex table. The single portal out
        # of it then attached to a 4-tile fragment rather than the 200-tile body, so the level's
        # main mass reached room 30 through a dead end. Chasing that is what found this.
        #
        # So: two floor tiles are also neighbours if their FOOTPRINTS overlap in plan and their
        # heights differ by no more than a body can step. That is what climbing a stair is.
        #
        # GE_STEP is deliberately smaller than the follower's 90-unit drop limit. This edge means
        # "a body can walk between these", and a 90-unit drop is survivable rather than walkable;
        # putting it in the mesh would route bots off ledges.
        GE_STEP = 40

        # Grid-bucketed, because the naive comparison is 2555 squared on surface and 26 levels of
        # that is not a run anyone waits for. Tiles are small relative to the cell, so a tile need
        # only be checked against its own cell and the eight around it.
        CELL = 200
        buckets = {}
        floor_tiles = [t for t in tiles if t["is_floor"]]
        for t in floor_tiles:
            p3 = t["poly3"]
            t["_bb"] = (min(p[0] for p in p3), min(p[2] for p in p3),
                        max(p[0] for p in p3), max(p[2] for p in p3))
            t["_y"] = sum(p[1] for p in p3) // len(p3)
            for gx in range(t["_bb"][0] // CELL, t["_bb"][2] // CELL + 1):
                for gz in range(t["_bb"][1] // CELL, t["_bb"][3] // CELL + 1):
                    buckets.setdefault((gx, gz), []).append(t)

        # WALL SEGMENTS, so a step edge can be refused if it crosses one.
        #
        # This is not optional. Without it, step adjacency took mean coverage from 73% to 97% and
        # Bunker 1 to a single component -- and 19.4% of Bunker 1's edges crossed a wall in plan
        # view, 18.9% on Dam. A connectivity number cannot tell those apart, and a porous mesh is
        # worse than a fragmented one: a fragmented mesh fails visibly, a porous one walks bots
        # into geometry and looks like a steering bug.
        #
        # Tiles either side of a thin wall touch in plan and sit at similar heights, which is
        # exactly what the step test accepts. The wall table is the thing that knows better.
        wall_segs = {}
        for t in tiles:
            if t["is_floor"]:
                continue
            p3 = t["poly3"]
            for i in range(len(p3)):
                a3, b3 = p3[i], p3[(i + 1) % len(p3)]
                if (a3[0], a3[2]) == (b3[0], b3[2]):
                    continue                      # vertical edge: no extent in plan
                seg = ((a3[0], a3[2]), (b3[0], b3[2]))
                lo_x, hi_x = min(seg[0][0], seg[1][0]), max(seg[0][0], seg[1][0])
                lo_z, hi_z = min(seg[0][1], seg[1][1]), max(seg[0][1], seg[1][1])
                for gx in range(lo_x // CELL, hi_x // CELL + 1):
                    for gz in range(lo_z // CELL, hi_z // CELL + 1):
                        wall_segs.setdefault((gx, gz), []).append(seg)

        def _cross(o, u, v):
            return (u[0] - o[0]) * (v[1] - o[1]) - (u[1] - o[1]) * (v[0] - o[0])

        def crosses_wall(pa, pb):
            lo_x, hi_x = min(pa[0], pb[0]), max(pa[0], pb[0])
            lo_z, hi_z = min(pa[1], pb[1]), max(pa[1], pb[1])
            seen_seg = set()
            for gx in range(lo_x // CELL, hi_x // CELL + 1):
                for gz in range(lo_z // CELL, hi_z // CELL + 1):
                    for s in wall_segs.get((gx, gz), ()):
                        if id(s) in seen_seg:
                            continue
                        seen_seg.add(id(s))
                        d1, d2 = _cross(s[0], s[1], pa), _cross(s[0], s[1], pb)
                        d3, d4 = _cross(pa, pb, s[0]), _cross(pa, pb, s[1])
                        if (d1 > 0) != (d2 > 0) and (d3 > 0) != (d4 > 0):
                            return True
            return False

        nav_step = 0
        nav_refused = 0
        checked = set()
        for cell in buckets.values():
            for i in range(len(cell)):
                a = cell[i]
                for j in range(i + 1, len(cell)):
                    b = cell[j]
                    key = (a["tile"], b["tile"]) if a["tile"] < b["tile"] else (b["tile"], a["tile"])
                    if key in checked:
                        continue
                    checked.add(key)
                    if abs(a["_y"] - b["_y"]) > GE_STEP:
                        continue
                    # Footprints must actually meet in plan. Touching counts: adjacent treads
                    # share a boundary rather than overlapping area.
                    if (a["_bb"][0] > b["_bb"][2] or b["_bb"][0] > a["_bb"][2] or
                            a["_bb"][1] > b["_bb"][3] or b["_bb"][1] > a["_bb"][3]):
                        continue
                    if b["tile"] in nav.get(a["tile"], ()):
                        continue                      # already joined by a shared edge

                    pa = ((a["_bb"][0] + a["_bb"][2]) // 2, (a["_bb"][1] + a["_bb"][3]) // 2)
                    pb = ((b["_bb"][0] + b["_bb"][2]) // 2, (b["_bb"][1] + b["_bb"][3]) // 2)
                    if crosses_wall(pa, pb):
                        nav_refused += 1
                        continue

                    nav.setdefault(a["tile"], set()).add(b["tile"])
                    nav.setdefault(b["tile"], set()).add(a["tile"])
                    nav_step += 1

        # ---------------------------------------------------------------- stairways
        #
        # A stairway is a run of connected tiles that climbs. Now that the mesh joins tiles a body
        # can step between, that is a walk over it collecting edges whose rise is real but within
        # a step -- which is what a tread is.
        #
        # This is emitted rather than left for a consumer to rediscover because "where are the
        # stairs" is a question a bot asks constantly and the answer is expensive to derive at
        # runtime. It also names the direction: a route that has to go UP wants a different tile
        # than one going down, and the mesh alone does not say which end is which.
        #
        # MIN_RISE excludes the merely uneven. Floors are not perfectly flat and a 1-2 unit
        # difference between abutting tiles is authoring noise, not a step; without the floor
        # would come out as thousands of one-tread staircases.
        MIN_RISE = 6

        tile_y = {t["tile"]: t["_y"] for t in floor_tiles}
        tile_room = {t["tile"]: t["room"] for t in floor_tiles}

        rising = {}          # tile -> [higher neighbours within one step]
        for a_id, ns in nav.items():
            if a_id not in tile_y:
                continue
            for b_id in ns:
                if b_id not in tile_y:
                    continue
                d = tile_y[b_id] - tile_y[a_id]
                if MIN_RISE <= d <= GE_STEP:
                    rising.setdefault(a_id, []).append(b_id)

        # Walk each run from its BOTTOM: a tile with a rising neighbour and no tile rising into
        # it. Starting anywhere else would emit the same staircase several times, once per tread.
        has_below = set()
        for a_id, ups in rising.items():
            for b_id in ups:
                has_below.add(b_id)

        stairs = []
        for a_id in sorted(rising):
            if a_id in has_below:
                continue
            chain = [a_id]
            cur = a_id
            guard = 0
            while cur in rising and guard < 200:
                guard += 1
                # Steepest continuation, so a landing that branches does not send the run
                # sideways into a neighbouring flat.
                nxt = max(rising[cur], key=lambda t: tile_y[t] - tile_y[cur])
                if nxt in chain:
                    break                      # a loop: stop rather than spin
                chain.append(nxt)
                cur = nxt
            if len(chain) >= 3:
                stairs.append({
                    "tiles": chain,
                    "room": tile_room.get(chain[0]),
                    "from_y": tile_y[chain[0]],
                    "to_y": tile_y[chain[-1]],
                    "rise": tile_y[chain[-1]] - tile_y[chain[0]],
                })

        # ---------------------------------------------------------------- bridge the doorways
        #
        # Shared edges alone leave the mesh in pieces, and the pieces are ROOMS: on Bunker 1, 60
        # of 66 components sit entirely inside one room. That is not a defect in the geometry --
        # a doorway is a GAP between two tile groups, not a shared edge, so no amount of edge
        # matching will join them.
        #
        # The portal table is the game's own account of which rooms connect and where. Each
        # portal joins the floor tile nearest its opening on each side, which is the tile a body
        # actually stands on as it goes through.
        #
        # Nearest to the OPENING, not nearest to the other tile: a room can be long, and its
        # closest tile to the neighbouring room may be nowhere near the door. That would produce
        # an edge no body could walk, which is the failure this whole exercise exists to remove.
        floor_by_room = {}
        for t in tiles:
            if t["is_floor"]:
                floor_by_room.setdefault(t["room"], []).append(t)

        # WHICH COMPONENT EACH TILE IS IN, so a portal can bridge to a room's MAIN BODY rather
        # than to whatever tile happens to sit nearest its opening.
        #
        # Bunker 1 is why. Room 30 has 226 tiles and exactly one portal, and the nearest tile to
        # that opening is in a FOUR-TILE island rather than the 202-tile body. The level therefore
        # reached room 30 through a dead end, and the room looked unreachable while being perfectly
        # walkable. Nearest-to-the-door is the right instinct and the wrong tile.
        comp_of = {}
        cid = 0
        for t in floor_tiles:
            if t["tile"] in comp_of:
                continue
            stack = [t["tile"]]
            comp_of[t["tile"]] = cid
            while stack:
                cur = stack.pop()
                for nb in nav.get(cur, ()):
                    if nb not in comp_of:
                        comp_of[nb] = cid
                        stack.append(nb)
            cid += 1

        comp_size = {}
        for c in comp_of.values():
            comp_size[c] = comp_size.get(c, 0) + 1

        # `openings` is None when the background model has no portal table -- several levels have
        # none. Guarded rather than assumed: the first version iterated it directly and died
        # mid-run, and because the crash printed above the summary it would have been easy to read
        # the stale files left behind as though the bridging had worked.
        for op in (openings or []):
            rs = op.get("rooms") or []
            poly = op.get("poly") or []
            if len(rs) != 2 or not poly:
                continue
            cx = sum(p[0] for p in poly) / len(poly)
            cz = sum(p[2] for p in poly) / len(poly)

            picked = []
            for r in rs:
                cand = floor_by_room.get(r) or []
                if not cand:
                    picked = []
                    break
                # Restrict to the room's LARGEST component. A room split into a body and a few
                # islands should be entered at its body -- bridging to an island connects the
                # doorway to a dead end and reports the room as reached.
                sizes = {}
                for t in cand:
                    c = comp_of.get(t["tile"])
                    if c is not None:
                        sizes[c] = sizes.get(c, 0) + 1
                if sizes:
                    main = max(sizes, key=lambda c: sizes[c])
                    body = [t for t in cand if comp_of.get(t["tile"]) == main]
                else:
                    body = cand

                best, bestd = None, None
                for t in body:
                    c = t["poly3"]
                    tx = sum(p[0] for p in c) / len(c)
                    tz = sum(p[2] for p in c) / len(c)
                    d = (tx - cx) ** 2 + (tz - cz) ** 2
                    if bestd is None or d < bestd:
                        best, bestd = t["tile"], d
                picked.append(best)

            if len(picked) == 2 and picked[0] != picked[1]:
                nav.setdefault(picked[0], set()).add(picked[1])
                nav.setdefault(picked[1], set()).add(picked[0])
                nav_portal += 1
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
            # once. Measured by the Mac build, who lost a test cycle to it.
            # THE LINKS ARE THE NAVIGATION MESH AND THEY WERE BEING THROWN AWAY.
            #
            # parse_stan has always read each tile's links to its neighbours; they were used to
            # derive ROOM adjacency and then discarded. That is the game's own account of where a
            # character can walk, at tile granularity -- and it is what a bot needs to get round a
            # crate, up a stairway, or through a doorway.
            #
            # The pad graph is not that. Bunker 1 has 45 waypoints against 866 floor tiles: pads
            # are where the designers put PROPS, so routing on them is routing between points of
            # interest and hoping the straight line between two of them is walkable. Half of
            # today's work has been discovering that it frequently is not. The tile graph has no
            # such hope in it -- an edge exists because the game says a body can cross it.
            #
            #   t  tile id, as the game numbers it
            #   l  links to other FLOOR tiles: the walkable edges
            #
            # Links to non-floor tiles are dropped rather than kept and filtered later, because a
            # consumer that forgot to filter would route a body through a wall and the data would
            # look like it said that was fine.
            "floors": [{"t": t["tile"],
                        "r": t["room"],
                        "c": [sum(p[0] for p in t["poly3"]) // len(t["poly3"]),
                              sum(p[1] for p in t["poly3"]) // len(t["poly3"]),
                              sum(p[2] for p in t["poly3"]) // len(t["poly3"])],
                        "bb": [min(p[0] for p in t["poly3"]), min(p[2] for p in t["poly3"]),
                               max(p[0] for p in t["poly3"]), max(p[2] for p in t["poly3"])],
                        "l": sorted(nav.get(t["tile"], ()))}
                       for t in tiles if t["is_floor"]],
            # Stairways, as runs of tiles that climb. Derived from shared-edge and step adjacency
            # only -- the portal bridges are added after this and are deliberately excluded, since
            # a doorway between two heights is a threshold, not a tread.
            "stairs": stairs,
            "waypoint_room": wp_rooms,
            # Floor height under each waypoint, keyed by waypoint INDEX. A seed check for the
            # runtime edge validator: if the tile the engine snapped to is at a materially
            # different height from this, the snap left the node's own surface. See floor_under.
            #
            # Absent for a waypoint with nothing beneath it, which is a finding rather than a
            # default -- a node floating over no floor is worth seeing, and a 0 would hide it.
            "waypoint_floor": wp_floor,
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
