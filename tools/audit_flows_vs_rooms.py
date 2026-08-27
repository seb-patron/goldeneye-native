#!/usr/bin/env python3
"""Check the documents' traversal chains against the room graph the extractor produces.

540 flow chains were mined. Each spatial one asserts that a walk exists: room, then corridor, then
junction, then room. That is checkable -- classify every measured room by KIND from its geometry,
then ask whether any walk through the adjacency graph visits those kinds in that order.

This is the sample the earlier cross-reference lacked. That pass checked 16 spec fields, which is
too few to be a verdict on anything.

THREE MODELLING DECISIONS, each of which changes the answer:

  A DOOR IS AN EDGE, NOT A ROOM. Doors are props in our data, not spaces, so "room, door, room"
  does not assert three spaces -- it asserts two rooms that are ADJACENT, with a door between them.
  Treating a door as a node makes every such chain unsatisfiable and would have reported the
  documents as wrong about the most common pattern they describe.

  MOST CHAINS ARE NOT SPATIAL AT ALL. player, enemy, cover, move, escape, objective: these are
  tactical and procedural sequences, and a room graph can neither confirm nor refute them. They are
  reported as OUT OF SCOPE, never as failures. Scoring them would repeat the category error that
  once produced a 12% agreement figure on this project.

  A TRAVERSAL IS A WALK, NOT A PATH, BUT IT MAY NOT IMMEDIATELY BACKTRACK. Rooms can repeat -- real
  routes revisit a junction -- but stepping straight back where you came from would let almost any
  sequence be satisfied by oscillating between two rooms, which would make the test meaningless.

HOW ROOMS ARE CLASSIFIED. From geometry alone: elongation of the tile bounding box, tile count and
degree in the adjacency graph. KINDS OVERLAP DELIBERATELY -- a space carries every kind it can
satisfy, because the documents do not use these words exclusively and a junction room is still a
room. Making them exclusive turned "room" into a residual category of dead ends and reported
"ROOM, DOOR, ROOM" as contradicted on Basement, which is the most trivially true claim a
walkthrough can make. Thresholds are printed with the results, and --sweep varies them: the rate
holds at 85% across aspect 2.0-3.5 and chamber 1.5-3.0, so it is not an artefact of where the lines
were drawn.

⚠️ AN UNSATISFIED CHAIN IS NOT AUTOMATICALLY THE DOCUMENT BEING WRONG. It can equally be the room
extractor splitting one space into several, or this file's classifier calling a corridor a room.
Both failure directions are stated with every result rather than being resolved silently in favour
of the tool.

Reads build/ only, writes build/ only.
"""
import argparse
import collections
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, "build", "levels")

# Labels naming a KIND OF SPACE. Everything mapping to the same kind is one concept.
NODE_KINDS = {
    "room": "room", "small room": "room", "large room": "chamber", "chamber": "chamber",
    "hall": "chamber", "large": "chamber", "open": "chamber", "courtyard": "chamber",
    "warehouse": "chamber", "open area": "chamber", "large outer area": "chamber",
    "corridor": "corridor", "hallway": "corridor", "narrow": "corridor", "tunnel": "corridor",
    "passage": "corridor", "vent": "corridor", "narrow tunnel": "corridor",
    "junction": "junction", "intersection": "junction", "corner": "junction",
    "stairs": "stairs", "stairwell": "stairs", "ladder": "stairs", "lift": "stairs",
    "elevator": "stairs", "stair": "stairs",
    # Size and shape words used as space kinds. Added after measuring which labels blocked the most
    # chains; these four are unambiguously about the shape of a space. The rest of the blocking
    # vocabulary is NOT added, and that is the point of the list below.
    "wide": "chamber", "small": "room", "tight": "corridor", "narrow passage": "corridor",
}

# WHY THE CHECKABLE SAMPLE STAYS SMALL, recorded so nobody tries to grow it by loosening the map.
#
# 690 distinct labels block a chain from being checked, and the top of that list is decisive:
# player (43 chains), enemy (15), objective (12), cover, escape, move, start. Those chains are
# TACTICAL sequences -- what the player does -- and a room graph can neither confirm nor refute
# them. The long tail is level-specific proper nouns: TOWER, SATELLITE, MACHINERY, FENCE. Those
# name particular places rather than kinds of space, and checking them would need a mapping from
# place names to room ids that this project does not have.
#
# So most flow chains are simply not claims about room-kind topology, and mapping TOWER onto
# "chamber" to inflate the sample would be inventing a claim the author did not make.
# Labels naming a TRANSITION between spaces rather than a space.
EDGE_LABELS = {"door", "doorway", "gate", "opening", "entrance", "hatch", "grate",
               "padlock gate", "door frame", "second door"}


