#!/usr/bin/env python3
"""Solve routes from the spawn to every objective, over each stage's own navigation graph.

WHY

The extraction already knows where objectives are: an objective names a tag, a tag resolves to a
prop, a prop to a pad, a pad to a position and a position to the nearest navigation node. What
was missing is the step between two nodes. A bot told "neutralise all alarms" and given four
coordinates still has to cross a level to reach them, and a straight line through a wall is not a
route.

This walks the graph the game itself uses for its guards -- pathwaypoints and path_table -- and
emits an ordered waypoint sequence per objective, with world positions, so a bot can be handed a
list of places to walk to rather than a destination to aim at.

KNOWN LIMITATION: THIS USES ONE OF THE TWO GRAPHS

The extraction emits both `graph` (path_table, per-waypoint) and `region_graph` (path_neighbors,
over waygroups). This router walks only the first, and that is why several stages route badly.

Dam is the clearest case. Its 205 waypoints split into two components -- only 95 reach node 0 --
and it is not an asymmetry bug: the edges are 100% bidirectional already, and treating them as
undirected changes nothing. So three of Dam's four alarms have no path from the start, which
cannot be what the game does, because its own guards patrol that level perfectly well.

The missing connectivity is almost certainly the region graph: waypoints are grouped into
waygroups, and movement between groups goes through the coarse graph rather than through
waypoint links. A correct router needs both layers -- path_table within a group, path_neighbors
between them. Until it does, treat an "unreachable" here as "unreachable through waypoint links
alone", not as "unreachable in the game".

WHAT IT WILL ALSO TELL YOU

Reachability, honestly. Not every stage's graph is one connected piece: Dam's is two components
and only about half its nodes are mutually reachable. An objective in the other component has no
route at all, and saying so is far more useful than emitting a route that stops halfway. Every
objective is reported as routed or unreachable, and the totals are printed.

Usage:
    python3 tools/gen_level_routes.py --out build/levels
"""

import argparse
import glob
import json
import os
import sys
from collections import deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def augment_with_regions(know):
    """Join the two graphs into one walkable adjacency.

    path_table links waypoints WITHIN a waygroup; it does not link groups to each other, which
    is why Dam's 205 waypoints came out as two components with only 95 reaching the start. The
    game does not navigate that way -- waypoints are grouped, and movement between groups goes
    through path_neighbors, the coarse region graph.

    So for every pair of groups the region graph says are adjacent, connect the CLOSEST pair of
    waypoints between them. Closest rather than arbitrary because the join has to be somewhere a
    character could plausibly walk: two adjacent regions touch somewhere, and their nearest
    members are the best available guess at where.

    Returns (adjacency, bridges_added).
    """
    graph = {int(k): list(v) for k, v in know.get("graph", {}).items()}
    regions = {int(k): list(v) for k, v in know.get("region_graph", {}).items()}
    groups = {int(k): list(v) for k, v in know.get("waygroups", {}).items()}
    pos = {w["index"]: w["pos"] for w in know.get("waypoints", []) if w.get("pos")}
    if not regions or not groups:
        return graph, 0

    def link(a, b):
        graph.setdefault(a, [])
        graph.setdefault(b, [])
        if b not in graph[a]:
            graph[a].append(b)
        if a not in graph[b]:
            graph[b].append(a)

    bridges = 0
    done = set()
    for g, neighbours in regions.items():
        for h in neighbours:
            key = (min(g, h), max(g, h))
            if key in done or g == h:
                continue
            done.add(key)
            ga = [w for w in groups.get(g, []) if w in pos]
            gb = [w for w in groups.get(h, []) if w in pos]
            if not ga or not gb:
                continue
            best, bestd = None, None
            for wa in ga:
                pa = pos[wa]
                for wb in gb:
                    pb = pos[wb]
                    d = ((pa[0]-pb[0])**2 + (pa[1]-pb[1])**2 + (pa[2]-pb[2])**2)
                    if bestd is None or d < bestd:
                        best, bestd = (wa, wb), d
            if best:
                link(best[0], best[1])
                bridges += 1
    return graph, bridges


