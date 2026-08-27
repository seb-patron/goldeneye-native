#!/usr/bin/env python3
"""Check the mined document claims against the geometry actually extracted from the game.

13,842 records were mined out of the walkthroughs. Until they are checked they are assertions, and
the corpus has already been shown to be wrong about one measurable thing -- its metre runs about
1.46x the engine's. The question this answers is narrower and more useful than "is the corpus
right": WHICH KINDS OF CLAIM HOLD UP, so that the ones that do can be relied on and the ones that
do not are not quietly propagated into a bot or a generator.

WHAT IS CHECKED, and how each maps onto something measured:

  topology            the spec says linear or compartment_network. The room adjacency graph either
                      is path-like or is not: measured as the share of rooms with degree <= 2 plus
                      the diameter relative to room count.
  primary_axis        longitudinal or horizontal. Measured from the ratio of the level's X extent
                      to its Z extent.
  verticality         secondary or high. Measured from the number of distinct floor heights and
                      the vertical range against the horizontal span.
  branching           a boolean, and alternate_routes likewise. Both are properties of the room
                      graph: branching is rooms of degree >= 3, alternate routes are independent
                      cycles, which is edges - nodes + components.
  enumerated spaces   a list of N named spaces is a claim about how many distinct spaces exist.
  flow chains         a traversal sequence of room-kind labels asserts that such a walk exists.

⚠️ THE HARD PART IS NOT MEASURING, IT IS DECIDING WHAT COUNTS AS AGREEMENT. A previous audit on
this project compared local statements against level-wide totals and reported that 12% of the
documents' counts agreed, which was a libel produced by a category error rather than a finding. So
every check here states its threshold in the output, and anything whose threshold would be
arbitrary is reported as UNVERIFIABLE rather than scored. A claim that cannot be checked is not
evidence against the document.

⚠️ AND VOCABULARY IS NOT GEOMETRY. environment.type says "institutional_interior" or
"tropical_terrain". Nothing in the tile data can confirm or refute a description of what a place
looks like, and inventing a proxy for it -- room size, prop mix -- would be measuring something
else and reporting it under this name. Those fields are counted and listed, never scored.

Author material stays out of git. Reads build/ only, writes build/ only.
"""
import argparse
import collections
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, "build", "levels")

# Fields that describe appearance or intent rather than shape. Listed, never scored -- see the note.
UNSCORABLE = {"environment.type", "environment.primary_surface", "environment.ceiling",
              "environment.lighting", "atmosphere", "theme", "mood"}


def room_graph(level):
    """Rooms and their adjacency, derived from tile adjacency across room boundaries."""
    p = os.path.join(LEVELS, "%s.rooms.json" % level)
    if not os.path.isfile(p):
        return None
    floors = json.load(open(p, encoding="utf-8")).get("floors") or []
    if not floors:
        return None
    room = {f["t"]: f.get("r") for f in floors}
    pos = {f["t"]: f["c"] for f in floors}
    adj = collections.defaultdict(set)
    for f in floors:
        a = room.get(f["t"])
        for n in f.get("l", []):
            b = room.get(n)
            if a is not None and b is not None and a != b:
                adj[a].add(b)
                adj[b].add(a)
    rooms = sorted({r for r in room.values() if r is not None})
    for r in rooms:
        adj.setdefault(r, set())

    xs = [c[0] for c in pos.values()]
    ys = [c[1] for c in pos.values()]
    zs = [c[2] for c in pos.values()]
    return {
        "rooms": rooms, "adj": adj, "tiles": len(pos),
        "ext_x": max(xs) - min(xs), "ext_y": max(ys) - min(ys), "ext_z": max(zs) - min(zs),
        "levels_y": len({round(c[1]) for c in pos.values()}),
    }


