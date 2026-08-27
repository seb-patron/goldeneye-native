#!/usr/bin/env python3
"""What SHAPE is each level? Corridor, open, or something between.

WHY

A bot that knows Train is a 30:1 corridor plays it completely differently from one that treats it
as open ground: on a corridor the answer to "which way" is almost always "along the axis", and
lateral space exists for cover rather than for route choice. That is a fact about the level, it is
cheap to measure, and nothing in the pack said it.

It came out of a human reconstruction of Train that Evan supplied, which described the level as
six cars on one axis with no branching. Our own extraction agrees to within wall thickness -- 53
doors spanning 22,244 units on X against 752 on Z -- which makes the document a CHECK ON OUR DATA
as much as a source, and it is the third independent confirmation of the levelscale fix.

WHAT IT MEASURES

Extent along each axis from the doors and waypoints, in RUNTIME coordinates, plus the ratio
between the dominant and cross axes:

    ratio >= 8   corridor   -- the axis is the route; lateral space is cover
    ratio >= 3   elongated  -- a main axis with real rooms off it
    otherwise    open       -- route choice is genuine

Extents, not connectivity. A doughnut-shaped level and a solid one look identical here, and a
level with two floors reads as its footprint. This says which way the level RUNS, not how it
joins up -- the room graph answers that and disagreeing with it means one of them is wrong.
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def level_scales():
    """Parsed from the decomp so there is one copy of these constants, not two."""
    path = os.path.join(ROOT, "vendor", "ge-decomp", "src", "game", "bg.c")
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    for m in re.finditer(r'\{LEVELID_(\w+),\s*"[^"]+",\s*"[^"]+",\s*([0-9.]+),', src):
        out[m.group(1).lower()] = float(m.group(2))
    return out


def classify(ratio):
    if ratio >= 8.0:
        return "corridor"
    if ratio >= 3.0:
        return "elongated"
    return "open"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join("build", "levels"))
    args = ap.parse_args()
    d = args.levels if os.path.isabs(args.levels) else os.path.join(ROOT, args.levels)

    scales = level_scales()
    rows = []
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".json") or "." in fn[:-5]:
            continue
        level = fn[:-5]
        scale = scales.get(level)
        with open(os.path.join(d, fn), encoding="utf-8") as fh:
            know = json.load(fh)

        pts = [w["pos"] for w in know.get("waypoints", []) if w.get("pos")]
        pts += [p["pos"] for p in know.get("props", []) if p.get("pos")]
        if len(pts) < 8:
            continue

        # Runtime coordinates, so the numbers mean the same thing as everything else we print.
        # A level with no scale is reported rather than silently mixed in.
        inv = (1.0 / scale) if scale else None
        if inv is None:
            print("  %-10s NO LEVELSCALE -- skipped rather than reported in the wrong space" % level)
            continue

        xs = [p[0] * inv for p in pts]
        ys = [p[1] * inv for p in pts]
        zs = [p[2] * inv for p in pts]
        ex, ey, ez = max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)

        # Horizontal only: vertical extent is storeys, not shape, and folding it in makes a
        # tower look like a corridor lying on its side.
        big, small = (ex, ez) if ex >= ez else (ez, ex)
        axis = "X" if ex >= ez else "Z"
        ratio = (big / small) if small > 1.0 else 999.0
        rows.append((ratio, level, axis, big, small, ey))

    rows.sort(reverse=True)
    print("%-10s %-9s %5s %9s %9s %8s" % ("level", "shape", "axis", "along", "across", "vert"))
    for ratio, level, axis, big, small, ey in rows:
        print("%-10s %-9s %5s %9.0f %9.0f %8.0f   %.1f:1"
              % (level, classify(ratio), axis, big, small, ey, ratio))

    corridors = [r for r in rows if r[0] >= 8.0]
    print("\n%d of %d levels are corridors (>= 8:1)" % (len(corridors), len(rows)))
    if corridors:
        print("  " + ", ".join(r[1] for r in corridors))


if __name__ == "__main__":
    main()
