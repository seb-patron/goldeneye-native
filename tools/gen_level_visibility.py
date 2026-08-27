#!/usr/bin/env python3
"""Measure SIGHTLINES and COVER per level by casting rays against the derived wall set.

WHY THIS EXISTS. audit_bot_archetypes.py needs to know whether a level offers long sightlines (a
sniper archetype wants them) and whether it offers cover (most archetypes want some). Its first
attempt used proxies -- mean tile adjacency for cover, aspect ratio for sightlines -- and both were
measuring the wrong quantity: adjacency is mesh CONNECTIVITY, so a room full of crates scores like
an empty one, and ratio is ELONGATION, so a large square hall fails while a corridor passes. Run
across several levels they produced identical verdicts, which is how they were caught.

These are the real questions, and the wall segments answer them:

  SIGHTLINE  from a standable point, how far can a ray travel before a wall stops it? Sampled over
             many directions and many points, the distribution is the level's own answer to "can
             you see far here".
  COVER      what fraction of directions from a point are blocked within a short radius? A point in
             the open has almost none blocked; a point beside a crate or in a doorway has many.
             That is occlusion, which is what cover actually means.

HEIGHT IS RESPECTED. Wall segments carry the floor height of the tile that produced them, and a
wall on another deck must not block a ray on this one. Ignoring that made an earlier audit report
multi-storey levels as almost entirely walled.

SAMPLED, AND THE SAMPLE SIZE IS REPORTED. Full pairwise visibility on 2,294 tiles against 7,532
walls is tens of millions of tests. Points and directions are sampled with a FIXED stride rather
than randomly, so the figures are reproducible run to run -- a metric that moves when nothing
changed is one nobody trusts.
"""
import argparse
import glob
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EPS = 1e-9


def load_walls(path):
    """[(x1,z1,x2,z2,y), ...] from gen_level_walls.py output."""
    d = json.load(open(path, encoding="utf-8"))
    out = []
    for s in d.get("segments", []):
        if isinstance(s, (list, tuple)) and len(s) >= 4:
            y = float(s[4]) if len(s) >= 5 else None
            out.append((float(s[0]), float(s[1]), float(s[2]), float(s[3]), y))
    return out


class Grid:
    """Uniform bucket grid over wall segments.

    Without it every ray tests every wall: 7,532 walls x 64 directions x 200 points is 96 million
    segment tests for one level. The grid makes a ray touch only the buckets it crosses, which is
    the difference between seconds and an afternoon.
    """

    def __init__(self, walls, cell=200.0):
        self.cell = cell
        self.b = {}
        for w in walls:
            x1, z1, x2, z2, _ = w
            # Every cell the segment's bounding box touches. Coarse -- a diagonal segment lands in
            # cells it does not cross -- but over-inclusion only costs a few extra exact tests,
            # whereas under-inclusion would silently miss real walls.
            for cx in range(int(min(x1, x2) // cell), int(max(x1, x2) // cell) + 1):
                for cz in range(int(min(z1, z2) // cell), int(max(z1, z2) // cell) + 1):
                    self.b.setdefault((cx, cz), []).append(w)

    def near(self, x0, z0, x1, z1):
        cell = self.b, self.cell
        seen, out = set(), []
        c = self.cell
        for cx in range(int(min(x0, x1) // c), int(max(x0, x1) // c) + 1):
            for cz in range(int(min(z0, z1) // c), int(max(z0, z1) // c) + 1):
                for w in self.b.get((cx, cz), ()):
                    i = id(w)
                    if i not in seen:
                        seen.add(i)
                        out.append(w)
        return out


def ray_hit(grid, x0, z0, dx, dz, reach, y, yband):
    """Distance to the first wall along a ray, or `reach` if nothing blocks it."""
    x1, z1 = x0 + dx * reach, z0 + dz * reach
    best = reach
    for (ax, az, bx, bz, wy) in grid.near(x0, z0, x1, z1):
        if wy is not None and y is not None and abs(wy - y) > yband:
            continue                      # another deck
        rx, rz = bx - ax, bz - az
        den = dx * rz - dz * rx
        if abs(den) < EPS:
            continue                      # parallel
        t = ((ax - x0) * rz - (az - z0) * rx) / den     # along the ray
        u = ((ax - x0) * dz - (az - z0) * dx) / den     # along the wall
        if 0.0 < t < best and 0.0 <= u <= 1.0:
            best = t
    return best


def measure(walls, pts, reach, dirs, cover_r, yband):
    grid = Grid(walls)
    sight, cover = [], []
    for (x, z, y) in pts:
        longest, blocked = 0.0, 0
        for k in range(dirs):
            a = 2.0 * math.pi * k / dirs
            dx, dz = math.cos(a), math.sin(a)
            d = ray_hit(grid, x, z, dx, dz, reach, y, yband)
            if d > longest:
                longest = d
            if d < cover_r:
                blocked += 1
        sight.append(longest)
        cover.append(blocked / float(dirs))
    return sight, cover


def pct(v, q):
    if not v:
        return 0.0
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    ap.add_argument("--level", default=None)
    ap.add_argument("--points", type=int, default=180, help="sampled standable points per level")
    ap.add_argument("--dirs", type=int, default=32, help="ray directions per point")
    ap.add_argument("--reach", type=float, default=4000.0, help="max sightline, asset units")
    ap.add_argument("--cover-radius", type=float, default=120.0,
                    help="a ray stopped inside this counts as cover")
    ap.add_argument("--yband", type=float, default=150.0, help="deck separation, asset units")
    a = ap.parse_args()

    out = {}
    for f in sorted(glob.glob(os.path.join(a.levels, "*.walls.json"))):
        lv = os.path.basename(f)[: -len(".walls.json")]
        if a.level and lv != a.level:
            continue
        rooms = os.path.join(a.levels, "%s.rooms.json" % lv)
        if not os.path.isfile(rooms):
            continue
        walls = load_walls(f)
        floors = json.load(open(rooms, encoding="utf-8")).get("floors") or []
        if not walls or not floors:
            continue

        # Fixed stride, not random: the same tree gives the same numbers every run.
        stride = max(1, len(floors) // a.points)
        pts = [(fl["c"][0], fl["c"][2], fl["c"][1]) for fl in floors[::stride]][: a.points]

        sight, cover = measure(walls, pts, a.reach, a.dirs, a.cover_radius, a.yband)
        out[lv] = {
            "points": len(pts), "walls": len(walls), "dirs": a.dirs,
            "sight_median": round(pct(sight, 0.5), 1),
            "sight_p90": round(pct(sight, 0.9), 1),
            "sight_max": round(max(sight), 1),
            "cover_mean": round(sum(cover) / len(cover), 3),
            "cover_p10": round(pct(cover, 0.1), 3),
        }
        print("  %-12s walls %5d  pts %3d | sightline med %7.1f p90 %7.1f | cover mean %.2f"
              % (lv, len(walls), len(pts), out[lv]["sight_median"], out[lv]["sight_p90"],
                 out[lv]["cover_mean"]))

    dest = os.path.join(a.levels, "_visibility.json")
    with open(dest, "w", encoding="utf-8") as fh:
        json.dump({"note": "measured by ray-casting the derived wall set; ASSET units",
                   "params": {"points": a.points, "dirs": a.dirs, "reach": a.reach,
                              "cover_radius": a.cover_radius, "yband": a.yband},
                   "levels": out}, fh, indent=1)
    print("\n%d level(s) -> %s" % (len(out), dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
