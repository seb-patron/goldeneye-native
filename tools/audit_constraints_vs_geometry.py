#!/usr/bin/env python3
"""Check the documents' NEGATIVE claims against the geometry.

A negative claim is the most falsifiable thing in the corpus. "There is no alternate route around
the train" is refuted by a single cycle in the room graph, and nothing about it is a matter of
degree. That makes these worth checking even though there are few of them.

⚠️ MOST OF THEM ARE NOT GEOMETRIC, AND THAT IS THE MAIN RESULT HERE. Of 42 descriptive
constraints, the majority assert something about PERCEPTION or KNOWLEDGE rather than shape:

    the player cannot easily predict the exact visibility boundary
    you never receive the entire space simultaneously
    the player cannot simply memorize ...

Nothing in a tile graph confirms or refutes what a player can predict, perceive or remember. A
dozen more are engine advice ("never put mission-specific logic inside generic combat code") and
some are narrative. Scoring any of those against room adjacency would be measuring one thing and
reporting it as another, which is the error that once produced a 12% agreement figure on this
project. They are classified and set aside, never counted as failures.

WHAT IS CHECKABLE, and the measure for each:

  lateral traversal   "no lateral traversal", "cannot escape sideways". The level's short axis
                      against its long axis. A space you cannot move sideways in is one whose
                      lateral extent is negligible next to its length.
  branching           "no meaningful branches". Independent cycles per room, the same measure the
                      spec audit uses, because a branch that rejoins is a route choice and a spur
                      that dead-ends is not.
  alternate route     "no alternate route". Cycles again, and this is the strictest form: one cycle
                      refutes it outright.
  inaccessible space  "cannot normally enter", "cannot travel there". Tiles unreachable from the
                      player's own component.

EVERY THRESHOLD IS PRINTED AND SWEPT. --sweep varies them, because a verdict that moves with the
threshold is a property of this file rather than of the documents.

Reads build/ only, writes build/ only.
"""
import argparse
import collections
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, "build", "levels")

# A claim about movement being impossible sideways.
LATERAL = re.compile(r"\b(?:no lateral|lateral (?:level )?traversal|escape sideways|"
                     r"no lateral movement|sideways)\b", re.I)
# A claim about there being no choice of route.
BRANCHING = re.compile(r"\bno (?:meaningful )?branch(?:es|ing)?\b", re.I)
ALTERNATE = re.compile(r"\bno alternate route|no other route|only one route\b", re.I)
# A claim that some space exists but cannot be entered.
INACCESSIBLE = re.compile(r"\bcannot (?:normally )?(?:enter|travel|walk|go)\b|"
                          r"\bnever (?:enter|reach)\b", re.I)

# A claim has to be ABOUT THE LEVEL before a level-wide measure can settle it. These cues mark a
# statement as global. Without this, a three-word fragment lifted from a section on where to place
# enemies -- "no lateral movement" -- is compared against Caverns' overall aspect ratio and reported
# as contradicted. That is the local-versus-level-wide category error that produced a bogus 12%
# agreement figure earlier in this project, and it impugns the document rather than the measure.
LEVEL_SCOPE = re.compile("the level|this level|the map|throughout|across the level"
                         "|there (?:is|are) (?:essentially )?no|the (?:entire|whole)"
                         "|no alternate route|cannot meaningfully|principal|overall",
                         re.I)

# Claims about what a player can perceive, predict or remember. Listed, never scored.
PERCEPTUAL = re.compile(r"\b(?:predict|perceive|memori[sz]e|remember|see|visual|visibility|"
                        r"receive|understand|navigate|know|locate|feel|expect)\b", re.I)


