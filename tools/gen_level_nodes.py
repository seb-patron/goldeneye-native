#!/usr/bin/env python3
"""S6: one table that maps a node id to a position, so a route can be read.

WHY THIS EXISTS. Route paths name node ids and nothing maps an id back to a place. Validating a
route, or a wall set against a route, is impossible without it -- mac-getv could not check walls
against real routes for exactly this reason.

🔴 AND THE FIRST THING THIS FOUND IS THAT A BARE ID IS AMBIGUOUS. Train has 104 engine waypoints
numbered from 0 and 682 floor tiles numbered from 0. `path: [74, 75, 76]` in a route is waypoint
74; tile 74 is a different place entirely. Any table keyed on a plain integer silently answers the
wrong question for one of the two, and answers it confidently.

So every node here carries a NAMESPACE, and the file also publishes a `uid` -- namespace base plus
local id -- for consumers that need a single flat integer. The bases are far apart and stated in
the file, so a uid can be decoded by eye when something looks wrong at 2am.

⚠️ THE ENGINE'S GRAPH IS THE REAL ONE. padhalllv.c is a complete two-level pathfinder the guards
have used all along, and its nodes come from g_CurrentSetup.pathwaypoints -- `waypoint {padID,
*neighbours, groupNum, dist}` with waygroups above. Those ids mean something to the engine. The
tile graph is OUR reconstruction of the same space and its ids mean nothing outside our tools, so
the two are labelled differently rather than merged into one anonymous pool.

⚠️ ASSET SPACE, with levelscale published beside it, matching gen_level_walls.py. Every position in
this file is asset; runtime = asset / levelscale.
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

# Namespace bases. Chosen far apart and round so a uid is readable at a glance: 1_000_074 is
# plainly waypoint 74, and nothing has to be looked up to see it. Deliberately NOT packed tightly
# -- saving three digits would cost the one property that makes this useful.
NS = {
    "waypoint": 1000000,   # the ENGINE's own graph, from pathwaypoints. Authoritative.
    "tile":     2000000,   # our stan floor mesh. A reconstruction; ids are ours, not the game's.
    "door":     3000000,   # door props, which routes traverse but which are not floor
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    ap.add_argument("--level", default=None, help="one level, or all when omitted")
    a = ap.parse_args()

    from pack_world import load_level_scales
    scales = load_level_scales(ROOT)

    names = []
    for f in sorted(os.listdir(a.levels)):
        if not f.endswith(".json"):
            continue
        stem = f[:-5]
        if any(k in stem for k in ("nuance", ".mp", "_general", "_prop", "_engine")):
            continue
        # ⚠️ .nodes IS IN THIS LIST BECAUSE IT WAS NOT, and the tool consumed its own output as a
        # level -- writing train.nodes.nodes.json with zero of everything. A generator that scans
        # the directory it writes into has to exclude its own extension or it grows a new empty
        # file on every run.
        if any(stem.endswith(s) for s in (".rooms", ".routes", ".tactics", ".nodes")):
            continue
        if a.level and stem != a.level:
            continue
        names.append(stem)

    total = 0
    for lv in names:
        kpath = os.path.join(a.levels, "%s.json" % lv)
        rpath = os.path.join(a.levels, "%s.rooms.json" % lv)
        if not os.path.isfile(kpath):
            continue
        know = json.load(open(kpath, encoding="utf-8"))
        rooms = json.load(open(rpath, encoding="utf-8")) if os.path.isfile(rpath) else {}

        nodes = []
        unresolved = []

        # ---- the engine's waypoints -------------------------------------------------------
        graph = know.get("graph", {}) or {}
        wroom = rooms.get("waypoint_room", {}) or {}
        wfloor = rooms.get("waypoint_floor", {}) or {}
        group_of = {}
        for gid, members in (know.get("waygroups", {}) or {}).items():
            for m in members:
                group_of[int(m)] = int(gid)

        for w in know.get("waypoints", []) or []:
            i = w["index"]
            pos = w.get("pos")
            if not pos:
                # Reported, never dropped. A waypoint the engine lists but whose pad we cannot
                # resolve is a hole in OUR extraction, and a table that quietly omits it lets a
                # route reference an id this file swears does not exist.
                unresolved.append({"ns": "waypoint", "id": i, "why": "no resolvable pad"})
                continue
            nodes.append({
                "uid": NS["waypoint"] + i,
                "ns": "waypoint", "id": i,
                "pos": [round(c, 1) for c in pos],
                "kind": "pad",              # engine waypoints are anchored to pads (waypoint.padID)
                "pad": w.get("pad"),
                "room": wroom.get(str(i)),
                "floor_y": wfloor.get(str(i)),
                "group": group_of.get(i),
                "links": [NS["waypoint"] + int(n) for n in graph.get(str(i), [])],
            })

        # ---- our tile mesh ----------------------------------------------------------------
        for f in rooms.get("floors", []) or []:
            nodes.append({
                "uid": NS["tile"] + f["t"],
                "ns": "tile", "id": f["t"],
                "pos": [round(c, 1) for c in f["c"]],
                "kind": "tile",
                "room": f.get("r"),
                "links": [NS["tile"] + int(n) for n in f.get("l", [])],
            })

        # ---- doors ------------------------------------------------------------------------
        for p in know.get("props", []) or []:
            if p.get("type") != "Door" or not p.get("pos"):
                continue
            nodes.append({
                "uid": NS["door"] + p["propdef"],
                "ns": "door", "id": p["propdef"],
                "pos": [round(c, 1) for c in p["pos"]],
                "kind": "door",
                "room": (rooms.get("prop_room", {}) or {}).get(str(p["propdef"])),
                "radius": p.get("radius"),
                "links": [],                # a door's connectivity belongs to whoever routes it
            })

        out = {
            "level": lv,
            "space": "asset",
            "levelscale": scales.get(lv),
            "namespaces": NS,
            "note": ("uid = namespaces[ns] + id. A bare id is AMBIGUOUS: waypoint 74 and tile 74 "
                     "are different places and both exist. Prefer uid."),
            "counts": {k: sum(1 for n in nodes if n["ns"] == k) for k in NS},
            "unresolved": unresolved,
            "nodes": nodes,
        }
        dest = os.path.join(a.levels, "%s.nodes.json" % lv)
        with open(dest, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=1)
        total += len(nodes)
        print("  %-12s %5d nodes  (%3d waypoint, %4d tile, %3d door)%s"
              % (lv, len(nodes), out["counts"]["waypoint"], out["counts"]["tile"],
                 out["counts"]["door"],
                 "  %d UNRESOLVED" % len(unresolved) if unresolved else ""))

    print("\n%d levels, %d nodes" % (len(names), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
