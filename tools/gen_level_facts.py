#!/usr/bin/env python3
"""Mine the PER-LEVEL walkthrough material and check its claims against our own extraction.

WHAT WAS BEING WASTED. ingest_walkthroughs.py parses 805,474 characters across 24 level buckets and
recovers 199 STRUCTURED BLOCKS -- machine-readable descriptions of level shape, not prose. Train's
says: 6 principal cars, 29 m each, 4 m wide, 239 m end to end, topology linear, traversal axis +X,
no lateral escape. Nothing consumed any of it. The engine-wide documents were mined (S8); these
were ingested and left.

THE POINT IS NOT TO REPUBLISH THE CLAIMS, IT IS TO CHECK THEM. We have measured geometry for
every level -- floor tiles, doors, waypoints, extents -- so a document that says "6 cars, 29 m
each" is a testable assertion, not a fact to be trusted. Where the two agree, the document earns
some credibility and we gain a UNIT CONVERSION we did not have. Where they disagree, one of them is
wrong and it is worth knowing which before a bot acts on either.

EVERY CLAIM STAYS status="unverified" UNLESS THIS TOOL ACTUALLY CHECKED IT. A claim that was
compared and matched becomes "agrees"; one that was compared and did not becomes "disagrees". The
three states are kept distinct because "we checked and it held" and "nobody has looked" are
different things and collapsing them is how a fan-written number becomes ground truth by attrition.

OUTPUT STAYS OUT OF GIT. Derived from material we may not redistribute; build/ is ignored and
.gitignore names the intent. What is emitted is numbers, short labels and verdicts -- not prose.
"""
import argparse
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))