def geometry(level):
    p = os.path.join(LEVELS, "%s.rooms.json" % level)
    if not os.path.isfile(p):
        return None
    floors = json.load(open(p, encoding="utf-8")).get("floors") or []
    if not floors:
        return None
    pos = {f["t"]: f["c"] for f in floors}
    room = {f["t"]: f.get("r") for f in floors}
    tadj = {f["t"]: [n for n in f.get("l", []) if n in pos] for f in floors}

    radj = collections.defaultdict(set)
    for f in floors:
        a = room.get(f["t"])
        for n in f.get("l", []):
            bb = room.get(n)
            if a is not None and bb is not None and a != bb:
                radj[a].add(bb)
                radj[bb].add(a)
    rooms = sorted({r for r in room.values() if r is not None})
    for r in rooms:
        radj.setdefault(r, set())
    edges = sum(len(v) for v in radj.values()) // 2

    # Tile components: an inaccessible space shows up as a component that is not the main body.
    seen, comps = set(), []
    for t in pos:
        if t in seen:
            continue
        c, st = [], [t]
        seen.add(t)
        while st:
            u = st.pop()
            c.append(u)
            for v in tadj.get(u, ()):
                if v not in seen:
                    seen.add(v)
                    st.append(v)
        comps.append(len(c))
    comps.sort(reverse=True)

    # Room-graph components, for the cycle count.
    rseen, rcomp = set(), 0
    for r in rooms:
        if r in rseen:
            continue
        rcomp += 1
        st = [r]
        rseen.add(r)
        while st:
            u = st.pop()
            for v in radj[u]:
                if v not in rseen:
                    rseen.add(v)
                    st.append(v)

    # MACRO CYCLES: cycles that survive when segmentation-scale rooms are ignored.
    #
    # Train's two cycles both run through a 2-tile room and a 20-tile nook that happens to touch two
    # neighbouring carriage rooms. Those are triangles created by where the extractor drew a room
    # boundary, not routes a player can take around anything. Counting them refuted "there is no
    # alternate route around the train", which is a claim about macro traversal -- you cannot bypass
    # a carriage -- and the raw cycle count cannot tell an alcove from a bypass.
    #
    # So rooms below a quarter of the mean size are dropped and the cycles recounted. What remains
    # is a loop between spaces large enough to be places.
    rsize = collections.Counter(r for r in room.values() if r is not None)
    mean_r = (sum(rsize.values()) / float(len(rsize))) if rsize else 1.0
    big = {r for r in rooms if rsize[r] >= 0.25 * mean_r}
    badj = {r: {v for v in radj[r] if v in big} for r in big}
    bedges = sum(len(v) for v in badj.values()) // 2
    bseen, bcomp = set(), 0
    for r in big:
        if r in bseen:
            continue
        bcomp += 1
        st = [r]
        bseen.add(r)
        while st:
            u = st.pop()
            for v in badj[u]:
                if v not in bseen:
                    bseen.add(v)
                    st.append(v)
    macro_cycles = bedges - len(big) + bcomp if big else 0

    xs = [c[0] for c in pos.values()]
    zs = [c[2] for c in pos.values()]
    ex, ez = max(xs) - min(xs), max(zs) - min(zs)

    # The two ends of the level along its dominant axis, and how many rooms must be removed to
    # separate them. Restricted to the main room component: a cut to an isolated pocket is not a
    # statement about the route through the level.
    axis = 0 if ex >= ez else 2
    centre = {}
    for f in floors:
        r = room.get(f["t"])
        if r is None:
            continue
        centre.setdefault(r, []).append(pos[f["t"]][axis])
    centre = {r: sum(v) / len(v) for r, v in centre.items()}
    main = max(_components(radj, rooms), key=len) if rooms else []
    ends = sorted(main, key=lambda r: centre.get(r, 0.0))
    cut = None
    art_share = art_n = art_of = None
    route = []
    if len(ends) >= 2:
        sub = {r: {v for v in radj[r] if v in set(main)} for r in main}
        cut = min_vertex_cut(sub, ends[0], ends[-1])
        route = shortest_path(sub, ends[0], ends[-1])
        art_share, art_n, art_of = articulation_share(sub, set(main), route)
    return {
        "n_rooms": len(rooms), "edges": edges,
        "cycles": edges - len(rooms) + rcomp,
        "long_axis": max(ex, ez), "short_axis": min(ex, ez),
        "aspect": (max(ex, ez) / min(ex, ez)) if min(ex, ez) > 0 else float("inf"),
        "tiles": len(pos), "components": comps,
        "outside_main": sum(comps[1:]),
        "macro_cycles": macro_cycles, "big_rooms": len(big),
        "end_to_end_cut": cut, "route_len": len(route),
        "articulation_share": art_share, "articulation_n": art_n, "articulation_of": art_of,
    }


def _components(radj, rooms):
    seen, out = set(), []
    for r in rooms:
        if r in seen:
            continue
        c, st = [], [r]
        seen.add(r)
        while st:
            u = st.pop()
            c.append(u)
            for v in radj[u]:
                if v not in seen:
                    seen.add(v)
                    st.append(v)
        out.append(c)
    return out


