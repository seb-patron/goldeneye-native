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
import heapq
import math
import sys
from collections import deque

# tools/ on the path so walkable_verdicts imports whichever way this script is invoked.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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


def bfs(graph, start, goal, pos=None):
    """Cheapest node sequence from start to goal, or None. Weighted by DISTANCE.

    This was breadth-first, on the reasoning that the edges are already short hops so fewest
    hops is a good proxy. That stopped being true the moment synthetic nodes were added. A spawn
    node links to whatever is nearest and a portal or door node links across a room, so the graph
    now carries edges from 40 to 1300 units and BFS PRICES THEM THE SAME.

    It picks the long leap every time, and the long leap is exactly the edge least likely to be
    walkable. Bunker 1: the router chose spawn -> portal at 1342 units over spawn -> door at 555,
    then the bot spent every run pressed against the wall the 1342-unit line passes through. The
    route was well formed and unwalkable, which is this project's most expensive failure shape.

    Falls back to hop counting when no positions are supplied, so a caller without them gets the
    old behaviour rather than a crash.
    """
    if start == goal:
        return [start]
    if pos is None:
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

    def cost(a, b):
        pa, pb = pos.get(a), pos.get(b)
        if pa is None or pb is None:
            # An edge with no geometry cannot be priced. Charge it heavily rather than free, so
            # it is a last resort instead of a shortcut the router reaches for.
            return 10000.0
        return math.hypot(pa[0] - pb[0], pa[2] - pb[2])

    best = {start: 0.0}
    heap = [(0.0, start, [start])]
    while heap:
        d, node, path = heapq.heappop(heap)
        if node == goal:
            return path
        if d > best.get(node, float("inf")):
            continue
        for nxt in graph.get(node, []):
            nd = d + cost(node, nxt)
            if nd < best.get(nxt, float("inf")):
                best[nxt] = nd
                heapq.heappush(heap, (nd, nxt, path + [nxt]))
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
                # Named eye_* rather than a/b: `a` is the step's origin NODE in the enclosing
                # loop, and reusing the name here silently replaced it with a position, so every
                # step recorded coordinates where a node index belonged. Nothing complained --
                # the JSON stayed well-formed and only broke when something tried to look the
                # node up.
                eye_here = (pb[0], pb[1] + EYE_HEIGHT, pb[2])
                eye_them = (gp[0], gp[1] + EYE_HEIGHT, gp[2])
                through = False
                for op in portals:
                    if step_room not in op["rooms"] or grm not in op["rooms"]:
                        continue
                    poly = op["poly"]
                    for i in range(1, len(poly) - 1):
                        if _tri_hit(eye_here, eye_them, poly[0], poly[i], poly[i + 1]):
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


def _seg_hits(ax, az, bx, bz, cx, cz, dx, dz):
    """Do segments AB and CD cross? Standard orientation test, no tolerance games."""
    def o(px, pz, qx, qz, rx, rz):
        v = (qz - pz) * (rx - qx) - (qx - px) * (rz - qz)
        return 0 if v == 0 else (1 if v > 0 else 2)

    o1, o2 = o(ax, az, bx, bz, cx, cz), o(ax, az, bx, bz, dx, dz)
    o3, o4 = o(cx, cz, dx, dz, ax, az), o(cx, cz, dx, dz, bx, bz)
    return o1 != o2 and o3 != o4


def path_is_clear(walls, a, b, headroom=180.0):
    """Can you walk in a straight line from a to b without crossing a wall?

    Proximity is not walkability, and on Bunker 1 the difference is the whole bug: the nearest
    node to the spawn is 583 units away THROUGH A WALL, so joining the graph by distance alone
    produced an edge the bot spent 109 detours failing to cross.

    Walls arrive as triangles with a bbox. The test is done in plan view -- does the path cross
    any edge of the triangle projected onto XZ -- because a wall is vertical and its projection is
    what a walking body meets.

    The height filter is what stops this rejecting everything. Floors and ceilings are in the
    same triangle soup as walls, and a floor's projection covers the whole room, so without a
    vertical overlap test every path in the level reads as blocked. Only geometry spanning the
    walker's own band can stop them.
    """
    ax, ay, az = a
    bx, by, bz = b
    lo = min(ay, by) - 20.0
    hi = max(ay, by) + headroom

    for w in walls:
        bb = w.get("bbox")
        poly = w.get("poly")
        if not bb or not poly or len(poly) < 2:
            continue
        # Vertical band first: cheapest rejection, and the one that matters most.
        if bb[4] < lo or bb[1] > hi:
            continue
        # Then the plan-view bounding box of the path against the wall's.
        if max(ax, bx) < bb[0] or min(ax, bx) > bb[3]:
            continue
        if max(az, bz) < bb[2] or min(az, bz) > bb[5]:
            continue
        for i in range(len(poly)):
            p, q = poly[i], poly[(i + 1) % len(poly)]
            if _seg_hits(ax, az, bx, bz, p[0], p[2], q[0], q[2]):
                return False
    return True


