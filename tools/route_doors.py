#!/usr/bin/env python3
"""S2's acceptance test: route a level over the TILE graph and report the doors crossed, in order.

WHY THIS AND NOT A BOT. Train's walkthrough describes a linear chain of carriages joined by doors,
with seven brake units along it. If our navmesh reproduces that shape, the graph is right; if it
does not, the graph is wrong in a way no coverage percentage would show -- 88% coverage and a
route that teleports between carriages both look fine in a summary. This runs with no bot, no
running game and no frame budget, which matters on a box that renders at about one frame a second.

PADS ARE NOT PLACES TO STAND. 139 of Train's 180 pad nodes cannot be stood on, so a route
graph built from pads hands a follower targets a body cannot occupy. The TILES are standable by
construction -- they are the floor -- which is why the graph here is tiles and their shared-edge
adjacency, not waypoints.

TWO SPACES, AND THEY MUST BE RECONCILED EXPLICITLY. The extracted JSONs are ASSET space; the
measured spawn was read out of the running game and is RUNTIME space. For Train they differ by
1/0.15019713, about 6.66x -- the spawn reads x=779 where the entire tile map ends at x=213, so
using it unconverted does not merely shift the answer, it starts the route outside the level.
asset = runtime * levelscale. Stated here rather than assumed, because this exact pairing is what
levelscale was hiding behind.

THE ROUTE STARTS FROM THE MEASURED SPAWN, NOT routes.json's. That file carries
spawn_is_assumed=true, and a route from an assumed start measures the assumption.
"""
import argparse
import heapq
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))


def load_scale(level):
    from pack_world import load_level_scales
    return load_level_scales(ROOT).get(level)


def build_graph(floors):
    """tile id -> (centre, neighbours). Adjacency is already geometric (shared edges), wall
    filtered, with portal bridges to each room's largest component."""
    pos = {f["t"]: f["c"] for f in floors}
    adj = {f["t"]: [n for n in f.get("l", []) if n in pos] for f in floors}
    return pos, adj


def nearest_tile(pos, p):
    best, bd = None, None
    for t, c in pos.items():
        d = (c[0] - p[0]) ** 2 + (c[2] - p[2]) ** 2
        if bd is None or d < bd:
            best, bd = t, d
    return best, math.sqrt(bd)


