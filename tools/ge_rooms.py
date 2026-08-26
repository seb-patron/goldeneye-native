#!/usr/bin/env python3
"""The level's rooms and the portals between them, as a graph you can route on.

WHY THIS EXISTS. A player that navigates by searching the floor in front of it can only find what
it can see, and on Train that is not enough: the bot stood at (-1388, -235) in room 5 for four
minutes reporting "nothing reachable gets any nearer", fifty-eight units from the portal into room
6. The floor search never sampled that point, and no amount of making the search finer or wider
fixed it -- both attempts made the run worse, because a slower search means the player stands
still while it runs.

The level already knows the answer. Rooms are connected by portals, the extractor writes them to
<level>.rooms.json, and the route from room 5 to room 7 is two hops through geometry Rare placed
deliberately. So: route coarsely on the room graph, steer finely to the next portal. That is
hierarchical pathfinding, and it is the standard answer to exactly this failure.

COORDINATES. The extracted polygons are in ASSET units; everything the running game reports is in
runtime units, and asset = runtime * levelscale. Conversions happen here, once, so no caller has
to remember which space it is holding.
"""
import json
import os
import re
from collections import deque

LEVELS_DIR = os.path.join("build", "levels")


def level_scale(level):
    """asset = runtime * levelscale, parsed from the decomp so there is one copy of the number."""
    p = os.path.join("vendor", "ge-decomp", "src", "game", "bg.c")
    if not os.path.exists(p):
        return 1.0
    with open(p, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    for m in re.finditer(r'\{LEVELID_(\w+),\s*"[^"]+",\s*"[^"]+",\s*([0-9.]+),', src):
        if m.group(1).lower() == level:
            return float(m.group(2))
    return 1.0


class Rooms(object):
    def __init__(self, level):
        self.ok = False
        path = os.path.join(LEVELS_DIR, "%s.rooms.json" % level)
        if not os.path.exists(path):
            return
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)

        scale = level_scale(level) or 1.0
        self.graph = {int(k): [int(v) for v in vs] for k, vs in data.get("portal_graph", {}).items()}

        # Portal centres, keyed both ways round: a portal is a door between two rooms and the
        # question is always "I am in A and want B", never "portal 8".
        self.centre = {}
        for p in data.get("portals", []):
            rooms = p.get("rooms") or []
            poly = p.get("poly") or []
            if len(rooms) != 2 or not poly:
                continue
            cx = sum(v[0] for v in poly) / len(poly) / scale
            cz = sum(v[2] for v in poly) / len(poly) / scale
            self.centre[(rooms[0], rooms[1])] = (cx, cz)
            self.centre[(rooms[1], rooms[0])] = (cx, cz)
        self.ok = bool(self.graph and self.centre)

    def route(self, start, goal):
        """Rooms to pass through, start first and goal last, or [] if there is no way."""
        if not self.ok or start == goal:
            return []
        seen = {start: None}
        q = deque([start])
        while q:
            cur = q.popleft()
            if cur == goal:
                chain = []
                while cur is not None:
                    chain.append(cur)
                    cur = seen[cur]
                return list(reversed(chain))
            for nxt in self.graph.get(cur, ()):
                if nxt not in seen:
                    seen[nxt] = cur
                    q.append(nxt)
        return []

    def next_portal(self, start, goal):
        """Where to walk to leave `start` on the way to `goal`, in runtime units, or None."""
        chain = self.route(start, goal)
        if len(chain) < 2:
            return None
        return self.centre.get((chain[0], chain[1]))


def _selftest():
    """Run directly: tools/ge_rooms.py -- checks the case that motivated the module."""
    r = Rooms("train")
    if not r.ok:
        print("train.rooms.json not found or empty; nothing to check")
        return 1
    chain = r.route(5, 7)
    portal = r.next_portal(5, 7)
    print("train: room 5 to room 7 goes %s" % (" -> ".join(str(c) for c in chain) or "nowhere"))
    print("       leave room 5 at (%.0f %.0f)" % portal if portal else "       no portal found")
    ok = chain == [5, 6, 7] and portal is not None
    print("selftest: %s" % ("pass" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(_selftest())