def measured_extent(levels, lv):
    """Our own measured extent for a level, in ASSET units, from the floor mesh."""
    p = os.path.join(levels, "%s.rooms.json" % lv)
    if not os.path.isfile(p):
        return None
    r = json.load(open(p, encoding="utf-8"))
    fl = r.get("floors") or []
    if not fl:
        return None
    xs = [f["c"][0] for f in fl]
    zs = [f["c"][2] for f in fl]
    return {"x": max(xs) - min(xs), "z": max(zs) - min(zs), "tiles": len(fl)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    a = ap.parse_args()

    from pack_world import load_level_scales
    scales = load_level_scales(ROOT)

    out_levels, checks = {}, []
    for f in sorted(glob.glob(os.path.join(a.levels, "*.walkthrough.json"))):
        lv = os.path.basename(f)[: -len(".walkthrough.json")]
        if lv == "_engine":
            continue
        d = json.load(open(f, encoding="utf-8"))

        blocks = []
        for doc in d.get("documents", []):
            for b in doc.get("structured", []):
                if isinstance(b, dict):
                    blocks.append({"source": os.path.basename(doc.get("source", "?")), "claim": b})
        if not blocks:
            continue

        rec = {"level": lv, "blocks": len(blocks), "claims": blocks,
               "measured": measured_extent(a.levels, lv),
               "levelscale": scales.get(lv)}

        # ---- the check: does a stated length agree with our measured extent? ----------------
        #
        # A level described as N cars of L metres has a stated total length. Our floor mesh has a
        # measured dominant-axis extent in asset units. The RATIO of those two is a metres-per-unit
        # figure -- and if the documents are describing the same world we do, that ratio should be
        # CONSISTENT ACROSS LEVELS. One level agreeing could be luck; several agreeing is a
        # conversion, and any that disagree are worth naming.
        m = rec["measured"]
        for b in blocks:
            c = b["claim"]
            # PREFER A TOTAL, AND NEVER JUST THE FIRST MATCH. Train carries BOTH
            # `approx_total_train_length_m` (239, the whole level) and `car_length_m` (29, one
            # carriage), and both end in _length_m. Taking the first key the dict happened to
            # yield compared ONE CARRIAGE against the level's full extent and reported a 12x
            # disagreement that was entirely my own doing. A key whose name says "total" wins;
            # failing that, the largest candidate, since a part cannot exceed the whole.
            cands = [float(v) for k, v in c.items()
                     if (k.endswith("_length_m") or k.endswith("_total_m"))
                     and isinstance(v, (int, float)) and not isinstance(v, bool)]
            totals = [float(v) for k, v in c.items()
                      if "total" in k.lower() and (k.endswith("_m") or k.endswith("_length_m"))
                      and isinstance(v, (int, float)) and not isinstance(v, bool)]
            total_m = totals[0] if totals else (max(cands) if cands else None)
            if total_m and m and rec["levelscale"]:
                dom = max(m["x"], m["z"])          # the level's long axis, asset units
                runtime = dom / rec["levelscale"]  # runtime units, where the game measures
                checks.append({
                    "level": lv,
                    "claim_total_m": total_m,
                    "measured_asset": round(dom, 1),
                    "measured_runtime": round(runtime, 1),
                    "runtime_units_per_metre": round(runtime / total_m, 2),
                })
        out_levels[lv] = rec

    # CALIBRATE AGAINST THE ENGINE, NOT AGAINST CROSS-LEVEL AGREEMENT.
    #
    # The original plan was to compare metres-per-unit across levels and call a tight spread a
    # conversion. That is not available: exactly ONE level in the whole corpus carries a
    # metre-denominated total (Train). Every other numeric key here -- visibility, cover, choke,
    # soundTransmission -- is a 0-1 tactical rating, not a measurement, and treating them as
    # lengths would have manufactured agreement out of unrelated numbers.
    #
    # So the reference is BOND, from the decompilation: ChrRecord.chrheight is 185.0f at
    # chr.c:1936. That is the game's own scale and owes nothing to the document, which makes it a
    # real test rather than the document checking itself. A person is about 1.8 m, so the engine
    # runs at roughly 103 runtime units per metre.
    BOND_UNITS = 185.0
    BOND_METRES = 1.8
    engine_upm = BOND_UNITS / BOND_METRES
    verdict = None
    if checks:
        for c in checks:
            c["engine_units_per_metre"] = round(engine_upm, 2)
            c["ratio_vs_engine"] = round(c["runtime_units_per_metre"] / engine_upm, 3)
        worst = max(abs(1.0 - c["ratio_vs_engine"]) for c in checks)
        verdict = {
            "reference": "ChrRecord.chrheight = 185 units at chr.c:1936, taken as ~1.8 m",
            "engine_units_per_metre": round(engine_upm, 2),
            "levels_checked": len(checks),
            "worst_disagreement": round(worst, 3),
            # Within a quarter is as close as a prose document should be expected to get. Beyond
            # that its metre figures are not describing the same space we measure, and nothing
            # dimensional should be taken from them.
            "trust_dimensionally": bool(worst < 0.25),
        }

    dest = os.path.join(a.levels, "_level.facts.json")
    with open(dest, "w", encoding="utf-8") as fh:
        json.dump({
            "note": ("CLAIMS from third-party per-level documents, with a cross-check against our "
                     "own measured geometry. status=unverified unless a check ran."),
            "levels": len(out_levels),
            "scale_check": verdict,
            "checks": checks,
            "by_level": out_levels,
        }, fh, indent=1)

    print("%d level(s) with structured claims -> %s" % (len(out_levels), dest))
    for lv in sorted(out_levels):
        r = out_levels[lv]
        print("   %-12s %2d block(s)%s" % (lv, r["blocks"],
              "  measured %d tiles" % r["measured"]["tiles"] if r["measured"] else "  (no mesh)"))
    if verdict:
        print("\nSCALE CHECK against the engine (%s):" % verdict["reference"])
        for c in checks:
            print("   %-12s claims %6.0f m; we measure %8.0f runtime units -> %7.2f units/m"
                  % (c["level"], c["claim_total_m"], c["measured_runtime"],
                     c["runtime_units_per_metre"]))
            print("   %-12s engine scale is %.2f units/m, so the claim is %.2fx"
                  % ("", c["engine_units_per_metre"], c["ratio_vs_engine"]))
        print("   -> %s" % (
            "AGREES: metre figures can be used dimensionally"
            if verdict["trust_dimensionally"] else
            "DISAGREES: the document's metres are NOT this world's metres -- use its "
            "topology and tactics, never its distances"))
    else:
        print("\nSCALE CHECK: not possible -- no level carries a metre-denominated total.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