def dijkstra(pos, adj, src, dst):
    """Shortest path by real distance, not hop count.

    Hop count would prefer a chain of large tiles over a shorter run of small ones and report a
    route that is not the one a body would walk."""
    dist = {src: 0.0}
    prev = {}
    q = [(0.0, src)]
    seen = set()
    while q:
        d, u = heapq.heappop(q)
        if u in seen:
            continue
        seen.add(u)
        if u == dst:
            break
        cu = pos[u]
        for v in adj.get(u, ()):
            cv = pos[v]
            w = math.dist((cu[0], cu[2]), (cv[0], cv[2]))
            nd = d + w
            if nd < dist.get(v, float("inf")):
                dist[v] = nd
                prev[v] = u
                heapq.heappush(q, (nd, v))
    if dst not in dist:
        return None, None
    path, u = [], dst
    while u != src:
        path.append(u)
        u = prev[u]
    path.append(src)
    path.reverse()
    return path, dist[dst]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", nargs="?", default="train")
    ap.add_argument("--reach", type=float, default=60.0,
                    help="how near a door must be to a path tile to count as crossed (asset units)")
    a = ap.parse_args()

    lv = os.path.join(ROOT, "build", "levels")
    rooms = json.load(open(os.path.join(lv, "%s.rooms.json" % a.level), encoding="utf-8"))
    know = json.load(open(os.path.join(lv, "%s.json" % a.level), encoding="utf-8"))
    caps = json.load(open(os.path.join(ROOT, "docs", "captures", "spawns.json"), encoding="utf-8"))

    scale = load_scale(a.level)
    spawn_rt = caps["spawns"][a.level]["pos"]
    spawn = [c * scale for c in spawn_rt]
    print("level %s   levelscale %.8f" % (a.level, scale))
    print("spawn  runtime %s -> asset (%.0f, %.0f)" % (spawn_rt, spawn[0], spawn[2]))

    pos, adj = build_graph(rooms["floors"])
    print("tiles %d   mean adjacency %.1f"
          % (len(pos), sum(len(v) for v in adj.values()) / max(1, len(adj))))

    start, sd = nearest_tile(pos, spawn)
    print("start tile %d, %.0f units from the spawn (room %s)"
          % (start, sd, rooms.get("prop_room", {}).get(str(start), "?")))

    # The far end of the corridor. Train runs on X, so the objective end is the extreme X reachable
    # from the spawn -- chosen from the graph rather than named, so this works on any level.
    reach = {start}
    stack = [start]
    while stack:
        u = stack.pop()
        for v in adj.get(u, ()):
            if v not in reach:
                reach.add(v)
                stack.append(v)
    print("reachable from spawn: %d of %d tiles (%.0f%%)"
          % (len(reach), len(pos), 100.0 * len(reach) / len(pos)))

    far = max(reach, key=lambda t: abs(pos[t][0] - pos[start][0]))
    path, dist = dijkstra(pos, adj, start, far)
    if not path:
        print("NO ROUTE")
        return 1
    print("route: %d tiles, %.0f asset units (%.0f runtime)"
          % (len(path), dist, dist / scale))

    # Doors along the route, in the order the route meets them.
    doors = [d for d in know["props"] if d.get("type") == "Door" and d.get("pos")]
    on = []
    for i, t in enumerate(path):
        c = pos[t]
        for d in doors:
            dp = d["pos"]
            if math.dist((c[0], c[2]), (dp[0], dp[2])) <= a.reach:
                key = d["pad_name"]
                if key not in [x[0] for x in on]:
                    on.append((key, i, dp))
    print("\ndoors on the route: %d of %d in the level" % (len(on), len(doors)))
    for name, i, dp in on:
        print("   %-10s at tile %3d/%d   x=%7.0f" % (name, i, len(path), dp[0]))

    # the room chain -- the acceptance test that has no tuning parameter.
    #
    # Counting "doors crossed" turned out to be the wrong measure, and the way it failed is worth
    # keeping. Door PROPS are leaves, not openings: a double door is two props at the same spot, so
    # 53 props are far fewer doorways. Clustering them by proximity gives 34, 11, 5 or 5 openings
    # at gaps of 10, 40, 80 and 150 units -- the answer moves with the knob, and choosing the knob
    # that yields the expected number is fitting the data to the acceptance criterion.
    #
    # Rooms are discrete and already assigned, so the chain they form needs no threshold. "A linear
    # chain of carriages" means exactly: every room entered once, none revisited.
    #
    # and it turns out to prove almost nothing -- reported anyway, with this warning attached,
    # because deleting a check that failed is how the same idea gets tried again in a fortnight.
    #
    # Measured across levels: Train, Bunker 1, Dam, Facility and Archives all come back
    # "linear chain: yes", including Bunker 1 at a 0.8:1 aspect ratio and Archives at 1.1:1, which
    # are open levels and nothing like a chain of carriages. The cause is structural rather than
    # geometric: a SHORTEST PATH has very little reason to re-enter a room it has left, so the
    # property holds for almost any level and almost any graph. It would hold on a graph that was
    # wrong.
    #
    # A test that passes everywhere cannot distinguish a good navmesh from a bad one. The measures
    # below do discriminate -- Train 100% monotonic at 53:1 against Bunker 1's 69% at 0.8:1 -- so
    # those are the ones to read.
    room_of = {f["t"]: f["r"] for f in rooms["floors"]}
    seq = []
    for t in path:
        r = room_of[t]
        if not seq or seq[-1] != r:
            seq.append(r)
    revisits = len(seq) - len(set(seq))
    print("\n-- room chain --")
    print("   %d transitions through %d distinct rooms" % (len(seq) - 1, len(set(seq))))
    print("   " + " -> ".join(str(r) for r in seq))
    print("   revisited rooms: %d   linear chain: %s   <-- WEAK: every level tested passes this,"
          % (revisits, "yes" if revisits == 0 else "no"))
    print("       including open ones, so it does not discriminate. Read the shape numbers below.")

    print("\n-- shape --")
    xs = [pos[t][0] for t in path]
    zs = [pos[t][2] for t in path]
    print("route spans x %.0f  z %.0f  -> ratio %.1f:1"
          % (max(xs) - min(xs), max(zs) - min(zs),
             (max(xs) - min(xs)) / max(1.0, (max(zs) - min(zs)))))
    mono = sum(1 for i in range(1, len(xs)) if (xs[i] - xs[i - 1]) * (xs[-1] - xs[0]) >= 0)
    print("monotonic along the dominant axis: %d of %d steps (%.0f%%)"
          % (mono, len(xs) - 1, 100.0 * mono / max(1, len(xs) - 1)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