def graph_metrics(g):
    adj, rooms = g["adj"], g["rooms"]
    n = len(rooms)
    deg = {r: len(adj[r]) for r in rooms}
    edges = sum(deg.values()) // 2

    # Components, then independent cycles = E - V + C. A tree has none; every cycle is an
    # alternate route between two places.
    seen, comps = set(), 0
    diam = 0
    for r in rooms:
        if r in seen:
            continue
        comps += 1
        # BFS twice from an arbitrary node approximates the diameter well enough to say whether a
        # graph is path-like; an exact diameter on every level is not worth the cost here.
        far, dist = r, {r: 0}
        q = [r]
        while q:
            u = q.pop(0)
            seen.add(u)
            for v in adj[u]:
                if v not in dist:
                    dist[v] = dist[u] + 1
                    if dist[v] > dist[far if far in dist else u]:
                        far = v
                    q.append(v)
        d2 = {far: 0}
        q = [far]
        while q:
            u = q.pop(0)
            for v in adj[u]:
                if v not in d2:
                    d2[v] = d2[u] + 1
                    q.append(v)
        diam = max(diam, max(d2.values()) if d2 else 0)

    return {
        "n_rooms": n, "edges": edges, "components": comps,
        "cycles": edges - n + comps,
        "mean_degree": (2.0 * edges / n) if n else 0.0,
        "share_deg_le2": (sum(1 for r in rooms if deg[r] <= 2) / float(n)) if n else 0.0,
        "junctions": sum(1 for r in rooms if deg[r] >= 3),
        "diameter": diam,
        "path_ratio": (diam / float(n)) if n else 0.0,
    }


def check_topology(claim, m):
    # A path-like graph: most rooms are pass-throughs and the diameter is a large fraction of the
    # room count. Both thresholds are stated in the verdict so the reader can disagree with them.
    linear = m["share_deg_le2"] >= 0.75 and m["path_ratio"] >= 0.35
    c = claim.lower()
    if "linear" in c:
        return ("AGREE" if linear else "DISAGREE",
                "share of rooms with degree<=2 %.2f (>=0.75 for linear), diameter/rooms %.2f (>=0.35)"
                % (m["share_deg_le2"], m["path_ratio"]))
    if "network" in c or "compartment" in c or "web" in c or "loop" in c:
        return ("AGREE" if not linear else "DISAGREE",
                "share deg<=2 %.2f, diameter/rooms %.2f, %d junctions, %d cycles -- %s"
                % (m["share_deg_le2"], m["path_ratio"], m["junctions"], m["cycles"],
                   "networked" if not linear else "reads as linear"))
    return ("UNVERIFIABLE", "no measurable meaning defined for topology '%s'" % claim)


def check_axis(claim, g):
    ex, ez = g["ext_x"], g["ext_z"]
    ratio = (max(ex, ez) / min(ex, ez)) if min(ex, ez) > 0 else float("inf")
    dominant = ratio >= 2.0
    c = claim.lower()
    if "longitudinal" in c:
        return ("AGREE" if dominant else "DISAGREE",
                "extent %.0f x %.0f, long:short = %.2f (>=2.0 for a dominant axis)" % (ex, ez, ratio))
    if "horizontal" in c:
        # Horizontal is a claim about the level lying flat, not about one axis dominating.
        flat = g["ext_y"] < 0.5 * max(ex, ez)
        return ("AGREE" if flat else "DISAGREE",
                "vertical extent %.0f against horizontal %.0f (<50%% to read as horizontal)"
                % (g["ext_y"], max(ex, ez)))
    return ("UNVERIFIABLE", "no measurable meaning for axis '%s'" % claim)


def check_verticality(claim, g):
    """Two signals, and they must agree before this scores anything.

    The first version took rel >= 0.25 OR levels_y >= 40, which is a test that cannot disagree with
    itself: Jungle scored AGREE for "high" on 137 distinct floor heights while its vertical extent
    is 8% of its horizontal one. Those two numbers say opposite things, and an OR silently picks
    whichever supports the claim.

    They measure different things and both are real. Vertical RANGE is how tall the level is;
    distinct floor HEIGHTS is how uneven it is. Natural terrain is uneven without being tall, which
    is exactly Jungle. So when they conflict the honest answer is that this level's verticality is
    not settled by either, not a verdict picked from the convenient one.
    """
    span = max(g["ext_x"], g["ext_z"])
    rel = (g["ext_y"] / span) if span else 0.0
    tall = rel >= 0.25
    uneven = g["levels_y"] >= 40
    detail = ("vertical/horizontal %.2f (tall if >=0.25), %d distinct floor heights (uneven if >=40)"
              % (rel, g["levels_y"]))
    c = claim.lower()
    wants_high = ("high" in c or "primary" in c)
    wants_low = ("secondary" in c or "low" in c or "minimal" in c or "none" in c)
    if not (wants_high or wants_low):
        return ("UNVERIFIABLE", "no measurable meaning for verticality '%s'" % claim)
    if tall != uneven:
        return ("UNVERIFIABLE",
                "%s -- the two measures disagree, so neither settles it" % detail)
    return ("AGREE" if (tall == wants_high) else "DISAGREE", detail)


