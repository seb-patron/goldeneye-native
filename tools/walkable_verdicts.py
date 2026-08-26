#!/usr/bin/env python3
"""Consume the engine's verdict on which graph edges are actually walkable.

WHY THIS EXISTS

The route graph's edges were assumed. Two offline attempts to check them disagreed with the game:
a triangle-intersection test against the exported wall polygons passes lines that
`bondviewTestLineUnobstructed` refuses, and stan does not know about walls at all -- it snaps to
the nearest standable tile, so it answers yes almost everywhere. The engine is the authority and
neither approximation is close enough.

So the check moves to runtime. the port's harness boots each level, walks every edge through
`gePortProbeWalkable` and writes the verdicts; this reads them back and drops what the engine
refused. That turns the graph from assumed to measured, and the division is by CAPABILITY rather
than by lane: producing the verdicts needs a machine that can run the game at speed, consuming
them is the extractor's job.

THE ONE RULE THAT MATTERS

AN EDGE NOT LISTED IS UNKNOWN, NOT UNWALKABLE. Only `ok: false` drops an edge.

If a capture dies halfway through a level -- a crash, a level that never reaches gameplay, a run
cut short -- the file is simply shorter. Treating absence as refusal would silently prune the
graph, and a router that quietly got stricter looks like a routing change rather than a truncated
capture. Every load reports how many edges went unmeasured so a short capture is visible instead
of inferred.

THE MASK IS RECORDED AND CHECKED

`gePortProbeWalkable` takes a collision-type mask and it changes the answer. the port measured
that including CDTYPE_DOORS turns Bunker 1's spawn into a sealed corridor 120 units wide -- a
convincing wrong answer, because a player walks through a door by opening it. A verdicts file
whose mask is unknown cannot be trusted later, so it is stored and surfaced.

Self-test (no capture and no game needed):

    python3 tools/walkable_verdicts.py --selftest
"""

import json
import os
import sys


def verdict_path(level, out_dir):
    return os.path.join(out_dir, level + ".walkable.json")


def load_verdicts(level, out_dir):
    """Return (verdicts, meta) or (None, None) when no capture exists.

    verdicts maps a normalised (lo, hi) edge to True/False. Normalised because the graph is
    undirected and a capture that recorded 6->13 must answer for 13->6; storing both directions
    would make a file that disagreed with itself possible.
    """
    p = verdict_path(level, out_dir)
    if not os.path.exists(p):
        return None, None

    with open(p, encoding="utf-8") as fh:
        doc = json.load(fh)

    verdicts = {}
    for e in doc.get("edges", []):
        try:
            a, b = int(e["a"]), int(e["b"])
        except (KeyError, TypeError, ValueError):
            continue                      # a malformed row is dropped, not guessed at
        if "ok" not in e:
            continue                      # no verdict recorded: unknown, same as absent
        verdicts[(min(a, b), max(a, b))] = bool(e["ok"])

    meta = {
        "probe": doc.get("probe"),
        "mask": doc.get("mask"),
        "level": doc.get("level", level),
        "recorded": len(verdicts),
    }
    return verdicts, meta


def filter_graph(graph, verdicts):
    """Drop edges the engine refused. Returns (graph, stats).

    The input graph is not modified: a caller that wants to report on both needs them both, and
    an in-place filter makes "before" unavailable by the time anyone asks.
    """
    out = {}
    seen = set()
    dropped = kept = unknown = 0

    for a, ns in graph.items():
        for b in ns:
            if a == b:
                continue
            key = (min(a, b), max(a, b))
            if key in seen:
                continue
            seen.add(key)

            v = verdicts.get(key)
            if v is False:
                dropped += 1
                continue
            if v is None:
                unknown += 1
            else:
                kept += 1
            out.setdefault(a, [])
            out.setdefault(b, [])
            if b not in out[a]:
                out[a].append(b)
            if a not in out[b]:
                out[b].append(a)

    # Nodes with no surviving edge still belong in the graph as isolated entries. Dropping them
    # would renumber nothing but would make "this node exists and is unreachable" indistinguishable
    # from "this node was never emitted", and the first is a finding while the second is a bug.
    for a in graph:
        out.setdefault(a, [])

    return out, {"dropped": dropped, "confirmed": kept, "unknown": unknown}


def apply_to_level(level, graph, out_dir, log=True):
    """Convenience wrapper: load, filter, report. Returns (graph, stats or None)."""
    verdicts, meta = load_verdicts(level, out_dir)
    if verdicts is None:
        if log:
            print("  %-10s no walkability capture -- every edge assumed" % level)
        return graph, None

    filtered, stats = filter_graph(graph, verdicts)
    if log:
        total = stats["confirmed"] + stats["dropped"] + stats["unknown"]
        print("  %-10s engine verdicts: %d confirmed, %d dropped, %d UNMEASURED of %d  (mask %s)"
              % (level, stats["confirmed"], stats["dropped"], stats["unknown"], total,
                 meta.get("mask") or "unrecorded"))
        if stats["unknown"]:
            # Loud on purpose. A partial capture is the failure this file is shaped around.
            print("             %d edges were not in the capture and are KEPT as unknown; "
                  "a short capture is not a stricter router" % stats["unknown"])
    return filtered, stats


# ---------------------------------------------------------------- self-test


def _selftest():
    fails = []

    def check(what, got, want):
        if got == want:
            print("  ok    %-46s %s" % (what, got))
        else:
            print("  FAIL  %-46s got %s want %s" % (what, got, want))
            fails.append(what)

    graph = {1: [2, 3], 2: [1, 4], 3: [1], 4: [2]}

    # Only ok:false drops. 2-4 is unmeasured and must survive.
    verdicts = {(1, 2): True, (1, 3): False}
    g, st = filter_graph(graph, verdicts)
    check("confirmed", st["confirmed"], 1)
    check("dropped", st["dropped"], 1)
    check("unknown kept", st["unknown"], 1)
    check("refused edge gone", 3 in g.get(1, []), False)
    check("confirmed edge kept", 2 in g.get(1, []), True)
    check("unmeasured edge kept", 4 in g.get(2, []), True)
    check("isolated node retained", 3 in g, True)
    check("edge is symmetric", 1 in g.get(2, []), True)
    check("input graph untouched", 3 in graph[1], True)

    # An EMPTY capture must change nothing. This is the truncated-run case: absence is not
    # refusal, and a router that pruned here would look stricter rather than under-measured.
    g2, st2 = filter_graph(graph, {})
    check("empty capture drops nothing", st2["dropped"], 0)
    # THREE distinct undirected edges: (1,2), (1,3), (2,4). The adjacency lists hold six entries
    # and it is easy to write 6 or 4 here by counting those instead; the confirmed/dropped/unknown
    # split above sums to 3 and is the cross-check.
    check("empty capture: all unknown", st2["unknown"], 3)
    check("empty capture keeps 1-3", 3 in g2.get(1, []), True)

    # Direction must not matter: the graph is undirected.
    g3, st3 = filter_graph(graph, {(1, 3): False})
    check("normalised both ways", 1 in g3.get(3, []), False)

    print("\n%s -- %d failure(s)" % ("FAILED" if fails else "PASSED", len(fails)))
    return 1 if fails else 0


def main():
    if "--selftest" in sys.argv:
        print("walkable verdicts consumer\n")
        return _selftest()
    print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