def bfs(graph, start, goal):
    """Shortest node sequence from start to goal, or None.

    Breadth-first rather than weighted: the graph's edges are the game's own waypoint links,
    which are already short hops between adjacent places. Fewest hops is a good proxy for
    shortest walk, and pretending to a precision the data does not carry would be worse.
    """
    if start == goal:
        return [start]
    seen = {start}
    q = deque([(start, [start])])
    while q:
        node, path = q.popleft()
        for nxt in graph.get(node, []):
            if nxt in seen:
                continue
            if nxt == goal:
                return path + [nxt]
            seen.add(nxt)
            q.append((nxt, path + [nxt]))
    return None


import math

# How close a guard has to be to a step for that step to count as covered by them. GoldenEye's
# guards engage well beyond this, but a threat list that includes every guard on the level tells
# a bot nothing -- this is "who is near enough to matter on this leg", not "who could ever see
# you".
THREAT_RADIUS = 700.0

# Positions are recorded at floor level. A ray cast between two floor points grazes geometry it
# should not, so both ends are lifted to roughly where a character's eyes are.
EYE_HEIGHT = 150.0

# Exposure classifies a threat by ROOM, not just distance: distance alone cannot tell a guard
# down the corridor from one through a wall, and those demand opposite behaviour. The rooms come
# from stan, the game's own standing-tile geometry, via gen_level_rooms.py.
#
# ACROSS ALL LEVELS: 1108 same-room, 2149 adjacent, 5011 separated. That is the distribution you
# would expect and the classification is sound.
#
# I twice called it broken before measuring properly, both times from Dam alone, which returns
# ZERO same-room. Dam is genuinely the outlier -- an outdoor level whose routes run along the
# wall past guards inside separate structures -- and reading one level's sample as a verdict on
# the model was the mistake, not the model. Depot is 108/46/30, Egypt 16/1/0, Caverns
# 246/388/374.
#
# What it is NOT is true line of sight. Two points in one large room can still have a pillar
# between them and this calls that visible. It is a good approximation, cheap, and derived from
# the game's own structure rather than invented.


def _tri_hit(p0, p1, a, b, c):
    """Moller-Trumbore: does segment p0->p1 pass through triangle abc?"""
    e1 = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
    e2 = (c[0]-a[0], c[1]-a[1], c[2]-a[2])
    d  = (p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2])
    h  = (d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2], d[0]*e2[1]-d[1]*e2[0])
    det = e1[0]*h[0] + e1[1]*h[1] + e1[2]*h[2]
    if -1e-9 < det < 1e-9:
        return False                      # parallel
    inv = 1.0 / det
    s = (p0[0]-a[0], p0[1]-a[1], p0[2]-a[2])
    u = inv * (s[0]*h[0] + s[1]*h[1] + s[2]*h[2])
    if u < 0.0 or u > 1.0:
        return False
    q = (s[1]*e1[2]-s[2]*e1[1], s[2]*e1[0]-s[0]*e1[2], s[0]*e1[1]-s[1]*e1[0])
    v = inv * (d[0]*q[0] + d[1]*q[1] + d[2]*q[2])
    if v < 0.0 or u + v > 1.0:
        return False
    t = inv * (e2[0]*q[0] + e2[1]*q[1] + e2[2]*q[2])
    return 0.0 < t < 1.0                  # within the segment, not beyond either end


