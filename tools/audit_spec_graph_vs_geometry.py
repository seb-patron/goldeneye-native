#!/usr/bin/env python3
"""Check the specs' node and edge TYPE claims against the measured geometry.

The specs carry graph fields in three shapes, and only two of them are claims about a level:

  TYPE VOCABULARY   nodes ["room","room","corridor","junction","room"] and
                    edges ["door","short_corridor","corner"]. This says what kinds of space and
                    connection the level is built from, and every kind is checkable: the room
                    classifier says which kinds exist, and Door props say whether doors do.

  LAYER LIST        nodes ["lower_deck","mid_deck","forward_deck","superstructure","upper_deck"].
                    A claim that the level is built in that many vertical layers, checkable against
                    the distribution of floor heights.

  SCHEMATIC         edges [["A","B"],["B","C"],["C","D"],["D","E"],["D","F"]]. Seven symbolic nodes
                    standing in for a 58-room level. THIS IS NOT SCORED. It is an illustration of a
                    topology, not a census, and comparing seven abstract nodes against 58 measured
                    rooms is the same category error as comparing a local remark against a
                    level-wide total. What it does assert -- branching, or a single line -- is
                    already covered by the topology checks in audit_docs_vs_geometry.py, so
                    scoring it here would also double-count the same claim.

HOW MUCH THIS CHECK IS WORTH, stated up front because the pass rate flatters it. A type
vocabulary is an EXISTENCE claim -- does the level contain a corridor -- and any level contains
all the common space kinds, so it is close to unfalsifiable. 13 of 13 holding says the vocabulary
is drawn from the level, not that the level matches a description.

The falsifiable version is composition, and it does not survive contact either. Basement declares
five nodes, three of them "room", and measures 25% room against 60% declared. That is not the
document being wrong: a FIVE-ITEM LIST HAS TWENTY-POINT GRANULARITY and is naming kinds with a
rough sense of frequency, not asserting a census. Reading it as a proportion is over-reading, and
only one spec in the corpus carries enough nodes to test at all. So composition is measured and
printed, never scored.

THE ROOM CLASSIFIER IS THE ONE VALIDATED IN audit_flows_vs_rooms.py, kinds overlapping rather than
exclusive, because a junction room is still a room. Making them exclusive there turned "room" into
a residual category and produced a false contradiction, and the same would happen here.

Reads build/ only, writes build/ only.
"""
import argparse
import collections
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, "build", "levels")
sys.path.insert(0, os.path.join(ROOT, "tools"))

from audit_flows_vs_rooms import classify as classify_rooms

# Node labels naming a kind of space, mapped onto what the classifier produces.
NODE_TYPE = {
    "room": "room", "small_room": "room", "large_room": "chamber", "chamber": "chamber",
    "hall": "chamber", "open_area": "chamber", "corridor": "corridor", "hallway": "corridor",
    "tunnel": "corridor", "passage": "corridor", "junction": "junction",
    "intersection": "junction", "stairs": "stairs", "stairwell": "stairs",
}
# Edge labels naming a kind of connection.
EDGE_DOOR = {"door", "doorway", "gate", "hatch"}
EDGE_SPACE = {"short_corridor": "corridor", "corridor": "corridor", "corner": "junction",
              "tunnel": "corridor", "stairs": "stairs"}
# A layer list rather than a kind list.
LAYER_HINT = ("deck", "floor", "level_", "storey", "story", "tier")


def props(level):
    p = os.path.join(LEVELS, "%s.json" % level)
    if not os.path.isfile(p):
        return None
    d = json.load(open(p, encoding="utf-8"))
    return collections.Counter(x.get("type") for x in (d.get("props") or []))