def inject_spawn_node(know, graph, level, spawns, rooms_for_walls=None, links=3):
    """Add the measured spawn to the graph as a real node, joined to its nearest neighbours.

    The waypoint set is built from the level's PADS, and no pad marks where the player starts --
    so the graph does not cover the spawn, and on Bunker 1 it does not even cover the ROOM: the
    player stands in stan room 29 and no waypoint belongs to it. A bot therefore begins off the
    graph and has to cross unmapped floor before any route applies to it, which is not a routing
    problem the follower can solve with a heuristic.

    This adds the node the extractor could not know about, because the answer only exists at
    runtime. Nodes are given indices above every existing one so nothing renumbers.

    THE EDGES ARE AN ASSUMPTION AND ARE MARKED ONE. Proximity is not walkability: a node four
    hundred units away through a wall is nearer than one six hundred away down the corridor, and
    nothing here can tell them apart -- the same-room test that would is unavailable, since the
    stan room the player reports and the room ids the waypoints carry are different numbering
    spaces (Bunker 1: player in 29, waypoints in {2,4,...,30}). Several links are added rather
    than one so a blocked first choice is not the only choice.
    """
    rec = (spawns or {}).get(level)
    if not rec or not rec.get("pos"):
        return None

    waypoints = [w for w in know.get("waypoints", []) if w.get("pos")]
    if not waypoints:
        return None

    sx, sy, sz = rec["pos"]
    # Horizontal only: the spawn is a body position and waypoints sit on the floor plane, so
    # including the vertical picks a node on the wrong storey wherever geometry stacks.
    ranked = sorted(waypoints,
                    key=lambda w: (w["pos"][0] - sx) ** 2 + (w["pos"][2] - sz) ** 2)

    # Prefer neighbours the walls say are actually reachable. Falls back to raw proximity when
    # none are, so a level with no wall data still gets a connected spawn rather than an orphan.
    walls = (rooms_for_walls or {}).get("walls") or []
    clear = [w for w in ranked if path_is_clear(walls, [sx, sy, sz], w["pos"])] if walls else []
    chosen = clear[:links] if clear else ranked[:links]
    picked_by = "clear line" if clear else ("no clear line -- nearest" if walls else "no walls data")

    index = max(w["index"] for w in know["waypoints"]) + 1
    know["waypoints"].append({
        "index": index,
        "pad": None,
        "pos": [sx, sy, sz],
        "pad_name": "spawn",
        "synthetic": True,
        "edges_are_assumed": True,
    })

    graph.setdefault(index, [])
    for w in chosen:
        if w["index"] not in graph[index]:
            graph[index].append(w["index"])
        graph.setdefault(w["index"], [])
        if index not in graph[w["index"]]:
            graph[w["index"]].append(index)

    first = chosen[0]
    nearest = ((first["pos"][0] - sx) ** 2 + (first["pos"][2] - sz) ** 2) ** 0.5
    print("  %-10s spawn node %d -> node %d at %4.0f units (%s, %d of %d neighbours clear)"
          % (level, index, first["index"], nearest, picked_by, len(clear), len(ranked)))
    return index


def load_walkable_verdicts(out_dir, level):
    """The engine's verdicts from tools/validate_edges.sh, as a set of normalised pairs.

    Inline rather than a separate module, deliberately: a second implementation of this same
    rule is how the two would eventually drift apart. One mechanism, in the file that consumes it.

    The validator already resolves direction: a pair is recorded once, with ok true only when
    both directions agree, and the count that disagreed is in the header. So this just reads.
    """
    path = os.path.join(out_dir, level + ".walkable.json")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    out = set()
    for e in doc.get("edges", []):
        try:
            a, b = int(e["a"]), int(e["b"])
        except (KeyError, TypeError, ValueError):
            continue          # a malformed row is dropped, never guessed at
        if e.get("ok"):
            out.add((min(a, b), max(a, b)))
    return out


def prune_unwalkable(graph, measured):
    """Drop edges the engine refused.

    Only where the measurement actually looked. The validator has a distance cutoff, so an
    edge it never examined is absent from the set and must NOT be read as refused -- that would
    silently delete every long-range link in the graph and look like a successful prune.
    """
    covered = set()
    for a, b in measured:
        covered.add(a)
        covered.add(b)

    dropped = 0
    for a in list(graph.keys()):
        if a not in covered:
            continue
        keep = []
        for b in graph[a]:
            if b not in covered or (min(a, b), max(a, b)) in measured:
                keep.append(b)
            else:
                dropped += 1
        graph[a] = keep
    return dropped