def line_of_sight(p0, p1, walls):
    """True if nothing walls off the segment. CORRECT, AND USELESS ON STAN DATA -- read on.

    This was written to replace the room approximation with a real answer: two points in one
    large room can have a pillar between them, and the room test calls that visible. The maths
    is right and it runs fast (a box reject kills almost every candidate before any triangle
    work). It is kept because it becomes useful the moment it is given real geometry.

    IT CANNOT BE GIVEN THAT BY STAN. Run against stan's non-floor tiles it returned "clear" for
    all 8268 threat rays in the game, including 4989 the room test called separated -- a uniform
    answer, which is the signature of a test that is not testing anything.

    The reason is that STAN IS A NAVIGATION MESH, NOT A VISIBILITY MESH. Its non-floor tiles are
    not walls; they are the little vertical connectors at floor-height transitions -- steps,
    kerbs, ledges. Measured on Dam: median height 6 units, and only 10 of 562 are taller than a
    character's eye. There is nothing there to block a sightline because stan does not describe
    the things that block sightlines. It describes where you can stand.

    Real occlusion lives in the background model (assets/obseg/bg), which is a different and far
    larger dataset. Until that is parsed, the room test is the best available approximation and
    is honest about being one -- and lowering the eye height until something finally blocks would
    be fitting to noise, not fixing the problem.
    """
    lo = (min(p0[0], p1[0]), min(p0[1], p1[1]), min(p0[2], p1[2]))
    hi = (max(p0[0], p1[0]), max(p0[1], p1[1]), max(p0[2], p1[2]))
    for w in walls:
        bb = w["bbox"]
        if bb[3] < lo[0] or bb[0] > hi[0]: continue
        if bb[4] < lo[1] or bb[1] > hi[1]: continue
        if bb[5] < lo[2] or bb[2] > hi[2]: continue
        poly = w["poly"]
        for i in range(1, len(poly) - 1):      # fan-triangulate
            if _tri_hit(p0, p1, poly[0], poly[i], poly[i+1]):
                return False
    return True


def turn_by_turn(know, path, guards, rooms=None):
    """A waypoint sequence into steps a bot can execute, with the threats on each.

    A route as node numbers is not usable: "go to node 139" says nothing about which way to
    face. Each step carries the distance to walk, the heading to walk it on, and the turn from
    the previous heading -- go 240 units, turn left 78 degrees, go 310 -- which is the form the
    instruction actually takes.

    Each step also carries the guards near it and, crucially, the node to fall back to, which is
    simply the previous one. A bot taking fire needs somewhere to go that it knows is walkable,
    and the step it just came from is the only place it has proof of that.
    """
    pos = {w["index"]: w["pos"] for w in know.get("waypoints", []) if w.get("pos")}
    steps = []
    prev_heading = None
    for i in range(len(path) - 1):
        a, b = path[i], path[i + 1]
        pa, pb = pos.get(a), pos.get(b)
        if pa is None or pb is None:
            continue
        dx, dz = pb[0] - pa[0], pb[2] - pa[2]
        dist = math.sqrt(dx * dx + dz * dz + (pb[1] - pa[1]) ** 2)
        heading = math.degrees(math.atan2(dx, dz))
        turn = None
        if prev_heading is not None:
            turn = (heading - prev_heading + 180.0) % 360.0 - 180.0
        prev_heading = heading

        near = []
        step_room = rooms.get("waypoint_room", {}).get(str(b)) if rooms else None
        room_graph = rooms.get("room_graph", {}) if rooms else {}
        walls = rooms.get("walls") if rooms else None
        portals = rooms.get("portals") if rooms else None
        for g in guards:
            gp = g.get("pos")
            if not gp:
                continue
            d = math.sqrt((gp[0] - pb[0]) ** 2 + (gp[1] - pb[1]) ** 2 + (gp[2] - pb[2]) ** 2)
            if d > THREAT_RADIUS:
                continue
            # Distance alone cannot tell a guard down the corridor from one through a wall, and
            # those demand opposite behaviour. The game's own standing tiles carry a room per
            # tile, so: same room is a clear line far more often than not, an adjacent room is
            # reachable through a door, and anything else is a guard who happens to be near in
            # space and irrelevant in practice.
            grm = rooms.get("prop_room", {}).get(str(g.get("propdef"))) if rooms else None
            if step_room is None or grm is None:
                exposure = "unknown"
            elif grm == step_room:
                exposure = "same_room"
            elif grm in room_graph.get(str(step_room), []):
                exposure = "adjacent_room"
            else:
                exposure = "separated"
            # For an adjacent room, being joined by a portal is not the same as seeing through
            # it: a doorway you are not lined up with blocks as surely as a wall. Test whether
            # the sightline actually passes through one of the openings joining the two rooms.
            #
            # Same room is taken as visible and separated as not, which is where the room test
            # was already sound. This refines the middle case, which is the one it could not
            # answer.
            through = None
            if exposure == "adjacent_room" and portals:
                a = (pb[0], pb[1] + EYE_HEIGHT, pb[2])
                bb = (gp[0], gp[1] + EYE_HEIGHT, gp[2])
                through = False
                for op in portals:
                    if step_room not in op["rooms"] or grm not in op["rooms"]:
                        continue
                    poly = op["poly"]
                    for i in range(1, len(poly) - 1):
                        if _tri_hit(a, bb, poly[0], poly[i], poly[i + 1]):
                            through = True
                            break
                    if through:
                        break

            near.append({"chrnum": g.get("obj"), "dist": round(d, 1),
                         "pos": gp, "node": g.get("nav_node"),
                         "room": grm, "exposure": exposure,
                         "through_portal": through})
        # Nearest first within the more dangerous class: a guard in the room beats a closer one
        # behind a wall.
        rank = {"same_room": 0, "adjacent_room": 1, "unknown": 2, "separated": 3}
        near.sort(key=lambda t: (rank.get(t["exposure"], 9), t["dist"]))

        steps.append({
            "from": a, "to": b,
            "distance": round(dist, 1),
            "heading": round(heading, 1),
            "turn": None if turn is None else round(turn, 1),
            "threats": near,
            # Where to go if this step goes wrong. The node just left is the only place with
            # proof of walkability and of what was there a moment ago.
            "retreat_to": a,
        })
    return steps


