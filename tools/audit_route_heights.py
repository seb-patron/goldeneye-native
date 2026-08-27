#!/usr/bin/env python3
"""How much does the planar route graph actually cost?

The graph carries a y on every node and routes on none of it. I hit that as a bot beelining
horizontally at a doorway it could not reach. Before modelling a descent, this measures how
widespread the problem is -- because "the graph is planar" is a description of the code, not a
measurement of the levels, and a fix nobody needs is worse than no fix.

TWO QUESTIONS, and the first has to be answered before the second means anything.

1. IS A WAYPOINT'S y THE FLOOR IT STANDS ON? Waypoints come from PADS -- where the designers put
   props -- and a pad height is not obviously a floor height. If pads sit at a consistent offset
   above the floor, then every comparison below has to account for it, and anyone comparing a pad
   height to a probed floor gets a phantom cliff. This already cost a test cycle over exactly
   that gap between BODY position and floor (157 units apart on Bunker 1).

2. HOW MANY EDGES ARE UNWALKABLE ON HEIGHT ALONE? An edge whose endpoints differ by more than a
   body can climb is a straight line through geometry. Counting them says whether the planar graph
   is a real obstacle or a theoretical one.

Run: python3 tools/audit_route_heights.py [level ...]
"""

import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, "build", "levels")

# What a walking body can climb between two adjacent nodes. GoldenEye's own step handling is
# generous -- stairs are climbed without a jump -- but a whole storey is not. 90 units is roughly
# the drop the bot's path follower uses as its limit elsewhere, kept the same here so the two agree.
MAX_STEP = 90.0


def floor_under(floors, x, y, z):
    """Nearest floor tile to an XZ point: containment first, then nearest centroid.

    Containment first because levels are multi-storey and several tiles cover the same footprint.
    Among the candidates, the answer is the HIGHEST tile at or below the point -- the floor you
    would land on, not the lowest one in the stack.

    That distinction is the whole measurement. Taking the lowest contained tile reported a median
    pad-above-floor of 2078 units on cradle, and cradle is a tall antenna structure: the tile
    containing a platform pad in plan view is the ground far beneath it. The number was real
    containment and a wrong question -- "how far above the ground floor is this pad" instead of
    "how far above its own floor". Orphan counts were 0 everywhere, so nothing flagged it.

    When nothing is at or below the point (a pad under a walkway, or one slightly below its own
    tile through rounding), the lowest tile above is the least-wrong answer and the offset comes
    out negative, which is visible rather than silently plausible.

    Returns (tile, contained). CONTAINED MATTERS AND MUST NOT BE COLLAPSED: when no tile covers
    the point, the nearest centroid can be most of a level away and at any height, and its offset
    is a measurement of the fallback rather than of the pad. The first run of this file reported a
    median pad-above-floor of 2078 units on cradle, which is not a pad height -- it is what
    nearest-centroid returns for waypoints standing over nothing. Mixing the two would have made
    the headline number meaningless while looking precise.
    """
    inside = []
    for t in floors:
        bb = t["bb"]
        if bb[0] <= x <= bb[2] and bb[1] <= z <= bb[3]:
            inside.append(t)
    if inside:
        below = [t for t in inside if t["c"][1] <= y]
        if below:
            return max(below, key=lambda t: t["c"][1]), True
        return min(inside, key=lambda t: t["c"][1]), True

    best, bestd = None, None
    for t in floors:
        cx, _, cz = t["c"]
        d = (cx - x) ** 2 + (cz - z) ** 2
        if bestd is None or d < bestd:
            best, bestd = t, d
    return best, False


def audit(level):
    kp = os.path.join(LEVELS, level + ".json")
    rp = os.path.join(LEVELS, level + ".rooms.json")
    if not (os.path.exists(kp) and os.path.exists(rp)):
        return None

    know = json.load(open(kp, encoding="utf-8"))
    rooms = json.load(open(rp, encoding="utf-8"))
    floors = rooms.get("floors") or []
    if not floors:
        return {"level": level, "note": "no floor tiles -- rerun gen_level_rooms.py"}

    pos = {w["index"]: w["pos"] for w in know.get("waypoints", []) if w.get("pos")}
    graph = {int(k): list(v) for k, v in know.get("graph", {}).items()}

    # Question 1: pad height against the floor beneath it.
    offsets = []      # only waypoints standing over a real tile
    orphans = 0       # waypoints with no floor beneath them at all
    for idx, p in pos.items():
        t, contained = floor_under(floors, p[0], p[1], p[2])
        if t and contained:
            offsets.append(p[1] - t["c"][1])
        else:
            orphans += 1

    # Question 2: edges that no body could walk on height alone.
    steep = []
    seen = set()
    edges = 0
    for a, ns in graph.items():
        for b in ns:
            key = (min(a, b), max(a, b))
            if key in seen or a == b:
                continue
            seen.add(key)
            if a not in pos or b not in pos:
                continue
            edges += 1
            dy = abs(pos[a][1] - pos[b][1])
            if dy > MAX_STEP:
                steep.append((dy, a, b))
    steep.sort(reverse=True)

    offsets.sort()
    n = len(offsets)
    return {
        "level": level,
        "waypoints": len(pos),
        "floors": len(floors),
        "edges": edges,
        "steep": len(steep),
        "orphans": orphans,
        "worst": steep[:3],
        "off_median": offsets[n // 2] if n else None,
        "off_min": offsets[0] if n else None,
        "off_max": offsets[-1] if n else None,
        "off_spread": (offsets[-1] - offsets[0]) if n else None,
    }


def main():
    wanted = sys.argv[1:]
    paths = sorted(glob.glob(os.path.join(LEVELS, "*.rooms.json")))
    names = [os.path.basename(p)[: -len(".rooms.json")] for p in paths]
    if wanted:
        names = [n for n in names if n in wanted]

    print("Waypoint height vs the floor beneath it, and edges too steep to walk (>%.0f units)\n"
          % MAX_STEP)
    print("%-10s %5s %6s %6s %6s %7s   %7s %7s %7s" %
          ("level", "wp", "floors", "edges", "steep", "orphan", "off_med", "off_min", "off_max"))

    tot_e = tot_s = 0
    rows = []
    for name in names:
        r = audit(name)
        if r is None:
            continue
        if "note" in r:
            print("%-10s %s" % (name, r["note"]))
            continue
        rows.append(r)
        tot_e += r["edges"]
        tot_s += r["steep"]
        print("%-10s %5d %6d %6d %6d %7d   %7s %7s %7s" %
              (r["level"], r["waypoints"], r["floors"], r["edges"], r["steep"], r["orphans"],
               r["off_median"], r["off_min"], r["off_max"]))

    if not rows:
        print("\nnothing to audit")
        return 1

    print("\n%d edges across %d levels, %d too steep to walk (%.1f%%)"
          % (tot_e, len(rows), tot_s, 100.0 * tot_s / tot_e if tot_e else 0.0))

    meds = sorted(r["off_median"] for r in rows if r["off_median"] is not None)
    print("per-level median pad-above-floor offset: %d .. %d" % (meds[0], meds[-1]))
    print("\nIF THAT RANGE IS TIGHT, a pad height is a floor height plus a constant and the two")
    print("  are comparable once it is subtracted. IF IT IS WIDE, pads are not floor-aligned at")
    print("  all and nothing downstream should compare them without saying which it holds.")

    worst = sorted(((r["steep"], r["level"]) for r in rows), reverse=True)[:5]
    print("\nlevels with the most unwalkable edges: " +
          ", ".join("%s(%d)" % (l, s) for s, l in worst if s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