def floor_under(rooms, x, z, y_hint=None):
    """Height of the floor beneath a point, from the extracted floor tiles, or None.

    gen_level_rooms.py emits waypoint_floor only for the nodes it creates. The spawn, door and
    portal nodes are added here, afterwards, so they have no entry -- and the height gate skips
    edges whose ends it cannot compare. That skipped precisely the synthetic edges, which are the
    ones most likely to be wrong: Train's route began spawn(y=334) -> waypoint 0(y=45), a
    289-unit drop through the floor of a carriage, and the gate let it through because the spawn
    had no recorded floor.

    Containment first, nearest centroid second. A point inside a tile has an exact answer; a
    point between tiles gets the nearest, which is right at a seam and wrong off the mesh -- and
    off the mesh there is no correct answer to give.
    """
    tiles = (rooms or {}).get("floors") or []
    if not tiles:
        return None

    best = None
    best_d = None
    for t in tiles:
        bb = t.get("bb")
        c = t.get("c")
        if not c:
            continue
        if bb and bb[0] <= x <= bb[2] and bb[1] <= z <= bb[3]:
            # Inside more than one tile happens where floors stack. Prefer the one nearest the
            # hint height, which is the surface the node actually belongs to.
            if y_hint is None:
                return c[1]
            d = abs(c[1] - y_hint)
            if best_d is None or d < best_d:
                best, best_d = c[1], d
    if best is not None:
        return best

    for t in tiles:
        c = t.get("c")
        if not c:
            continue
        d = ((c[0] - x) ** 2) + ((c[2] - z) ** 2)
        if best_d is None or d < best_d:
            best, best_d = c[1], d
    # Beyond this the nearest tile is not the floor under anything. Refuse rather than return a
    # height from across the level, which would make the gate confidently wrong.
    if best_d is not None and best_d > (400.0 ** 2):
        return None
    return best


def prune_by_height(graph, rooms, know, max_step=40.0):
    """Drop edges whose ends are more than one step apart in height.

    The graph has been planar since it was built: every node carries y and nothing routed on it,
    so a route could join a walkway to the floor beneath it as readily as to the next tile along.
    The line test cannot catch that -- it is a plan-view test, and a floor twelve feet down has
    an unobstructed line to the railing above it.

    waypoint_floor, from gen_level_rooms.py, is the height of the floor DIRECTLY BENEATH each
    node, which is the number this needs. Not the node's own y: a pad can sit above its floor, and
    comparing pad heights compares two things that are each some distance off the surface a body
    would stand on.

    Only prunes where BOTH ends have a floor recorded. A node with no floor beneath it is
    already the more interesting problem -- 33 to 240 per level cannot be stood on at all -- and
    treating "unknown" as "too steep" would delete the graph around exactly those places instead
    of leaving them visible.

    max_step is what a walking body climbs between adjacent nodes. Stairs are chains of small
    rises, so a real flight survives this and a railing-to-floor shortcut does not.
    """
    wf = (rooms or {}).get("waypoint_floor") or {}
    if not wf:
        return 0, 0

    dropped = unknown = 0
    for a in list(graph.keys()):
        fa = wf.get(str(a))
        keep = []
        for b in graph[a]:
            fb = wf.get(str(b))
            if fa is None or fb is None:
                unknown += 1
                keep.append(b)
                continue
            if abs(float(fa) - float(fb)) > max_step:
                dropped += 1
                continue
            keep.append(b)
        graph[a] = keep
    return dropped, unknown


def level_scales():
    """levelscale per level, parsed from the decomp so there is one copy of these constants.

    asset = runtime * levelscale. The measured spawns come out of the RUNNING GAME and are
    runtime; every JSON here is asset space. Train's spawn reads x=779 where its tile map ends at
    x=213, so an unconverted spawn starts the route outside the level and the nearest node is
    thousands of units away -- which the distance guard then correctly refuses, leaving no spawn
    at all. The failure looks like missing data and is a missing multiplication.
    """
    import re as _re
    path = os.path.join(ROOT, "vendor", "ge-decomp", "src", "game", "bg.c")
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    for m in _re.finditer(r'\{LEVELID_(\w+),\s*"[^"]+",\s*"[^"]+",\s*([0-9.]+),', src):
        out[m.group(1).lower()] = float(m.group(2))
    return out


# Prop kinds that stop a body. Doors are NOT here: a door is a passage, and removing its tile
# would seal every room the route needs to cross. Collectables, ammo and hats are walk-through
# pickups. Guards move and are the follower's problem, not the graph's.
BLOCKING_PROPS = ("StandardProp", "Glass", "TintedGlass", "Alarm", "Cctv",
                  "SingleMonitor", "MultiMonitor", "HangingMonitor", "Drone")