def spawn_node(know, graph):
    """Where a run starts, as a graph node.

    Campaign stages carry no explicit player start among the props, so this is an assumption
    either way -- but it must be a USEFUL one, and the obvious choice was not.

    Taking the lowest-numbered waypoint looked harmless and was badly wrong: on Surface, node 0
    links only to node 1 and that pair is an orphan, so routing from it reached 2 of 251 nodes
    and every objective on the stage came back unreachable. The graph was fine; the start was in
    a corner of it. That failure is indistinguishable from a broken level unless you measure.

    So: start in the LARGEST connected component, at its best-connected node. Being in the big
    component is what makes routes possible at all, and picking a hub within it keeps the first
    leg short. Still an assumption, still labelled one -- only the first leg depends on it.
    """
    nodes = [w["index"] for w in know.get("waypoints", []) if w.get("pos")]
    if not nodes:
        return None

    seen, best = set(), []
    for n in nodes:
        if n in seen:
            continue
        comp, q = [], deque([n])
        seen.add(n)
        while q:
            c = q.popleft()
            comp.append(c)
            for m in graph.get(c, []):
                if m not in seen:
                    seen.add(m)
                    q.append(m)
        if len(comp) > len(best):
            best = comp
    if not best:
        return min(nodes)
    return max(best, key=lambda n: len(graph.get(n, [])))


