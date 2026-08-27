#!/usr/bin/env python3
"""Do the scaled prop extents describe objects that could physically be in these rooms?

WHY THIS EXISTS SEPARATELY FROM THE EXTRACTOR. The scale chain has three multiplications --
model box, extrascale/256, and levelscale -- and getting any one wrong produces numbers that are
still plausible-looking. A radius that is 6.7x too large does not error, it just quietly tells a
bot the room is full. The join was already verified (340/340 enum members, 342/342 Train props);
this checks the MAGNITUDES, which is a different question and the one that bit.

⚠️ IT DELIBERATELY DOES NOT ASSERT THAT EVERY PROP FITS ITS ROOM. Reasoning from "prop wider than
the walkable floor => the scale is wrong" is what produced a false alarm on Train: the floor tiles
cover a carriage INTERIOR, while roller doors, hatch fittings and hull furniture sit on the
structure outside it and are legitimately wider than anything you can stand on. Demanding that
everything fits would fail correct data.

So it reports a DISTRIBUTION and names the outliers, and the judgement stays with a reader. The
useful signal is not "some props are large", it is "the typical prop is a sane size for the space",
plus objects of known real size landing at known real proportions.
"""
import argparse
import json
import os
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

# Bond, from ChrRecord: chrwidth 20, chrheight 185 (chr.c:1936). The only object in the world whose
# real size is known for certain, so everything else is judged in multiples of a person.
BOND_H = 185.0
BOND_W = 20.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", nargs="?", default="train")
    a = ap.parse_args()

    import gen_level_knowledge as glk
    from pack_world import load_level_scales

    lv = os.path.join(ROOT, "build", "levels")
    ext = json.load(open(os.path.join(lv, "_prop_extents.json"), encoding="utf-8"))["by_obj"]
    know = json.load(open(os.path.join(lv, "%s.json" % a.level), encoding="utf-8"))
    rooms = json.load(open(os.path.join(lv, "%s.rooms.json" % a.level), encoding="utf-8"))
    ls = load_level_scales(ROOT)[a.level]

    # extrascale lives in the setup file, not the knowledge JSON, until the emission lands.
    setup = know.get("setup")
    spath = None
    for base in (os.path.join(ROOT, "vendor", "ge-decomp", "assets", "obseg", "setup", "e"),):
        if setup and os.path.isfile(os.path.join(base, setup)):
            spath = os.path.join(base, setup)
    if spath is None:
        print("could not locate the setup file (%r) -- cannot read extrascale" % setup)
        return 1
    props_raw = glk.parse_props(open(spath, encoding="utf-8", errors="replace").read())
    scale_by_propdef = {p["propdef"]: p["extrascale"] for p in props_raw}

    # Room footprints from the floor tiles, for context rather than for a pass/fail.
    zs = [f["c"][2] for f in rooms["floors"]]
    xs = [f["c"][0] for f in rooms["floors"]]
    print("%s: levelscale %.5f | walkable floor spans x %.0f, z %.0f (ASSET units)"
          % (a.level, ls, max(xs) - min(xs), max(zs) - min(zs)))
    print("Bond for reference: %.0f tall, %.0f wide\n" % (BOND_H, BOND_W))

    widths, rows, missing = [], [], 0
    for p in know.get("props", []):
        if p.get("type") == "Guard":
            continue
        e = ext.get(str(p.get("obj")))
        es = scale_by_propdef.get(p.get("propdef"))
        if e is None or es is None:
            missing += 1
            continue
        s = es / 256.0
        if s <= 0.0:
            continue
        # asset-space half-extents
        hx = e["hx"] * s * ls
        hz = e["hz"] * s * ls
        rad = e["radius"] * s * ls
        widths.append(max(hx, hz) * 2.0)
        rows.append((max(hx, hz) * 2.0, e["model"], p.get("type"), rad))

    if not widths:
        print("no props resolved -- nothing to judge")
        return 1

    widths.sort()
    print("scaled footprint (full width, asset units) over %d props:" % len(widths))
    for q, name in ((0.05, " 5th"), (0.25, "25th"), (0.50, " MED"), (0.75, "75th"), (0.95, "95th")):
        v = widths[min(len(widths) - 1, int(q * len(widths)))]
        print("   %s pct  %7.1f   = %.2f x Bond's height" % (name, v, v / BOND_H))
    print("   mean      %7.1f" % statistics.mean(widths))

    print("\nlargest 6 (expected to be structure rather than furniture):")
    for w, m, t, r in sorted(rows, reverse=True)[:6]:
        print("   %-18s %-14s width %7.1f  radius %6.1f" % (m, t, w, r))

    print("\nsmallest 6 (expected to be pickups):")
    for w, m, t, r in sorted(rows)[:6]:
        print("   %-18s %-14s width %7.1f  radius %6.1f" % (m, t, w, r))

    # The check that actually discriminates: things whose real-world size is known.
    print("\nobjects of KNOWN real size, as a fraction of Bond's height (%.0f):" % BOND_H)
    known = {"chrtt33": ("TT-33 pistol", 0.11), "chrfnp90": ("P90 SMG", 0.28),
             "chrak47": ("AK-47", 0.48), "chrgrenade": ("grenade", 0.06),
             "ak47mag": ("AK magazine", 0.14)}
    for w, m, t, r in sorted(rows):
        if m in known:
            label, real = known[m]
            got = w / BOND_H
            flag = "ok" if 0.4 * real <= got <= 2.5 * real else "OUT BY %.1fx" % (got / real)
            print("   %-14s %-16s modelled %.2f vs real %.2f   %s" % (m, label, got, real, flag))
            known.pop(m)

    if missing:
        print("\n%d props had no model box or no readable extrascale (reported, not hidden)" % missing)
    return 0


if __name__ == "__main__":
    sys.exit(main())