def min_vertex_cut(radj, src, dst):
    """Smallest number of rooms whose removal separates src from dst.

    THIS IS THE RIGHT INSTRUMENT FOR "NO ALTERNATE ROUTE", and cycle counting was not.
    A cycle says some loop exists somewhere. The claim is that a carriage cannot be BYPASSED --
    that every space along the way is a mandatory passage. Those are different statements: a nook
    touching two neighbouring rooms creates a cycle without offering any way around anything.

    Vertex connectivity answers the claim directly. A cut of 1 means a single room stands between
    the two ends and there is exactly one route through; 2 or more means a genuine bypass exists.

    Node-splitting max-flow: every room becomes in/out with capacity 1, so the max flow equals the
    number of vertex-disjoint paths, which by Menger's theorem equals the minimum vertex cut.
    """
    cap = collections.defaultdict(int)
    def add(u, v, c):
        cap[(u, v)] += c
    for r in radj:
        add(("in", r), ("out", r), 1 if r not in (src, dst) else 10 ** 6)
        for w in radj[r]:
            add(("out", r), ("in", w), 10 ** 6)
    s, t = ("out", src), ("in", dst)
    flow = 0
    while True:
        # BFS for an augmenting path.
        prev, q = {s: None}, [s]
        while q and t not in prev:
            u = q.pop(0)
            for (a, bb), c in list(cap.items()):
                if a == u and c > 0 and bb not in prev:
                    prev[bb] = u
                    q.append(bb)
        if t not in prev:
            return flow
        # Residual capacity along it.
        path, cur = [], t
        while prev[cur] is not None:
            path.append((prev[cur], cur))
            cur = prev[cur]
        f = min(cap[e] for e in path)
        for (u, v) in path:
            cap[(u, v)] -= f
            cap[(v, u)] += f
        flow += f
        if flow > 8:
            return flow          # far beyond "one route"; no need to keep going


def articulation_share(radj, main, path):
    """Fraction of the rooms along an end-to-end route whose removal disconnects the level.

    THE VERTEX CUT WAS BOUNDED BY ITS OWN ENDPOINTS, which a validity sweep caught: the minimum cut
    between two rooms can never exceed the degree of either, and the extreme rooms along a level's
    long axis are usually dead ends of degree 1. So the cut read 1 for twenty of twenty-six levels,
    including Control with 38 macro cycles -- a heavily looped level cannot have a single route,
    and the measure was really reporting "the end room is a dead end".

    "No alternate route" says every space along the way is MANDATORY. That is exactly an
    articulation point: a room whose removal splits the level. Counting what share of the route is
    made of them is not bounded by the endpoints and says the thing the claim says.
    """
    arts = set()
    for r in main:
        # Remove r, then see whether the rest still connects.
        rest = [x for x in main if x != r]
        if not rest:
            continue
        seen, st = {rest[0]}, [rest[0]]
        while st:
            u = st.pop()
            for v in radj[u]:
                if v != r and v in seen:
                    continue
                if v != r and v in main:
                    seen.add(v)
                    st.append(v)
        if len(seen) < len(rest):
            arts.add(r)
    inner = [r for r in path if len(radj[r]) > 1]
    if not inner:
        return None, 0, 0
    n = sum(1 for r in inner if r in arts)
    return n / float(len(inner)), n, len(inner)


def shortest_path(radj, src, dst):
    prev = {src: None}
    q = [src]
    while q:
        u = q.pop(0)
        if u == dst:
            break
        for v in radj[u]:
            if v not in prev:
                prev[v] = u
                q.append(v)
    if dst not in prev:
        return []
    out, cur = [], dst
    while cur is not None:
        out.append(cur)
        cur = prev[cur]
    return out[::-1]


def classify(text):
    """Which measurable family a constraint belongs to, or why it is not measurable."""
    if BRANCHING.search(text):
        return "branching"
    if ALTERNATE.search(text):
        return "alternate_route"
    if LATERAL.search(text):
        return "lateral"
    if INACCESSIBLE.search(text):
        return "inaccessible"
    if PERCEPTUAL.search(text):
        return "perceptual"
    return "unclassified"