def subtract_props_from_tiles(know, rooms, graph):
    """Remove floor tiles a solid prop stands on.

    THE TILE MESH IS FLOOR GEOMETRY AND PROPS SIT ON TOP OF IT. A tile with a crate on it is
    still a tile, so the router happily plans through furniture and the follower walks into it --
    which is exactly what stopped the bot two waypoints into Train, with the navmesh insisting
    there was floor there and being right about the floor.

    Uses the prop's CENTRE against each tile's footprint, which needs no extents and no model
    scale. That is the honest version of what is available: a prop wider than its tile still
    overhangs its neighbours and this will not catch that.

    SO THIS IS A FLOOR, NOT A CEILING. It removes the tile a crate stands on, not the space a
    crate occupies. Real extents -- scaled and rotation-aware through obj->mtx -- would subtract
    the footprint properly, and that is a reasonable follow-up. This is the part that can be done
    correctly today rather than approximately.

    And it must not disconnect the level. A tile removed from a corridor one tile wide severs
    the route entirely, which is worse than routing through a crate -- so a removal that would
    orphan its neighbours is refused and counted.
    """
    floors = (rooms or {}).get("floors") or []
    props = know.get("props", []) or []
    if not floors or not props:
        return 0, 0

    tiles = {w["index"]: w for w in know.get("waypoints", []) if w.get("tile")}
    if not tiles:
        return 0, 0

    # tile index -> footprint, from the same floors list the nodes were built from.
    base = min(tiles) - min(f["t"] for f in floors if f.get("t") is not None)
    boxes = {}
    for f in floors:
        t = f.get("t")
        bb = f.get("bb")
        if t is None or not bb:
            continue
        boxes[base + int(t)] = bb

    blocked = set()
    for pr in props:
        if str(pr.get("type")) not in BLOCKING_PROPS or not pr.get("pos"):
            continue
        px, pz = pr["pos"][0], pr["pos"][2]
        for idx, bb in boxes.items():
            if bb[0] <= px <= bb[2] and bb[1] <= pz <= bb[3]:
                blocked.add(idx)
                break

    removed = kept = 0
    for idx in blocked:
        nbrs = graph.get(idx, [])
        # Refuse a removal that would strand a neighbour with nothing left. Severing a
        # one-tile-wide corridor is a worse outcome than planning through a crate, because the
        # follower can push past furniture and cannot cross a gap in the graph.
        strands = any(len([x for x in graph.get(n, []) if x != idx]) == 0 for n in nbrs)
        if strands:
            kept += 1
            continue
        for n in nbrs:
            graph[n] = [x for x in graph.get(n, []) if x != idx]
        graph[idx] = []
        removed += 1

    return removed, kept


def tile_graph_as_waypoints(know, rooms):
    """Replace the PAD waypoint set with the FLOOR TILE mesh.

    THIS IS THE FIX FOR THE THING THAT BLOCKED THE BOT ALL WEEK. Pads are prop markers, not
    places to walk: measured with the teleport probe, 139 of Train's 180 pads cannot be stood on,
    and across the twenty solo levels it runs 33 to 240 each. A follower handed targets a body
    cannot occupy is short by however far the pad sits off the floor, every time, and no amount of
    steering fixes it.

    Tiles are standable by construction -- they ARE the floor -- they carry a real height, and
    their adjacency is the engine's own, already wall-filtered with portal bridges to each room's
    largest component. Routing Train over them gives 117 of 117 steps monotonic along its axis
    against 0.8:1 for a level that is genuinely open, so the graph discriminates rather than
    merely existing.

    Pads keep doing what they are for: marking props, doors and spawns.

    Tile ids are the extractor's and pad indices are ours, and they overlap. Tiles are offset
    past the highest pad index so a route step can never be ambiguous about which set it names --
    two id spaces sharing a range is how a graph silently routes through the wrong nodes.
    """
    floors = (rooms or {}).get("floors") or []
    if not floors:
        return None, None

    pads = know.get("waypoints", []) or []
    base = (max((w["index"] for w in pads), default=-1)) + 1

    nodes, graph = [], {}
    for f in floors:
        tid = f.get("t")
        c = f.get("c")
        if tid is None or not c:
            continue
        idx = base + int(tid)
        nodes.append({
            "index": idx,
            "pad": None,
            # Tile centres are ASSET space, like everything else in rooms.json. The pack scales
            # at its own boundary, so these stay in asset space to match what pack_world expects.
            "pos": [float(c[0]), float(c[1]), float(c[2])],
            "pad_name": "tile_%d" % tid,
            # NOT marked synthetic. That flag means "rebuilt from scratch each run" and the strip
            # pass deletes everything carrying it -- which silently removed all 682 tiles a moment
            # after they were added, leaving no graph and no spawn, and printing nothing.
            "tile": True,
            "room": f.get("r"),
        })
        graph[idx] = [base + int(n) for n in (f.get("l") or [])]

    # Neighbours that name a tile we skipped would be dangling edges, and a router following one
    # gets a KeyError or, worse, silently drops the step.
    live = {n["index"] for n in nodes}
    for k in list(graph):
        graph[k] = [v for v in graph[k] if v in live]

    return nodes, graph