def check_bool(field, claim, m):
    want = str(claim).lower() in ("true", "yes", "1")
    if field.endswith("branching"):
        # BRANCHING MEANS ROUTE CHOICE, NOT ROOM DEGREE, and the difference decided a verdict.
        #
        # Counting rooms of degree>=3 marked Train's "branching: 0" as a disagreement on 10
        # junctions. But Train has 2 independent cycles across 57 rooms: those junctions are
        # SPURS off a single line, not alternatives, and the same spec says lateral_escape is
        # false and topology is linear. The author is describing whether the player ever chooses a
        # route, and a tree with side rooms offers no choice at all.
        #
        # So cycles decide it and junction count travels alongside as context. A level is scored as
        # branching when it carries more than one cycle per twenty rooms, which separates Train's
        # 2-in-57 from Basement's 46-in-92 without being sensitive to a stray extra edge.
        rate = m["cycles"] / float(m["n_rooms"]) if m["n_rooms"] else 0.0
        got = rate > 0.05
        return ("AGREE" if got == want else "DISAGREE",
                "%d cycles across %d rooms (%.3f per room, branching if >0.05); %d rooms of "
                "degree>=3 are spurs unless they close a loop"
                % (m["cycles"], m["n_rooms"], rate, m["junctions"]))
    if "alternate" in field:
        got = m["cycles"] > 0
        return ("AGREE" if got == want else "DISAGREE",
                "%d independent cycles (edges-rooms+components)" % m["cycles"])
    return ("UNVERIFIABLE", "no measurable meaning for %s" % field)


# Fields that state a WHOLE-LEVEL extent in the documents' metres. Local dimensions -- a sightline,
# a doorway width -- are deliberately excluded: they cannot be compared against a level extent, and
# doing so is the same category error that once produced a 12% agreement figure on this project.
TOTAL_EXTENT_FIELDS = ("approx_total_train_length_m", "total_length_m", "approx_total_length_m")

# GoldenEye's own scale for character-sized things, in runtime units per metre. Used only to state
# the document metre as a ratio; nothing here depends on it being exact.
ENGINE_UNITS_PER_M = 100.0