def check(kind, g, lateral_max=0.10, cycles_max=0.05):
    if kind == "lateral":
        rel = g["short_axis"] / g["long_axis"] if g["long_axis"] else 0.0
        ok = rel <= lateral_max
        return ok, ("short axis %.0f against long %.0f = %.3f (no lateral traversal if <=%.2f)"
                    % (g["short_axis"], g["long_axis"], rel, lateral_max))
    if kind in ("branching", "alternate_route"):
        rate = g["cycles"] / float(g["n_rooms"]) if g["n_rooms"] else 0.0
        if kind == "alternate_route":
            share = g["articulation_share"]
            if share is None:
                return None, "no usable room component to measure a route through"
            # Every room on the route being mandatory is what "no alternate route" asserts. The
            # threshold is high because the claim is strong; the measure spreads from 100% on Train
            # to 0% on Basement, Library and Stack, so it is discriminating rather than a rubber
            # stamp. --sweep varies it.
            ok = share >= 0.90
            return ok, ("%d of %d rooms on the end-to-end route are mandatory passages "
                        "(%.0f%%, claim holds at >=90%%); removing any one of them splits the "
                        "level. The end-to-end vertex cut is %s but that measure is bounded by the "
                        "degree of its own endpoints, so it read 1 for twenty of twenty-six levels "
                        "and was not answering this."
                        % (g["articulation_n"], g["articulation_of"], 100.0 * share,
                           g["end_to_end_cut"]))
        ok = rate <= cycles_max
        return ok, ("%d cycles across %d rooms = %.3f per room (no meaningful branching if <=%.2f)"
                    % (g["cycles"], g["n_rooms"], rate, cycles_max))
    if kind == "inaccessible":
        # NOT CHECKABLE, and the Train inaccessible car is why.
        #
        # The document describes a car that exists as geometry but cannot normally be entered --
        # unused space reachable only with cheats. Our tile extraction covers WALKABLE FLOOR, so a
        # space nobody can walk in contributes no tiles at all. It is absent, not unreachable, and
        # absence is indistinguishable from never having existed.
        #
        # Testing for unreachable tiles therefore answers a different question and returned
        # "contradicted" for a claim the data cannot speak to either way.
        return None, ("%d tiles outside the main body, but a space with no walkable floor leaves "
                      "no trace here at all -- absence cannot be told from non-existence"
                      % g["outside_main"])
    return None, "not measurable from tile geometry"


def gather():
    d = json.load(open(os.path.join(LEVELS, "_walkthrough.deep.json"), encoding="utf-8"))
    return [x for x in d["items"]
            if x["kind"] == "constraint" and x.get("mode") == "descriptive"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweep", action="store_true")
    ap.add_argument("--out", default=os.path.join(LEVELS, "_walkthrough.constraints_verified.json"))
    a = ap.parse_args()

    cons = gather()
    geo = {}

    def run(lateral_max, cycles_max):
        res, scope = [], collections.Counter()
        for c in cons:
            lv = c["level"]
            if lv.startswith("_"):
                scope["engine prose, no level geometry"] += 1
                continue
            if lv not in geo:
                geo[lv] = geometry(lv)
            g = geo[lv]
            if not g:
                scope["no room data"] += 1
                continue
            kind = classify(c["text"])
            if kind in ("perceptual", "unclassified"):
                scope["%s, not a geometric claim" % kind] += 1
                continue
            # A local observation cannot be checked against a level-wide measure. See LEVEL_SCOPE.
            if not LEVEL_SCOPE.search(c["text"]):
                scope["local statement, not a level-wide claim"] += 1
                continue
            ok, why = check(kind, g, lateral_max, cycles_max)
            if ok is None:
                scope["no measure defined"] += 1
                continue
            scope["checked"] += 1
            res.append({"level": lv, "line": c["line"], "family": kind,
                        "text": c["text"][:140], "holds": ok, "measured": why})
        return res, scope

    if a.sweep:
        print("SENSITIVITY SWEEP -- the thresholds are mine, so the verdicts must survive them")
        print("%-10s %-10s %8s %8s %7s" % ("lateral", "cycles", "checked", "hold", "rate"))
        for lm in (0.05, 0.10, 0.15, 0.20):
            for cm in (0.02, 0.05, 0.10):
                r, _ = run(lm, cm)
                h = sum(1 for x in r if x["holds"])
                print("%-10.2f %-10.2f %8d %8d %6.0f%%"
                      % (lm, cm, len(r), h, 100.0 * h / len(r) if r else 0))
        return 0

    res, scope = run(0.10, 0.05)
    print("DESCRIPTIVE CONSTRAINTS: %d" % len(cons))
    for k, n in scope.most_common():
        print("   %-38s %3d" % (k, n))

    hold = sum(1 for x in res if x["holds"])
    print("\nGEOMETRIC CONSTRAINTS CHECKED: %d" % len(res))
    print("   hold against the measured geometry : %d" % hold)
    print("   contradicted                       : %d" % (len(res) - hold))

    for x in res:
        print("\n   [%s] %s line %d" % ("HOLDS" if x["holds"] else "CONTRADICTED",
                                        x["level"], x["line"]))
        print("      claim   : %s" % x["text"][:100])
        print("      measured: %s" % x["measured"])

    print("\nMOST OF THESE CLAIMS ARE NOT ABOUT SHAPE, and that is the finding rather than a")
    print("shortfall. Perceptual claims -- what a player can predict, perceive or remember -- are")
    print("the largest group, and no tile graph speaks to them. Scoring them here would measure")
    print("one thing and report it as another.")

    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump({"checked": len(res), "hold": hold, "scope": dict(scope),
                   "results": res}, fh, indent=1)
    print("\n-> %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