def inject_door_nodes(know, graph, max_link=200.0):
    """Put the level's doors in the graph, because that is how you get between rooms.

    The waypoint set comes from PADS and a door is a PROP, so doorways were missing from the
    graph entirely -- and a doorway is the single most consequential place in a level. The result
    is routes that cut from a node in one room to a node in another along a line no body can
    walk, and a bot that presses into the wall beside the door forever.

    Bunker 1 is the clean example. The spawn links to node 6, and there is a Door prop 42 units
    from node 6 at (-1267, 1741) -- the route runs THROUGH it, and nothing in the graph said so.
    The bot spent every run scraping the corridor wall 400 units short of the doorway.

    Each door becomes a node joined to its own nav_node -- the prop export already resolves that,
    with the distance -- plus any other waypoint close enough to be the far side of the same
    opening. That second link is what makes a door a passage rather than a dead end hanging off
    one room.

    Doors are not walls but they are not free either: a locked door still stops a bot, and
    nothing here knows which are locked. That shows up as a route that stalls at a real doorway,
    which is a much better failure than one that stalls at a blank wall -- and the follower
    presses the action button when it stalls.
    """
    waypoints = [w for w in know.get("waypoints", []) if w.get("pos")]
    if not waypoints:
        return 0

    doors = [pr for pr in know.get("props", [])
             if str(pr.get("type", "")).lower() == "door" and pr.get("pos")]
    if not doors:
        return 0

    next_index = max(w["index"] for w in know["waypoints"]) + 1
    added = 0

    for d in doors:
        dx, dy, dz = d["pos"]
        near = [w for w in waypoints
                if ((w["pos"][0] - dx) ** 2 + (w["pos"][2] - dz) ** 2) <= max_link * max_link]

        # The prop export already resolved the nearest nav node, so use it rather than
        # recomputing a slightly different answer from slightly different rounding.
        nav = d.get("nav_node")
        links = {w["index"] for w in near}
        if nav is not None:
            links.add(nav)
        if not links:
            continue

        know["waypoints"].append({
            "index": next_index,
            "pad": None,
            "pos": [dx, dy, dz],
            "pad_name": "door",
            "synthetic": True,
            "door": True,
            "needs_action": True,
        })
        graph.setdefault(next_index, [])
        for other in links:
            if other not in graph[next_index]:
                graph[next_index].append(other)
            graph.setdefault(other, [])
            if next_index not in graph[other]:
                graph[other].append(next_index)

        next_index += 1
        added += 1

    return added