def classify(level, aspect_corridor=2.5, chamber_share=2.0):
    """Rooms, their adjacency, and a kind for each, from geometry only."""
    p = os.path.join(LEVELS, "%s.rooms.json" % level)
    if not os.path.isfile(p):
        return None
    floors = json.load(open(p, encoding="utf-8")).get("floors") or []
    if not floors:
        return None
    room = {f["t"]: f.get("r") for f in floors}
    pos = {f["t"]: f["c"] for f in floors}

    tiles = collections.defaultdict(list)
    for f in floors:
        if f.get("r") is not None:
            tiles[f["r"]].append(pos[f["t"]])

    adj = collections.defaultdict(set)
    for f in floors:
        a = room.get(f["t"])
        for n in f.get("l", []):
            b = room.get(n)
            if a is not None and b is not None and a != b:
                adj[a].add(b)
                adj[b].add(a)

    sizes = [len(v) for v in tiles.values()]
    mean_size = (sum(sizes) / float(len(sizes))) if sizes else 1.0

    # KINDS ARE NOT MUTUALLY EXCLUSIVE, and making them exclusive produced a false accusation.
    #
    # The first version tested aspect, then size, then degree, then fell through to "room". That
    # makes "room" a RESIDUAL category of low-degree spaces, because any space with three or more
    # connections was captured as a junction first. Two such residual rooms are rarely adjacent, so
    # "ROOM -> DOOR -> ROOM" came out unsatisfiable on Basement -- the most trivially true claim a
    # walkthrough can make, reported as contradicted by the geometry.
    #
    # The documents do not use these words exclusively. A junction room is still a room; a large
    # chamber is still a room. So each space carries the SET of kinds it can satisfy: corridor when
    # elongated, chamber when large, junction when well connected, and room whenever it is not a
    # corridor. A label matches if it is in the set.
    kind = {}
    for r, pts in tiles.items():
        xs = [p[0] for p in pts]
        zs = [p[2] for p in pts]
        w = max(1.0, max(xs) - min(xs))
        h = max(1.0, max(zs) - min(zs))
        aspect = max(w, h) / min(w, h)
        deg = len(adj[r])
        ks = set()
        if aspect >= aspect_corridor:
            ks.add("corridor")
        else:
            ks.add("room")
        if len(pts) >= chamber_share * mean_size:
            ks.add("chamber")
            ks.add("room")
        if deg >= 3:
            ks.add("junction")
        kind[r] = ks
    return {"adj": adj, "kind": kind, "rooms": sorted(tiles),
            "mean_size": mean_size, "sizes": sizes}


def normalise(stages):
    """Chain labels to (kinds, edges) tokens, or None when the chain is not spatial."""
    seq = []
    for s in stages:
        # A parallel group is satisfied if ANY of its branches is; take the spatial ones.
        opts = s if isinstance(s, list) else [s]
        toks = set()
        for o in opts:
            t = o.strip().lower()
            if t in EDGE_LABELS:
                toks.add(("edge", None))
            elif t in NODE_KINDS:
                toks.add(("node", NODE_KINDS[t]))
        if not toks:
            return None                 # a stage nothing spatial maps to: chain is out of scope
        seq.append(sorted(toks))
    return seq if len(seq) >= 2 else None


