#!/usr/bin/env python3
"""Is a walkability capture trustworthy, and does it meet item 1's finish condition?

SURFACE_QUEUE item 1: the edge validator's answer depends entirely on the seed tile. Same Bunker 1
level, same 2926 pairs, three seeds: 98%, 73%, 0% walkable. `sub_GAME_7F0AFB78` snaps to the
nearest standable tile and *nearest* can be through a wall, so a snap-seeded run reports clear
lines the player demonstrably cannot walk.

The fix is to put a body at every node and test from there, which needs a running game. This is
the other half: given a capture, decide whether to believe it -- offline, before anyone routes on
it.

Why this exists AT all. A percentage is not a verdict. A run that seeds badly still completes and
still prints a number, and 98% looks better than 73% while being the more wrong of the two. Every
check below is a way for a capture to fail LOUDLY that it would otherwise pass quietly.

The finish condition IS encoded, not described. The queue says Bunker 1's spawn must have no
walkable edge to the portal at ~1109 units and must have one along the corridor toward the door.
Written down, that is prose someone has to check by eye; written here, it is a test that fails.

    python3 tools/audit_walkable.py [level ...]
"""

import glob
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, "build", "levels")

# The queue's numbers for Bunker 1, kept as constants so a change to them is visible in a diff
# rather than buried in a comparison.
B1_PORTAL_DIST = 1109.0
B1_TOLERANCE = 60.0

# How far the engine's seed may sit from the node's own floor before the seed is suspect. A snap
# that crosses a wall usually lands on a surface at a different height; one that stays on the same
# surface does not. Generous, because a legitimate seed can sit a step above or below.
SEED_SUSPECT = 120.0


def load(level, name):
    p = os.path.join(LEVELS, level + name)
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8") as fh:
        return json.load(fh)


def audit(level):
    verdicts = load(level, ".walkable.json")
    if verdicts is None:
        return {"level": level, "missing": True}

    know = load(level, ".json") or {}
    rooms = load(level, ".rooms.json") or {}
    pos = {w["index"]: w["pos"] for w in know.get("waypoints", []) if w.get("pos")}
    floor = {int(k): v for k, v in (rooms.get("waypoint_floor") or {}).items()}

    edges = verdicts.get("edges", [])
    seen = {}
    asym = 0
    for e in edges:
        try:
            a, b, ok = int(e["a"]), int(e["b"]), bool(e["ok"])
        except (KeyError, TypeError, ValueError):
            continue
        key = (min(a, b), max(a, b))
        if key in seen and seen[key] != ok:
            asym += 1
        seen[key] = seen.get(key, ok) and ok    # both directions must agree

    walkable = sum(1 for v in seen.values() if v)

    # SEED SANITY. A node whose own floor is far below it is one the engine's snap is most likely
    # to resolve somewhere else, because there is a lot of vertical room for "nearest" to mean
    # something surprising. Flagging them says which verdicts to distrust without re-running.
    suspect = []
    for idx, p in pos.items():
        f = floor.get(idx)
        if f is None:
            suspect.append((idx, None))          # nothing beneath it at all
        elif (p[1] - f) > SEED_SUSPECT:
            suspect.append((idx, p[1] - f))

    return {
        "level": level,
        "missing": False,
        "mask": verdicts.get("mask"),
        "probe": verdicts.get("probe"),
        "rows": len(edges),
        "pairs": len(seen),
        "walkable": walkable,
        "asym": asym,
        "nodes": len(pos),
        "suspect": suspect,
    }


def check_bunker1_finish():
    """The queue's finish condition, as a test rather than a sentence.

    Returns a list of (passed, description). Reported per clause so a half-met condition is
    visible: "the spawn refuses the portal" and "the spawn reaches the door" are different claims
    and a run can satisfy one without the other.
    """
    know = load("bunker1", ".json") or {}
    verdicts = load("bunker1", ".walkable.json")
    if verdicts is None:
        return [(None, "no capture for bunker1")]

    pos = {w["index"]: w["pos"] for w in know.get("waypoints", []) if w.get("pos")}
    ok_pairs = set()
    for e in verdicts.get("edges", []):
        if e.get("ok"):
            ok_pairs.add((min(int(e["a"]), int(e["b"])), max(int(e["a"]), int(e["b"]))))

    # The spawn is whichever node the capture calls the spawn; fall back to the highest index,
    # which is where the synthetic nodes are appended.
    spawn = verdicts.get("spawn_node")
    if spawn is None:
        spawn = max(pos) if pos else None
    if spawn is None or spawn not in pos:
        return [(None, "cannot identify the spawn node")]

    out = []
    far = [(i, math.dist(pos[spawn], pos[i])) for i in pos if i != spawn]
    near_portal = [i for i, d in far if abs(d - B1_PORTAL_DIST) <= B1_TOLERANCE]
    if not near_portal:
        out.append((None, "no node ~%.0f units from the spawn to test" % B1_PORTAL_DIST))
    else:
        bad = [i for i in near_portal
               if (min(spawn, i), max(spawn, i)) in ok_pairs]
        out.append((not bad,
                    "spawn has NO walkable edge to the node ~%.0f away%s"
                    % (B1_PORTAL_DIST, "" if not bad else " (but %s is walkable)" % bad)))

    reachable = [i for i, d in far if d < 700 and (min(spawn, i), max(spawn, i)) in ok_pairs]
    out.append((bool(reachable),
                "spawn HAS a walkable edge along the corridor (%d within 700u)" % len(reachable)))
    return out


def main():
    wanted = sys.argv[1:]
    caps = sorted(os.path.basename(p)[: -len(".walkable.json")]
                  for p in glob.glob(os.path.join(LEVELS, "*.walkable.json")))
    names = wanted or caps

    if not caps:
        print("No .walkable.json captures found in build/levels.\n")
        print("These are produced by tools/validate_edges.sh, which boots the game once per")
        print("level. This machine cannot run it at usable speed, so the captures come from the")
        print("Mac. Everything below runs the moment one lands.\n")

    print("%-10s %6s %7s %8s %7s %8s  %s"
          % ("level", "rows", "pairs", "walkable", "asym", "suspect", "mask"))
    for name in names:
        r = audit(name)
        if r.get("missing"):
            print("%-10s %6s %7s %8s %7s %8s  %s" % (name, "-", "-", "-", "-", "-", "no capture"))
            continue
        print("%-10s %6d %7d %7d%% %7d %8d  %s"
              % (r["level"], r["rows"], r["pairs"],
                 (100 * r["walkable"] // r["pairs"]) if r["pairs"] else 0,
                 r["asym"], len(r["suspect"]), r["mask"] or "UNRECORDED"))
        if r["mask"] is None:
            print(" no mask recorded -- the mask decides the answer "
                  "(CDTYPE_DOORS seals every room)")
        if r["asym"]:
            print("           %d pair(s) disagree by direction; both must agree to count as "
                  "walkable" % r["asym"])

    print("\n--- item 1 finish condition, Bunker 1 ---")
    for passed, desc in check_bunker1_finish() or []:
        mark = "  ok  " if passed else ("  FAIL" if passed is False else "  n/a ")
        print("%s  %s" % (mark, desc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