def link_spawn_through_portals(know, graph, rooms, spawn_index, spawn_pos, spawn_room,
                               level="?", walls=None):
    """Connect the spawn room to the rest of the level through its actual doorways.

    THIS IS THE ONE THAT MATTERS. The waypoint set is built from PADS, and no pad sits in the
    room the player starts in -- Bunker 1 spawns in room 29 and the waypoint rooms are
    {2,4,...,30} with no 29 in them. So the bot is confined to its spawn room with no node in it,
    and every route begins somewhere it cannot walk to. Measured: 109 detours, closest approach
    426 against an arrive radius of 120, never leaving room 29.

    A straight line to the nearest node cannot fix that, and the wall test correctly refuses to
    pretend otherwise. What connects two rooms in this engine is a PORTAL, and rooms.json already
    carries them with their polygons -- a doorway is a real, walkable place, which is exactly the
    thing proximity cannot express.

    So: a node at each portal of the spawn room, joined to the spawn on one side and to the
    nearest waypoint in the room beyond on the other. That is a path a body can actually walk.

    Returns the number of portal nodes added.
    """
    if spawn_room is None or not rooms:
        return 0

    portals = rooms.get("portals") or []
    wr = rooms.get("waypoint_room") or {}
    by_index = {w["index"]: w for w in know.get("waypoints", []) if w.get("pos")}

    added = 0
    next_index = max(w["index"] for w in know["waypoints"]) + 1

    for por in portals:
        pair = por.get("rooms") or []
        poly = por.get("poly") or []
        if spawn_room not in pair or len(poly) < 3:
            continue

        other = pair[0] if pair[1] == spawn_room else pair[1]

        # The doorway itself, as a point a body can stand on. The polygon is a vertical quad, so
        # the centre of its FOOTPRINT is the walkable spot -- averaging the y as well would put
        # the node halfway up the door frame.
        cx = sum(pt[0] for pt in poly) / float(len(poly))
        cz = sum(pt[2] for pt in poly) / float(len(poly))
        cy = min(pt[1] for pt in poly)

        # Nearest waypoint on the far side. Without one the portal leads nowhere useful, and a
        # node with a single edge back to the spawn is just a dead end with extra steps.
        cands = [w for i, w in by_index.items() if wr.get(str(i)) == other]
        if not cands:
            continue
        target = min(cands, key=lambda w: (w["pos"][0] - cx) ** 2 + (w["pos"][2] - cz) ** 2)

        know["waypoints"].append({
            "index": next_index,
            "pad": None,
            "pos": [cx, cy, cz],
            "pad_name": "portal_%d" % por.get("portal", -1),
            "synthetic": True,
            "portal": por.get("portal"),
            "rooms": pair,
        })
        # VALIDATE BOTH ENDS. An unchecked link from the spawn to a portal is a straight line
        # across a room the spawn may not open onto, and a weighted router will happily take it
        # because it is one long cheap hop -- Bunker 1 chose spawn -> portal at 1342 units over
        # spawn -> door at 555 and then walked into the wall the long line crosses. Distance
        # weighting cannot rescue an edge that should not exist.
        graph.setdefault(next_index, [])
        for other_end in (spawn_index, target["index"]):
            end_pos = spawn_pos if other_end == spawn_index else target["pos"]
            if walls and end_pos and not path_is_clear(walls, [cx, cy, cz], end_pos):
                continue
            if other_end not in graph[next_index]:
                graph[next_index].append(other_end)
            graph.setdefault(other_end, [])
            if next_index not in graph[other_end]:
                graph[other_end].append(next_index)

        if not graph[next_index]:
            # A portal node joined to nothing is worse than no node: it enlarges the graph and
            # can never be routed through. Drop it and say so.
            know["waypoints"].pop()
            print("  %-10s portal %s dropped -- no clear line to either side"
                  % (level, por.get("portal")))
            continue

        next_index += 1
        added += 1

    return added