def node_pos(know, node):
    for w in know.get("waypoints", []):
        if w.get("index") == node:
            return w.get("pos")
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join("build", "levels"))
    args = ap.parse_args()
    out_dir = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)

    tac_files = sorted(glob.glob(os.path.join(out_dir, "*.tactics.json")))
    if not tac_files:
        print("no tactics files in %s -- run gen_level_tactics.py first" % out_dir)
        return 1

    total_obj = total_routed = total_unreachable = total_untargeted = 0

    for tp in tac_files:
        level = os.path.basename(tp)[:-len(".tactics.json")]
        kp = os.path.join(out_dir, level + ".json")
        if not os.path.exists(kp):
            continue
        know, tact = load(kp), load(tp)
        graph, bridges = augment_with_regions(know)
        guards = [p for p in know.get("props", []) if p.get("type") == "Guard" and p.get("pos")]
        rp = os.path.join(out_dir, level + ".rooms.json")
        rooms = load(rp) if os.path.exists(rp) else None

        # PREFER THE PORTAL GRAPH. GoldenEye's renderer is portal-based and the background model
        # carries a portal_data_table naming the two rooms each portal joins -- the game's own
        # answer to "can these rooms see into each other", which is precisely what the exposure
        # test is asking. Everything before this inferred adjacency from tile links or from the
        # navigation graph; both were guesses at something the data states outright.
        #
        # Validated before trusting: stan room ids and portal room ids are the same numbering
        # space. Facility and Bunker 1 share every room between the two tables, Dam 68 of 69,
        # Control 81 of 82.
        if rooms and rooms.get("portal_graph"):
            rooms = dict(rooms)
            rooms["room_graph"] = rooms["portal_graph"]
        elif rooms:
            rg = {k: set(v) for k, v in rooms.get("room_graph", {}).items()}
            wr = rooms.get("waypoint_room", {})
            for a, nbrs in know.get("graph", {}).items():
                ra = wr.get(a)
                if ra is None:
                    continue
                for b in nbrs:
                    rb = wr.get(str(b))
                    if rb is None or rb == ra:
                        continue
                    rg.setdefault(str(ra), set()).add(rb)
                    rg.setdefault(str(rb), set()).add(ra)
            rooms = dict(rooms)
            rooms["room_graph"] = {k: sorted(v) for k, v in rg.items()}
        props_by_tag = {p["tag"]: p for p in know.get("props", []) if p.get("tag") is not None}
        start = spawn_node(know, graph)

        routes = []
        for obj in tact.get("objectives", []):
            # Every tag the objective names, whatever the verb: destroy, collect, photograph or
            # copy all point at a tagged prop, and a bot has to walk to it either way.
            tags = obj.get("target_tags") or obj.get("destroy_tags") or []
            targets = []
            for t in tags:
                p = props_by_tag.get(t)
                if p is None or p.get("nav_node") is None:
                    continue
                targets.append((t, p))

            if not targets:
                # Most objectives are not "destroy a tagged thing" -- they are install, collect,
                # photograph, escort. Those carry no tag, so there is nothing to route to yet,
                # and that is a gap in the extraction rather than in the level.
                routes.append({"objective": obj.get("objective"), "text": obj.get("text"),
                               "routable": False, "why": "no tagged target"})
                total_untargeted += 1
                total_obj += 1
                continue

            legs = []
            here = start
            for t, p in targets:
                goal = p["nav_node"]
                path = bfs(graph, here, goal) if here is not None else None
                legs.append({
                    "tag": t, "type": p.get("type"), "pad": p.get("pad"),
                    "pos": p.get("pos"), "node": goal,
                    "hops": (len(path) - 1) if path else None,
                    "path": path,
                    "reachable": path is not None,
                    "steps": turn_by_turn(know, path, guards, rooms) if path else [],
                })
                if path:
                    here = goal

            reachable = sum(1 for l in legs if l["reachable"])
            routes.append({
                "objective": obj.get("objective"), "text": obj.get("text"),
                "routable": reachable > 0,
                "targets": len(legs), "reachable": reachable,
                "legs": legs,
            })
            total_obj += 1
            if reachable == len(legs):
                total_routed += 1
            else:
                total_unreachable += 1

        doc = {
            "level": level,
            "spawn_node": start,
            "spawn_pos": node_pos(know, start) if start is not None else None,
            "spawn_is_assumed": True,
            "counts": {"objectives": len(routes),
                       "routable": sum(1 for r in routes if r.get("routable"))},
            "routes": routes,
        }
        with open(os.path.join(out_dir, level + ".routes.json"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1)

        shown = ", ".join(
            "obj%s:%s" % (r["objective"],
                          ("%dhops" % sum(l["hops"] for l in r["legs"] if l["hops"] is not None))
                          if r.get("routable") else "-")
            for r in routes)
        print("%-10s spawn=%-5s bridges=%-3d %s" % (level, start, bridges, shown))

    print("\nobjectives %d: %d fully routed, %d partly unreachable, %d with no tagged target"
          % (total_obj, total_routed, total_unreachable, total_untargeted))
    print("\nUnreachable is real information, not a failure: several stages' graphs are more")
    print("than one component, so an objective can genuinely have no path from the start node.")
    print("Emitting a route that stops halfway would hide that.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
