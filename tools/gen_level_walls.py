#!/usr/bin/env python3
"""Emit each level's WALLS as data, derived from the floor mesh.

The bot has been discovering walls by ray, one frame at a time, which is why it walks into them:
a probe answers "is there something 79 units ahead" and by then the body is already committed. A
wall is not a thing to be discovered, it is a thing that is simply true about the level, and it
should be available the same way the floor is.

The derivation needs no new extraction. A floor tile knows its own extent and its neighbours, so
the parts of its boundary that NO neighbour covers are exactly the places a body cannot leave the
mesh from -- the walls. Where a neighbour does cover a span, that span is a doorway or an open
join, and the bot may cross it freely. This is the whole permission set: inside the mesh is where
it can go, a boundary span is where it has no chance of going.

Segments are emitted in ASSET space, matching rooms.json and what pack_world expects; the runtime
divides by levelscale at its own boundary like everything else.
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_level_routes import level_scales

def spans_minus(full, covered):
    """The parts of interval `full` that `covered` does not cover."""
    lo, hi = full
    out, cuts = [], sorted(covered)
    at = lo
    for c0, c1 in cuts:
        if c1 <= at or c0 >= hi:
            continue
        if c0 > at:
            out.append((at, min(c0, hi)))
        at = max(at, c1)
        if at >= hi:
            break
    if at < hi:
        out.append((at, hi))
    # Sub-unit slivers are float noise on a shared edge, not gaps a body could pass through.
    return [s for s in out if (s[1] - s[0]) > 1.0]

def open_the_doors(segs, doors, half=35.0):
    """Cut a gap in the wall wherever there is a door.

    ⚠️ THIS IS NOT A REFINEMENT, IT IS REQUIRED. Tile neighbour lists do not bridge rooms, so the
    one place the mesh is genuinely passable -- the doorway -- has no neighbour covering it and
    falls out of the derivation as solid wall. Measured on Train: all 53 doors came back sealed,
    and a bot handed that set is being told the level has no way through, which is exactly the
    failure it then produces.

    The gap is cut, not the segment dropped. A door sits in a wall and the wall either side of it
    is still real; deleting the whole segment would open the carriage.
    """
    if not doors:
        return segs
    out = []
    for x0, z0, x1, z1, y in segs:
        horizontal = abs(z1 - z0) < 1e-6
        axis0, axis1 = (x0, x1) if horizontal else (z0, z1)
        at = z0 if horizontal else x0
        lo, hi = min(axis0, axis1), max(axis0, axis1)

        cuts = []
        for d in doors:
            dx, dz = float(d[0]), float(d[2])
            across, along = (dz, dx) if horizontal else (dx, dz)
            if abs(across - at) > half:
                continue
            cuts.append((along - half, along + half))

        for a, b in (spans_minus((lo, hi), cuts) if cuts else [(lo, hi)]):
            if horizontal:
                out.append([a, at, b, at, y])
            else:
                out.append([at, a, at, b, y])
    return out


def doors_of(level):
    """Door positions from the level's own prop export -- asset space, same as the tiles."""
    p = os.path.join("build", "levels", "%s.json" % level)
    if not os.path.exists(p):
        return []
    know = json.load(open(p))
    out = []
    for pr in (know.get("props") or []):
        if "door" not in str(pr.get("type", "")).lower():
            continue
        pos = pr.get("pos")
        if pos:
            out.append(pos)
    return out


def walls_for(rooms):
    floors = rooms.get("floors") or []
    by_id = {f["t"]: f for f in floors if f.get("t") is not None and f.get("bb")}
    segs = []
    for f in by_id.values():
        x0, z0, x1, z1 = [float(v) for v in f["bb"]]
        y = float(f["c"][1]) if f.get("c") else 0.0
        nb = [by_id[n] for n in (f.get("l") or []) if n in by_id]

        # Four sides. For each, collect the spans neighbours share with it, then keep the rest.
        # A neighbour counts only if it actually ABUTS this side -- a diagonal neighbour touches
        # at a corner and covers nothing, and treating it as coverage opens a wall that is real.
        sides = (
            ("z", z0, (x0, x1), lambda n: abs(float(n["bb"][3]) - z0) < 2.0, lambda n: (float(n["bb"][0]), float(n["bb"][2]))),
            ("z", z1, (x0, x1), lambda n: abs(float(n["bb"][1]) - z1) < 2.0, lambda n: (float(n["bb"][0]), float(n["bb"][2]))),
            ("x", x0, (z0, z1), lambda n: abs(float(n["bb"][2]) - x0) < 2.0, lambda n: (float(n["bb"][1]), float(n["bb"][3]))),
            ("x", x1, (z0, z1), lambda n: abs(float(n["bb"][0]) - x1) < 2.0, lambda n: (float(n["bb"][1]), float(n["bb"][3]))),
        )
        for axis, at, full, abuts, span_of in sides:
            covered = [span_of(n) for n in nb if abuts(n)]
            for a, b in spans_minus(full, covered):
                if axis == "z":
                    segs.append([a, at, b, at, y])
                else:
                    segs.append([at, a, at, b, y])
    return segs

def main():
    d = "build/levels"
    names = sys.argv[1:] or sorted(
        n[:-len(".rooms.json")] for n in os.listdir(d) if n.endswith(".rooms.json"))
    scales = level_scales()
    for name in names:
        p = os.path.join(d, "%s.rooms.json" % name)
        if not os.path.exists(p):
            print("  %-10s no rooms.json" % name); continue
        rooms = json.load(open(p))
        segs = walls_for(rooms)
        doors = doors_of(name)
        before = len(segs)
        segs = open_the_doors(segs, doors)
        # 🔑 asset = runtime * levelscale. Emitting the scale beside the segments means the loader
        # divides once at its own boundary and every consumer downstream is in runtime space --
        # the alternative is each consumer remembering to convert, which is how Train's spawn
        # ended up thousands of units outside its own level.
        scale = scales.get(name, 1.0)
        out = os.path.join(d, "%s.walls.json" % name)
        json.dump({"level": name, "space": "asset", "levelscale": scale, "segments": segs},
                  open(out, "w"), separators=(",", ":"))
        tiles = len(rooms.get("floors") or [])
        print("  %-10s %5d wall segment(s) from %d tile(s), %d door gap(s) cut, levelscale %g"
              % (name, len(segs), tiles, len(doors), scale))

main()