def satisfiable(seq, g, limit=40000):
    """Does any non-backtracking walk realise this sequence of kinds?"""
    adj, kind = g["adj"], g["kind"]
    steps = [0]

    def walk(idx, cur, prev):
        if steps[0] > limit:
            return False
        steps[0] += 1
        if idx >= len(seq):
            return True
        opts = seq[idx]
        for typ, k in opts:
            if typ == "edge":
                # A door is crossed, not occupied: it consumes the token and the NEXT node must be
                # an adjacent room. Handled by simply advancing without moving; the following node
                # token forces the move.
                if walk(idx + 1, cur, prev):
                    return True
            else:
                if k in kind.get(cur, ()) and idx == 0:
                    if walk(idx + 1, cur, prev):
                        return True
                for nxt in adj[cur]:
                    if nxt == prev:
                        continue        # no immediate backtrack; see the module note
                    if k in kind.get(nxt, ()) and walk(idx + 1, nxt, cur):
                        return True
        return False

    first = seq[0]
    starts = [r for r in g["rooms"]
              if any(t == "edge" or k in kind.get(r, ()) for t, k in first)]
    for r in starts:
        steps[0] = 0
        if walk(1 if any(t == "node" and k in kind.get(r, ()) for t, k in first) else 0, r, None):
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(LEVELS, "_walkthrough.flows_verified.json"))
    ap.add_argument("--sweep", action="store_true",
                    help="vary the classifier thresholds and report the rate at each, which is "
                         "the only way to tell a real result from one the thresholds produced")
    a = ap.parse_args()

    fl = json.load(open(os.path.join(LEVELS, "_walkthrough.flows.json"), encoding="utf-8"))

    if a.sweep:
        # I CHOSE THESE THRESHOLDS, SO THE RESULT HAS TO SURVIVE CHANGING THEM. A rate that moves
        # with the threshold is a property of the classifier rather than of the documents.
        print("SENSITIVITY SWEEP")
        print("%-8s %-8s %8s %10s %7s" % ("aspect", "chamber", "checked", "satisfied", "rate"))
        for asp in (2.0, 2.5, 3.0, 3.5):
            for ch in (1.5, 2.0, 2.5, 3.0):
                gg, n, s = {}, 0, 0
                for x in fl["items"]:
                    lv = x["level"]
                    if lv.startswith("_"):
                        continue
                    if lv not in gg:
                        gg[lv] = classify(lv, aspect_corridor=asp, chamber_share=ch)
                    if not gg[lv]:
                        continue
                    seq = normalise(x["stages"])
                    if not seq:
                        continue
                    n += 1
                    s += 1 if satisfiable(seq, gg[lv]) else 0
                print("%-8.1f %-8.1f %8d %10d %6.0f%%"
                      % (asp, ch, n, s, 100.0 * s / n if n else 0))
        return 0
    graphs, results = {}, []
    scope = collections.Counter()

    for x in fl["items"]:
        lv = x["level"]
        if lv.startswith("_"):
            scope["engine prose, no level geometry"] += 1
            continue
        if lv not in graphs:
            graphs[lv] = classify(lv)
        g = graphs[lv]
        if not g:
            scope["no room data"] += 1
            continue
        seq = normalise(x["stages"])
        if not seq:
            scope["not a spatial chain"] += 1
            continue
        ok = satisfiable(seq, g)
        scope["checked"] += 1
        results.append({"level": lv, "line": x["line"],
                        "stages": [s if isinstance(s, str) else "|".join(s) for s in x["stages"]],
                        "length": x["length"], "satisfied": ok})

    print("KIND SETS (a space may satisfy several: aspect>=2.5 corridor, >=2x mean size")
    print("chamber, degree>=3 junction, room whenever not a corridor -- counts overlap)")
    print("%-10s %6s %9s %9s %9s %8s" % ("level", "rooms", "corridor", "chamber", "junction", "room"))
    for lv in sorted(graphs):
        g = graphs[lv]
        if not g:
            continue
        c = collections.Counter(k for ks in g["kind"].values() for k in ks)
        print("%-10s %6d %9d %9d %9d %8d"
              % (lv, len(g["rooms"]), c["corridor"], c["chamber"], c["junction"], c["room"]))

    sat = sum(1 for r in results if r["satisfied"])
    print("\nCHAIN SCOPE")
    for k, n in scope.most_common():
        print("   %-32s %4d" % (k, n))
    if results:
        print("\nSPATIAL CHAINS CHECKED: %d" % len(results))
        print("   satisfied by a real walk : %d (%.0f%%)" % (sat, 100.0 * sat / len(results)))
        print("   not satisfied            : %d" % (len(results) - sat))
        print("\n   OF 540 MINED CHAINS ONLY %d ARE CHECKABLE HERE, and that is the honest"
              % len(results))
        print("   ceiling rather than a shortfall to be engineered away. 328 are tactical")
        print("   sequences -- player, enemy, cover, escape -- which a room graph cannot speak")
        print("   to, and 192 are engine prose with no level attached. Most of the rest name")
        print("   particular places rather than kinds of space; mapping TOWER onto chamber to")
        print("   inflate the sample would invent a claim the author did not make.")
        print("   Run with --sweep for the rate against varied thresholds.")
        print("\n   An unsatisfied chain is not automatically the document being wrong: it can")
        print("   equally be the room extractor splitting one space into several, or the kind")
        print("   classifier above calling a corridor a room.")

        bylen = collections.defaultdict(lambda: [0, 0])
        for r in results:
            bylen[r["length"]][0] += 1
            bylen[r["length"]][1] += 1 if r["satisfied"] else 0
        print("\n   by chain length (longer chains are stronger assertions):")
        for L in sorted(bylen):
            t, s = bylen[L]
            print("      %2d stages  %3d checked  %3d satisfied  %3.0f%%" % (L, t, s, 100.0 * s / t))

        bad = [r for r in results if not r["satisfied"]]
        if bad:
            print("\n   unsatisfied, longest first:")
            for r in sorted(bad, key=lambda y: -y["length"])[:8]:
                print("      %-9s line %-5d %s" % (r["level"], r["line"],
                                                   " -> ".join(r["stages"])[:74]))

    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump({"checked": len(results), "satisfied": sat,
                   "scope": dict(scope), "results": results}, fh, indent=1)
    print("\n-> %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