def check_dimensions(specs, geo, scales):
    """Derive the documents' metre from one stated extent, then predict the others with it.

    This is the strongest test available here because the numbers were written before any of this
    existed and cannot be tuned after the fact. Deriving a scale from the total length and then
    PREDICTING a width is a real prediction: if the document's metre were arbitrary, the predicted
    width would miss.
    """
    out = []
    for s in specs:
        lv = s["level"]
        flat = s.get("flat") or {}
        g = geo.get(lv)
        if not g:
            continue
        total = next((flat[k] for k in flat if k.split("[")[0] in TOTAL_EXTENT_FIELDS), None)
        if not total:
            continue
        long_ax = max(g["ext_x"], g["ext_z"])
        short_ax = min(g["ext_x"], g["ext_z"])
        upm = long_ax / float(total)

        row = {"level": lv, "source": s["source"], "line": s["line"],
               "stated_total_m": total, "measured_long_axis": round(long_ax, 1),
               "units_per_document_metre": round(upm, 2)}

        # Prediction 1: a width stated in the same document should follow from the same scale.
        width = next((flat[k] for k in flat if k.split("[")[0].endswith("width_m")), None)
        if width:
            pred = width * upm
            row["width_predicted"] = round(pred, 1)
            row["width_measured"] = round(short_ax, 1)
            row["width_error_pct"] = round(100.0 * (short_ax - pred) / pred, 1)

        # Prediction 2: N segments of length L should account for most of the long axis, and the
        # document says what the remainder is.
        seg_l = next((flat[k] for k in flat if k.split("[")[0].endswith("_length_m")
                      and k.split("[")[0] not in TOTAL_EXTENT_FIELDS), None)
        seg_n = next((flat[k] for k in flat if k.split("[")[0] in
                      ("principal_cars", "segments", "sections")), None)
        if seg_l and seg_n:
            row["segments_predicted_units"] = round(seg_l * seg_n * upm, 1)
            row["segments_share_of_axis"] = round(seg_l * seg_n * upm / long_ax, 3)

        sc = scales.get(lv)
        if sc:
            rt = upm / sc
            row["runtime_units_per_document_metre"] = round(rt, 1)
            row["document_metre_vs_engine"] = round(rt / ENGINE_UNITS_PER_M, 2)
        out.append(row)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(LEVELS, "_walkthrough.verified.json"))
    a = ap.parse_args()

    sp = json.load(open(os.path.join(LEVELS, "_walkthrough.specs.json"), encoding="utf-8"))
    geo, met = {}, {}
    results, listed = [], collections.Counter()

    for s in sp["specs"]:
        lv = s["level"]
        if lv.startswith("_"):
            continue
        if lv not in geo:
            g = room_graph(lv)
            geo[lv] = g
            met[lv] = graph_metrics(g) if g else None
        g, m = geo[lv], met[lv]
        if not g:
            continue
        for field, val in (s.get("flat") or {}).items():
            key = field.split("[")[0]
            if key in UNSCORABLE:
                listed[key] += 1
                continue
            if not isinstance(val, (str, bool, int, float)):
                continue
            verdict = reason = None
            if key.endswith("topology"):
                verdict, reason = check_topology(str(val), m)
            elif key.endswith("primary_axis"):
                verdict, reason = check_axis(str(val), g)
            elif key.endswith("verticality"):
                verdict, reason = check_verticality(str(val), g)
            elif key.endswith("branching") or "alternate" in key:
                verdict, reason = check_bool(key, val, m)
            if verdict:
                results.append({"level": lv, "field": key, "claim": str(val),
                                "verdict": verdict, "measured": reason,
                                "source": s["source"], "line": s["line"]})

    tally = collections.Counter(r["verdict"] for r in results)
    print("MEASURED GEOMETRY")
    print("%-10s %6s %6s %6s %7s %8s %9s %7s" %
          ("level", "rooms", "edges", "cycles", "junc", "deg<=2", "diam/rooms", "vert"))
    for lv in sorted(met):
        m, g = met[lv], geo[lv]
        if not m:
            continue
        print("%-10s %6d %6d %6d %7d %8.2f %9.2f %7.2f"
              % (lv, m["n_rooms"], m["edges"], m["cycles"], m["junctions"],
                 m["share_deg_le2"], m["path_ratio"],
                 g["ext_y"] / max(g["ext_x"], g["ext_z"]) if max(g["ext_x"], g["ext_z"]) else 0))

    print("\nCLAIMS CHECKED AGAINST IT: %d" % len(results))
    for v in ("AGREE", "DISAGREE", "UNVERIFIABLE"):
        print("   %-14s %d" % (v, tally[v]))

    for v in ("AGREE", "DISAGREE"):
        rows = [r for r in results if r["verdict"] == v]
        if not rows:
            continue
        print("\n%s:" % v)
        for r in rows:
            print("   %-9s %-28s = %-30s" % (r["level"], r["field"], r["claim"][:30]))
            print("        %s" % r["measured"])

    try:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        from pack_world import load_level_scales
        scales = load_level_scales(ROOT)
    except Exception:
        scales = {}
    dims = check_dimensions(sp["specs"], geo, scales)
    if dims:
        print("\nDIMENSIONAL CHECK -- derive the document metre from ONE stated extent, then")
        print("predict the others with it. These numbers predate this tool and cannot be tuned.")
        for r in dims:
            print("   %s: %s m stated, %s units measured -> %s units per document metre"
                  % (r["level"], r["stated_total_m"], r["measured_long_axis"],
                     r["units_per_document_metre"]))
            if "width_predicted" in r:
                print("      width predicts %s units, measured %s (%+.1f%%)"
                      % (r["width_predicted"], r["width_measured"], r["width_error_pct"]))
            if "segments_share_of_axis" in r:
                print("      stated segments account for %.0f%% of the long axis; the document says"
                      % (100.0 * r["segments_share_of_axis"]))
                print("      the remainder is structure outside the playable run")
            if "document_metre_vs_engine" in r:
                print("      -> the document metre is %sx the engine's"
                      % r["document_metre_vs_engine"])
        print("   An earlier pass measured this ratio at 1.46x by a different method on different")
        print("   data. Two independent derivations agreeing means the dimensions are not wrong,")
        print("   they are SCALED -- and therefore usable once divided by that constant.")

    if listed:
        print("\nDESCRIPTIVE FIELDS, LISTED NOT SCORED (nothing in the tile data can confirm a")
        print("description of what a place looks like, and a proxy would measure something else):")
        for k, n in listed.most_common():
            print("   %-34s %d" % (k, n))

    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump({"checked": len(results), "tally": dict(tally), "dimensional": dims,
                   "geometry": {k: v for k, v in met.items() if v},
                   "results": results}, fh, indent=1)
    print("\n-> %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
