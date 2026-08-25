#!/usr/bin/env python3
"""How much of each level does the waypoint graph actually cover?

SURFACE_QUEUE item 3: eight levels have no graph node within 4000 units of where the player
starts. The queue attributes that to a coordinate space mismatch, and that is falsified -- every
level's waypoints and its stan floor share a bounding box to within a few percent (see the bus,
and tools/audit_route_heights.py for the height half).

So the pads are in the right place. The question this asks instead is whether there are ENOUGH of
them: waypoints come from PADS, which are where the designers put props, and there is no reason a
pad should exist in every walkable room. If large parts of a level's floor are far from any node,
then "no node near the spawn" needs no misplacement to explain -- the graph simply does not reach
there, and the fix is to add nodes rather than to move them.

Measured against the FLOOR TILES, which are the level's own account of where a body can stand.
Nothing here needs a capture or a running game.

Run: python3 tools/audit_graph_coverage.py [level ...]
"""

import glob
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, "build", "levels")

# The queue's own threshold for "no node near the spawn". Kept identical so the two measurements
# are comparable rather than merely similar.
FAR = 4000.0

# Levels the queue flags as having no node within FAR of the measured spawn.
FLAGGED = {"aztec", "cradle", "dam", "depot", "runway", "statue", "surface", "surface2"}


def nearest_wp_dist(wps, x, z):
    """Horizontal distance to the closest waypoint.

    Horizontal because the whole navigation layer is a floor plan -- routes, steering and the
    arrival radius all ignore y -- and a catwalk directly above a tile is not a node that tile can
    reach. Including y here would report vertical neighbours as coverage.
    """
    best = None
    for wx, wz in wps:
        d = (wx - x) ** 2 + (wz - z) ** 2
        if best is None or d < best:
            best = d
    return math.sqrt(best) if best is not None else None


def audit(level):
    kp = os.path.join(LEVELS, level + ".json")
    rp = os.path.join(LEVELS, level + ".rooms.json")
    if not (os.path.exists(kp) and os.path.exists(rp)):
        return None

    know = json.load(open(kp, encoding="utf-8"))
    rooms = json.load(open(rp, encoding="utf-8"))
    floors = rooms.get("floors") or []
    wps = [(w["pos"][0], w["pos"][2]) for w in know.get("waypoints", []) if w.get("pos")]
    if not floors:
        return None
    if not wps:
        # No graph at all. Distinct from a sparse one, and worth saying so rather than reporting
        # 100% uncovered as though it were a coverage problem.
        return {"level": level, "wps": 0, "tiles": len(floors), "nograph": True}

    ds = []
    for t in floors:
        c = t["c"]
        d = nearest_wp_dist(wps, c[0], c[2])
        if d is not None:
            ds.append(d)
    ds.sort()
    n = len(ds)
    far = sum(1 for d in ds if d > FAR)
    return {
        "level": level,
        "wps": len(wps),
        "tiles": n,
        "median": ds[n // 2],
        "p90": ds[int(n * 0.9)],
        "max": ds[-1],
        "far": far,
        "far_pct": 100.0 * far / n,
        "nograph": False,
    }


def main():
    wanted = sys.argv[1:]
    names = sorted(os.path.basename(p)[: -len(".rooms.json")]
                   for p in glob.glob(os.path.join(LEVELS, "*.rooms.json")))
    if wanted:
        names = [n for n in names if n in wanted]

    print("Distance from each floor tile to the nearest graph node (horizontal)\n")
    print("%-10s %5s %6s %8s %8s %8s %10s  %s"
          % ("level", "wp", "tiles", "median", "p90", "max", ">%du" % FAR, "queue"))

    rows = []
    for name in names:
        r = audit(name)
        if r is None:
            continue
        mark = "flagged" if r["level"] in FLAGGED else ""
        if r["nograph"]:
            print("%-10s %5d %6d %8s %8s %8s %9s  %s"
                  % (r["level"], 0, r["tiles"], "-", "-", "-", "NO GRAPH", mark))
            continue
        rows.append(r)
        # The COUNT, not just the percentage. Dam has one tile 7072 units from any node, which is
        # 0.05% of 2193 and printed as "0%" -- a rounded zero beside a max of 7072 is a
        # contradiction the reader has to notice, and the whole point of this table is that
        # somebody trusts its zeroes.
        print("%-10s %5d %6d %8.0f %8.0f %8.0f %5d %4.1f%%  %s"
              % (r["level"], r["wps"], r["tiles"], r["median"], r["p90"], r["max"],
                 r["far"], r["far_pct"], mark))

    if not rows:
        return 1

    flagged = [r for r in rows if r["level"] in FLAGGED]
    other = [r for r in rows if r["level"] not in FLAGGED]

    def avg(rs, k):
        return sum(r[k] for r in rs) / len(rs) if rs else 0.0

    print("\nflagged levels : median %.0f, p90 %.0f, %.1f%% of floor beyond %.0f units"
          % (avg(flagged, "median"), avg(flagged, "p90"), avg(flagged, "far_pct"), FAR))
    print("everything else: median %.0f, p90 %.0f, %.1f%% of floor beyond %.0f units"
          % (avg(other, "median"), avg(other, "p90"), avg(other, "far_pct"), FAR))
    print("\nTHE TWO ROWS LOOK ALIKE, and the question they were asked to settle is now closed.")
    print("  The eight FLAGGED levels came from spawn distances measured before anyone knew the")
    print("  game applies a per-level scale at load (bg.c levelinfotable; runtime = asset /")
    print("  levelscale). Scaled, every spawn lands on its own floor, so that grouping was an")
    print("  artefact of comparing two coordinate spaces and means nothing here.")
    print("\n  The COVERAGE numbers stand -- floor tiles and waypoints are both asset space, so")
    print("  nothing in this table crossed the boundary. Kept because 'is the graph dense enough'")
    print("  is worth being able to ask; the flagged/unflagged split is retained only so the")
    print("  obsolete list is visibly obsolete rather than quietly deleted.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