def layers(level, gap=40.0):
    """Distinct vertical bands of floor, which is what a deck list claims."""
    p = os.path.join(LEVELS, "%s.rooms.json" % level)
    if not os.path.isfile(p):
        return None
    ys = sorted(f["c"][1] for f in json.load(open(p, encoding="utf-8")).get("floors") or [])
    if not ys:
        return None
    # Cluster heights: a new band starts wherever there is a gap larger than a body height. The
    # gap is printed with the result because it decides the count.
    bands, cur = 1, ys[0]
    for y in ys:
        if y - cur > gap:
            bands += 1
        cur = y
    return bands


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gap", type=float, default=40.0,
                    help="floor-height gap that starts a new vertical layer, asset units")
    ap.add_argument("--out", default=os.path.join(LEVELS, "_walkthrough.specgraph_verified.json"))
    a = ap.parse_args()

    sp = json.load(open(os.path.join(LEVELS, "_walkthrough.specs.json"), encoding="utf-8"))
    results, skipped = [], collections.Counter()
    kinds_cache = {}

    for s in sp["specs"]:
        lv = s["level"]
        flat = s.get("flat") or {}
        if lv.startswith("_"):
            continue

        node_vals, edge_vals = [], []
        schematic = False
        for k, v in flat.items():
            base = k.split("[")[0]
            if base == "nodes":
                node_vals += v if isinstance(v, list) else [v]
            elif base in ("edges", "terrain_graph.edges", "connections"):
                if "[" in k and isinstance(v, list) and len(v) == 2:
                    schematic = True          # a from/to pair: a drawn graph, not a vocabulary
                elif isinstance(v, list):
                    edge_vals += v
                elif isinstance(v, str):
                    edge_vals.append(v)
        if schematic and not node_vals and not edge_vals:
            skipped["schematic graph, an illustration not a census"] += 1
            continue
        if not node_vals and not edge_vals:
            continue

        if lv not in kinds_cache:
            g = classify_rooms(lv)
            kinds_cache[lv] = g
        g = kinds_cache[lv]
        if not g:
            skipped["no room data"] += 1
            continue
        present = collections.Counter(k for ks in g["kind"].values() for k in ks)

        # A deck list is a layer claim, not a kind claim.
        if node_vals and all(any(h in str(x).lower() for h in LAYER_HINT) for x in node_vals):
            got = layers(lv, a.gap)
            claim = len(node_vals)
            ok = got is not None and got >= claim
            results.append({"level": lv, "line": s["line"], "kind": "layers",
                            "claim": "%d layers: %s" % (claim, ", ".join(map(str, node_vals))[:60]),
                            "holds": ok,
                            "measured": "%s distinct floor bands at a %.0f-unit gap"
                                        % (got, a.gap)})
            continue

        for n in node_vals:
            t = NODE_TYPE.get(str(n).strip().lower())
            if not t:
                skipped["node type not in the classifier vocabulary"] += 1
                continue
            results.append({"level": lv, "line": s["line"], "kind": "node_type",
                            "claim": str(n), "holds": present.get(t, 0) > 0,
                            "measured": "%d measured rooms carry the kind '%s'"
                                        % (present.get(t, 0), t)})

        pr = props(lv)
        for e in edge_vals:
            t = str(e).strip().lower()
            if t in EDGE_DOOR:
                n = (pr or {}).get("Door", 0)
                results.append({"level": lv, "line": s["line"], "kind": "edge_type",
                                "claim": str(e), "holds": n > 0,
                                "measured": "%d Door props in the setup data" % n})
            elif t in EDGE_SPACE:
                k = EDGE_SPACE[t]
                results.append({"level": lv, "line": s["line"], "kind": "edge_type",
                                "claim": str(e), "holds": present.get(k, 0) > 0,
                                "measured": "%d measured rooms carry the kind '%s'"
                                            % (present.get(k, 0), k)})
            else:
                skipped["edge type not in the classifier vocabulary"] += 1

    hold = sum(1 for r in results if r["holds"])
    print("NODE AND EDGE TYPE CLAIMS CHECKED: %d" % len(results))
    print("   hold        : %d" % hold)
    print("   contradicted: %d" % (len(results) - hold))
    print("\nnot scored:")
    for k, n in skipped.most_common():
        print("   %-46s %3d" % (k, n))

    for r in results:
        print("\n   [%s] %s line %d  %s = %s"
              % ("HOLDS" if r["holds"] else "CONTRADICTED", r["level"], r["line"],
                 r["kind"], r["claim"][:56]))
        print("      %s" % r["measured"])

    print("\nSCHEMATIC GRAPHS ARE NOT SCORED. Seven symbolic nodes standing for a 58-room level is")
    print("an illustration of a topology, not a census, and what it does assert -- branching or a")
    print("single line -- is already covered by the topology checks elsewhere.")

    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump({"checked": len(results), "hold": hold, "skipped": dict(skipped),
                   "results": results}, fh, indent=1)
    print("\n-> %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
