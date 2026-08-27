#!/usr/bin/env python3
"""Do the measured spawns land on the floor data, and in the room the game says?

mac-getv's docs/captures/spawns.json records where the game ACTUALLY puts the player, measured by
booting each level rather than read from assets. That settles three things that have been argued
from inference all day:

  1. Is the spawn inside the level's own geometry at all? Dam's spawn was reported 20,254 units
     from the nearest graph node, which is 1.5x the level's entire horizontal extent -- a position
     that far out is not in the level, and if so no amount of adding nodes fixes it.

  2. Does the room the game reports match the room whose floor tiles contain the spawn? Bunker 1's
     spawn is reported in room 29, and room 29's six tiles are all at y=93 while the spawn is at
     y=340. Either the numbering disagrees or the spawn is not over room 29's floor.

  3. What is the real body-to-floor offset, per level? It has been quoted as ~157 units from one
     Bunker 1 measurement. If it varies, anything comparing a body position to a floor height needs
     to say which level it is on.

Nothing here needs a running game -- the capture already happened.
"""

import importlib.util
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, "build", "levels")
SPAWNS = os.path.join(ROOT, "docs", "captures", "spawns.json")


def level_scales():
    """Per-level scale from bg.c's levelinfotable, via mac-getv's parser in pack_world.py.

    ⚠️ THIS IS WHY THE FIRST VERSION OF THIS FILE WAS WRONG. The extraction is in ASSET space and
    the game runs in a scaled one: runtime = asset / levelscale, applied at load by setLevelScale.
    Dam's scale is 0.23364, its floor tiles reach x=4735, and the measured spawn is x=20198 -- the
    same place, expressed twice.

    Comparing the two directly reported 17 of 20 spawns outside their own level and 19 of 20 with
    no floor beneath them. Both numbers were real and neither meant what I said it meant.

    Imported rather than reimplemented: pack_world.py owns this table, and a second parser is a
    second thing to be wrong. That is the walkable_verdicts lesson.
    """
    path = os.path.join(ROOT, "tools", "pack_world.py")
    spec = importlib.util.spec_from_file_location("pack_world", path)
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
    except SystemExit:
        pass
    return mod.load_level_scales(ROOT)


def containing_tiles(floors, x, z):
    """Floor tiles whose XZ footprint contains the point."""
    return [t for t in floors
            if t["bb"][0] <= x <= t["bb"][2] and t["bb"][1] <= z <= t["bb"][3]]


def nearest_tile(floors, x, z):
    best, bestd = None, None
    for t in floors:
        dx = t["c"][0] - x
        dz = t["c"][2] - z
        d = dx * dx + dz * dz
        if bestd is None or d < bestd:
            best, bestd = t, d
    return best, (bestd ** 0.5 if bestd is not None else None)


def main():
    if not os.path.exists(SPAWNS):
        print("no docs/captures/spawns.json -- it is a runtime capture from the Mac")
        return 1

    doc = json.load(open(SPAWNS, encoding="utf-8"))
    spawns = doc.get("spawns") or {}
    scales = level_scales()

    print("Measured spawns against the extracted floor (frame %s)\n" % doc.get("frame"))
    print("%-10s %-22s %5s %6s %8s %8s  %s"
          % ("level", "spawn xz", "room", "under", "nearest", "offset", "verdict"))

    offsets = []
    mismatch = 0
    outside = 0
    for level in sorted(spawns):
        s = spawns[level]
        pos = s.get("pos")
        rp = os.path.join(LEVELS, level + ".rooms.json")
        if not pos or not os.path.exists(rp):
            continue
        floors = (json.load(open(rp, encoding="utf-8")).get("floors") or [])
        if not floors:
            continue

        # Runtime -> asset, so the spawn can be compared to the extracted floor at all.
        # runtime = asset / levelscale, therefore asset = runtime * levelscale.
        sc = scales.get(level, 1.0) or 1.0
        x, y, z = pos[0] * sc, pos[1] * sc, pos[2] * sc
        inside = containing_tiles(floors, x, z)
        near, dist = nearest_tile(floors, x, z)

        # The tile a body would stand on: highest contained at or below, same rule as
        # gen_level_rooms.floor_under. Falls back to the nearest when nothing contains the point.
        under = None
        if inside:
            below = [t for t in inside if t["c"][1] <= y]
            under = max(below, key=lambda t: t["c"][1]) if below else min(
                inside, key=lambda t: t["c"][1])

        room_under = under["r"] if under else None
        off = (y - under["c"][1]) if under else None
        if off is not None:
            offsets.append((off, level))

        verdict = []
        if not inside:
            outside += 1
            verdict.append("NO TILE CONTAINS IT (nearest %.0fu)" % dist)
        elif room_under != s.get("room"):
            mismatch += 1
            verdict.append("ROOM MISMATCH: game says %s, floor says %s"
                           % (s.get("room"), room_under))
        else:
            verdict.append("ok")

        print("%-10s %-22s %5s %6s %8.0f %8s  %s"
              % (level, "%.0f,%.0f" % (x, z), s.get("room"),
                 room_under if room_under is not None else "-",
                 dist, ("%.0f" % off) if off is not None else "-",
                 " ".join(verdict)))

    print("\n%d spawn(s) with no floor tile beneath them, %d in a different room than the game "
          "reports" % (outside, mismatch))

    if offsets:
        offsets.sort()
        print("\nbody-above-floor offset: %.0f (%s) .. %.0f (%s), median %.0f"
              % (offsets[0][0], offsets[0][1], offsets[-1][0], offsets[-1][1],
                 offsets[len(offsets) // 2][0]))
        print("  A SINGLE NUMBER FOR THIS IS UNSAFE if that range is wide. It has been quoted as")
        print("  ~157 from one Bunker 1 reading, and anything comparing a body position to a floor")
        print("  height needs to know which it is holding.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