def measured_spawn_node(know, graph, level, spawns):
    """The graph node nearest to where the game ACTUALLY puts the player.

    spawn_node() below picks a well-connected hub and labels the result assumed, on the reasoning
    that only the first leg depends on it. That reasoning does not hold: a bot is not at the hub,
    it is at the spawn, so every leg of the route is measured from somewhere the bot has never
    been. Measured against the running game, all twenty levels were wrong, by 1,469 to 27,850
    units -- and the failure is silent, because a route from the wrong start is still perfectly
    well formed. The bot walks confidently into a wall.

    tools/dump_spawns.py produces the truth by booting each level and asking the game, which is
    the only authority: which start pad the engine picks, and where the stan query finally places
    the body, are runtime decisions that no amount of asset reading will settle.

    Returns None when there is no measurement for this level, so the caller falls back and keeps
    labelling the result assumed. A missing measurement must not silently become a confident one.
    """
    rec = (spawns or {}).get(level)
    if not rec or not rec.get("pos"):
        return None

    sx, _sy, sz = rec["pos"]
    best, best_d = None, None
    for w in know.get("waypoints", []):
        pos = w.get("pos")
        if not pos:
            continue
        # Horizontal distance only. Vertical disagreement between the spawn and the waypoint
        # plane is normal -- the spawn is an eye/body position and waypoints sit on the floor --
        # and including it picks a node on the wrong storey in any level with stacked geometry.
        d = ((pos[0] - sx) ** 2) + ((pos[2] - sz) ** 2)
        if best_d is None or d < best_d:
            best, best_d = w["index"], d

    if best is None:
        return None

    # A nearest node that is nowhere near the spawn means the graph does not cover where the
    # player starts, and Dam is exactly that case: its spawn is 27,850 units from the assumed
    # one and its waypoints are not in world coordinates at all. Snapping to the nearest node
    # would produce a confident route through the wrong space. Refuse instead, loudly.
    if best_d > (4000.0 ** 2):
        print("  %-10s SPAWN NOT IN GRAPH: nearest node %d is %.0f units away -- "
              "coordinate space mismatch, not a wrong pad. Falling back."
              % (level, best, best_d ** 0.5))
        return None

    return best


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
    ap.add_argument("--spawns", default=os.path.join("build", "levels", "spawns.json"),
                    help="measured spawns from tools/dump_spawns.py; absent means fall back to "
                         "the assumed hub and keep saying so")
    args = ap.parse_args()
    out_dir = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)

    spawn_path = args.spawns if os.path.isabs(args.spawns) else os.path.join(ROOT, args.spawns)
    measured = {}
    if os.path.exists(spawn_path):
        with open(spawn_path) as sf:
            measured = json.load(sf).get("spawns", {})
        print("using %d measured spawn(s) from %s" % (len(measured), args.spawns))
    else:
        print("no measured spawns at %s -- every start stays an assumption. "
              "Run tools/dump_spawns.py." % args.spawns)

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
        # Prefer a real node AT the spawn over the nearest node to it: starting the route where
        # the bot actually is removes the unmapped first leg entirely.
        # Positions for the weighted search, rebuilt AFTER every synthetic node is added --
        # a table built earlier would silently price the new nodes at the no-geometry penalty and
        # the router would avoid exactly the doorways this pass exists to add.
        # (assigned below, once the graph is final)

        # ROUTE ON TILES, NOT PADS, when the extractor has given us a mesh. Everything downstream
        # -- spawn injection, doors, portals, the height gate, the engine verdicts -- works on
        # whatever graph it is handed, so this is a swap rather than a rewrite.
        # Convert the measured spawn into ASSET space before it meets any JSON geometry.
        _sc = level_scales().get(level)
        if _sc and level in measured and measured[level].get("pos"):
            _p = measured[level]["pos"]
            measured = dict(measured)
            measured[level] = dict(measured[level],
                                   pos=[_p[0] * _sc, _p[1] * _sc, _p[2] * _sc],
                                   space="asset (converted from the measured runtime position)")

        _tnodes, _tgraph = tile_graph_as_waypoints(know, rooms)
        if _tnodes:
            # Pads are REPLACED, not appended. Keeping them as routing candidates leaves the
            # objective resolving to the nearest PAD -- which has no tile edges, so the route
            # comes out one step long and unwalkable, which is what happened the first time.
            # Props travel in the pack's own props section now, so nothing needs pads here.
            know["waypoints"] = _tnodes
            # Pad adjacency is dropped deliberately. Mixing the two graphs would let a route hop
            # from a walkable tile to a pad hanging in the air and back, which is exactly the
            # class of edge the tile mesh exists to remove.
            graph = dict(_tgraph)
            print("  %-10s routing on %d floor tiles (pads kept as markers only)"
                  % (level, len(_tnodes)))

        if _tnodes:
            _rm, _kept = subtract_props_from_tiles(know, rooms, graph)
            if _rm or _kept:
                print("  %-10s props block %d tile(s); %d kept to avoid severing the graph"
                      % (level, _rm, _kept))

        # STRIP ANY SYNTHETIC NODES FROM A PREVIOUS RUN FIRST.
        #
        # They are persisted back into the knowledge file so pack_world can see them, which means
        # a second run finds them already there and appends MORE on top -- and every index above
        # the first synthetic node shifts. That silently invalidates anything keyed to node ids,
        # which is exactly what the engine's edge verdicts are: the validator measured the spawn
        # as node 83 and the next run called the same place node 63, so every verdict pointed at
        # the wrong node and the pruning did nothing while reporting that it had.
        #
        # Rebuilding them from scratch every run makes the indices a pure function of the level
        # data, so verdicts stay valid as long as the level data has not changed.
        _before = len(know.get("waypoints", []))
        know["waypoints"] = [w for w in know.get("waypoints", []) if not w.get("synthetic")]
        if len(know["waypoints"]) != _before:
            _live = {w["index"] for w in know["waypoints"]}
            graph = {a: [b for b in bs if b in _live]
                     for a, bs in graph.items() if a in _live}

        # Doors BEFORE the spawn node, so the spawn can link to a doorway if that is genuinely
        # its nearest way out -- which on Bunker 1 it is.
        _doors = inject_door_nodes(know, graph)
        if _doors:
            print("  %-10s %d door node(s) added" % (level, _doors))

        start = inject_spawn_node(know, graph, level, measured, rooms)
        if start is not None:
            _rec = measured.get(level) or {}
            _n = link_spawn_through_portals(know, graph, rooms, start, _rec.get("pos"),
                                            _rec.get("room"), level,
                                            (rooms or {}).get("walls") or [])
            if _n:
                print("  %-10s %d portal node(s) out of spawn room %s"
                      % (level, _n, _rec.get("room")))
            else:
                # Loud, because a spawn room with no usable portal is the difference between a
                # route a bot can walk and one it cannot, and nothing downstream can tell.
                print("  %-10s NO PORTAL out of spawn room %s -- routes start walled in"
                      % (level, _rec.get("room")))
        if start is None:
            start = measured_spawn_node(know, graph, level, measured)
        start_measured = start is not None
        if start is None:
            start = spawn_node(know, graph)

        node_positions = {w["index"]: w["pos"]
                          for w in know.get("waypoints", []) if w.get("pos")}

        # Replace assumption with measurement, last, so it prunes the synthetic links too --
        # those are the ones proximity got wrong.
        # Uses the walkable_verdicts module rather than a second copy of the same logic here --
        # two implementations of one rule is how they drift apart.
        # Heights first: an edge that climbs a storey is wrong whatever the line test says, and
        # the line test is a plan-view test that cannot see it.
        # Give the synthetic nodes a floor before the gate runs, or it skips exactly the edges
        # this pass added -- which are the ones proximity got wrong.
        if rooms is not None:
            _wf = rooms.setdefault("waypoint_floor", {})
            _added = 0
            for _w in know.get("waypoints", []):
                if not _w.get("synthetic") or not _w.get("pos"):
                    continue
                if str(_w["index"]) in _wf:
                    continue
                _f = floor_under(rooms, _w["pos"][0], _w["pos"][2], _w["pos"][1])
                if _f is not None:
                    _wf[str(_w["index"])] = _f
                    _added += 1
            if _added:
                print("  %-10s floors computed for %d synthetic node(s)" % (level, _added))

        # MEASURED floors win over extracted ones. The engine placed a body at each node and
        # reported where it landed; the extraction has coverage holes exactly where the player
        # starts -- Train's spawn is outside its own floor bounding box -- so the offline number
        # is missing precisely where it is needed most.
        _wpath = os.path.join(out_dir, level + ".walkable.json")
        if rooms is not None and os.path.exists(_wpath):
            with open(_wpath, encoding="utf-8") as _fh:
                _nf = json.load(_fh).get("node_floor") or []
            if _nf:
                _wf = rooms.setdefault("waypoint_floor", {})
                for _e in _nf:
                    try:
                        _wf[str(int(_e["id"]))] = float(_e["floor"])
                    except (KeyError, TypeError, ValueError):
                        continue      # a malformed row is skipped, never guessed at
                print("  %-10s measured floors for %d node(s)" % (level, len(_nf)))

        _hdrop, _hunk = prune_by_height(graph, rooms, know)
        if _hdrop or _hunk:
            print("  %-10s heights: %d edge(s) too steep, %d with no floor recorded"
                  % (level, _hdrop, _hunk))

        _measured = load_walkable_verdicts(out_dir, level)
        if _measured is not None:
            _dropped = prune_unwalkable(graph, _measured)
            print("  %-10s engine verdicts: %d walkable pair(s), %d assumed edge(s) dropped"
                  % (level, len(_measured), _dropped))
        else:
            print("  %-10s NO ENGINE VERDICTS -- every edge is still an assumption. "
                  "Run tools/validate_edges.sh." % level)

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
                # Re-resolve the goal against the graph we are ACTUALLY routing on.
                #
                # nav_node comes from the prop export and names a PAD, because that is what
                # existed when it was computed. Routing on tiles leaves those ids pointing at
                # nodes the graph no longer contains, so every objective comes back unreachable
                # with no error -- the ids are valid integers, they simply name a different set.
                # An id space is only meaningful next to the graph it indexes.
                goal = p["nav_node"]
                if goal not in graph and p.get("pos"):
                    gx, gz = p["pos"][0], p["pos"][2]
                    best, bd = None, None
                    for _n, _pp in node_positions.items():
                        d = ((_pp[0] - gx) ** 2) + ((_pp[2] - gz) ** 2)
                        if bd is None or d < bd:
                            best, bd = _n, d
                    goal = best
                path = bfs(graph, here, goal, pos=node_positions) if here is not None else None
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
            "spawn_is_assumed": not start_measured,
            "counts": {"objectives": len(routes),
                       "routable": sum(1 for r in routes if r.get("routable"))},
            "routes": routes,
        }
        with open(os.path.join(out_dir, level + ".routes.json"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1)

        # Persist the synthetic nodes back into the knowledge file.
        #
        # The spawn and portal nodes were added to `know` in memory, and pack_world reads the
        # waypoint table from this file -- so without writing them back the pack carried 45
        # waypoints while the routes referenced 48. The follower looks each step's waypoint up by
        # id, does not find it, and RETURNS SILENTLY: no trace, no error, a bot that simply never
        # starts. That is a worse failure than a wrong route and it cost a full test cycle.
        #
        # gen_level_knowledge overwrites this file, which is fine: it runs before this tool and
        # this tool re-adds the nodes every time.
        if any(w.get("synthetic") for w in know.get("waypoints", [])):
            with open(kp, "w", encoding="utf-8") as fh:
                json.dump(know, fh, indent=1)

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
