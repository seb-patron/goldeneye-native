#!/usr/bin/env python3
"""Prop extents, offline, from model data.

WHY THIS EXISTS: every position in the pack is a POINT and the world is made of solids. A crate
reported "278 away" is 278 units to its CENTRE, so a bot that still sees room has already walked
into the corner of it.

THIS IS THE REPORTING HALF, NOT THE DECISION HALF. Knowing a crate's surface is at 158 does not
tell you whether the gap between it and the wall admits a body -- that is gePortCanStandAt over
stanTestVolume, which already exists. Extents make the REPORT honest. They are not a clearance
test and must not be used as one.

WHERE THE NUMBERS COME FROM. Two sources, and they answer subtly different questions:

  MODEL BOX      assets/obseg/prop/<name>/Model.c, a ModelRoData_BoundingBoxRecord holding
                 {flag, {xmin, xmax, ymin, ymax, zmin, zmax}}. This is the model's own box in
                 model space, UNROTATED and UNSCALED. Available for every prop that has a model.

  BOUND-PAD BOX  the 6 floats trailing each BoundPadRecord row in a level's pad3dlist. The decomp
                 says a bound pad "holds an extra Bounding Box which any prop assigned will try to
                 fill (non-uniform scaling)", so for those props this is the FITTED volume at that
                 placement -- strictly better than the model box. It only exists for props placed
                 with pad >= 10000, which measured 26% of all props (1273 of 4871).

So the model box is the floor of coverage and the bound-pad box is the refinement. Neither is
guessed: where we have neither, the prop is emitted WITHOUT extents rather than with a default,
because a made-up radius is worse than a missing one -- a reader can handle "unknown" and cannot
detect "plausible but invented".

FIELD ORDER IS xmin, xmax, ymin, ymax, zmin, zmax -- NOT min-then-max.
bondtypes.h's `bbox` union also aliases `coord3d min; coord3d max;` over the same storage, which
would make min = (xmin, xmax, ymin). That alias is wrong and reading it would silently produce
nonsense boxes. The order used here is confirmed twice: against the named fields, and against
ak47mag's own vertex data, whose x reaches -104 and z reaches 37 exactly as its box says.

THE MODEL BOX IS UNROTATED. A long crate at forty-five degrees occupies more width than its
half-extent suggests. That is why `radius` is emitted alongside `hx`/`hz` and labelled: hx/hz are
axis-aligned half-extents in the model's own frame, radius is the XZ circumradius and is the only
one of the three that is safe to use without knowing the prop's orientation.

THESE NUMBERS ARE NOT YET USABLE AS WORLD LENGTHS. READ THIS BEFORE WIRING THEM INTO ANYTHING.

The join is sound -- all 340 enum members resolve, and 342 of Train's 342 props find a model box.
The MAGNITUDES are not. These boxes are in MODEL space, and the engine multiplies them by
`model->scale` before use: chrobjGetBboxFromObjectRecord's caller does exactly that, scaling
Bounds.x/zmin/max by `model->scale` and rotating the result into four corners.

The measurement that shows it: Train's level spans about 4241 x 109 units, and the median model
box on Train is hx=221 with a max of 2275. A crate cannot be four times wider than the train it
sits in, and half the props would be wider than the carriage is. The implied scale is somewhere
around 0.1-0.15, not 1.0.

`model->scale` is set by modelSetScale at runtime and every caller I can find is a cheat, the
watch or a character -- none is the prop placement path -- so the prop's scale is applied
somewhere in the object loader and is NOT available to this script. Until that is resolved this
file emits MODEL-SPACE numbers only, and anything consuming them as world lengths will be wrong
by roughly a factor of ten in the DANGEROUS direction: it would report props far larger than they
are and a bot would refuse gaps it fits through easily.

Emitting them anyway with a guessed scale would be worse than emitting nothing. A missing radius
is visibly missing; a plausible wrong one is indistinguishable from a measurement.

WHAT WOULD CLOSE IT, cheapest first:
  1. Read model->scale live for a handful of known props on Train and compare against these boxes.
     One run gives the constant, or proves it is per-prop.
  2. For the 26% of props on bound pads, the pad's own bbox is the FITTED volume and needs no
     scale at all -- bondtypes.h:2746 documents a flag meaning "scale object to fit completely
     within preset bounds", which is that mechanism. Those can ship first and independently.

SPACE. Everything here is ASSET space, matching the extractor's convention (the pack converts
at its boundary by dividing by levelscale). Emitting runtime lengths here would make this file
disagree with every other JSON it sits beside.
"""
import json
import os
import re
import sys

DECOMP = os.environ.get("GE_DECOMP", r"C:\ge\vendor\ge-decomp")
PROPDIR = os.path.join(DECOMP, "assets", "obseg", "prop")
CONSTS = os.path.join(DECOMP, "src", "bondconstants.h")

FLOAT = r"-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?f?"


def prop_enum_order():
    """PROP_ALARM1, PROP_ALARM2, ... in declaration order -> index.

    The enum is ordinal and the model directories are the enumerator names lowercased with the
    PROP_ prefix dropped (PROP_AMMO_CRATE1 -> ammo_crate1), which is what joins a setup file's
    `obj` id to a model on disk.

    PROP_INVALID = -1 is declared FIRST and is explicitly negative -- the decomp comments that
    it exists to force the enum signed. Counting it as member 0 would shift every prop id by one
    and mislabel every extent in the game, so it is skipped rather than enumerated.
    """
    text = open(CONSTS, encoding="utf-8", errors="replace").read()
    m = re.search(r"PROP_INVALID\s*=\s*-1\s*,(.*?)\n\s*\}", text, re.S)
    if not m:
        raise SystemExit("could not locate the PROP_ enum body in bondconstants.h")
    names = re.findall(r"^\s*(PROP_[A-Z0-9_]+)\s*(?:,|=)", m.group(1), re.M)
    return {n: i for i, n in enumerate(names)}


def model_boxes():
    """<model dir> -> (xmin, xmax, ymin, ymax, zmin, zmax), in model space."""
    out = {}
    if not os.path.isdir(PROPDIR):
        raise SystemExit("no prop model directory at %s" % PROPDIR)
    row = re.compile(
        r"ModelRoData_BoundingBoxRecord\s+\w+\s*=\s*\{\s*[^,]+,\s*\{\s*"
        + r"\s*,\s*".join([r"(%s)" % FLOAT] * 6) + r"\s*\}", re.S)
    for name in sorted(os.listdir(PROPDIR)):
        f = os.path.join(PROPDIR, name, "Model.c")
        if not os.path.isfile(f):
            continue
        mm = row.search(open(f, encoding="utf-8", errors="replace").read())
        if mm:
            out[name] = tuple(float(g.rstrip("f")) for g in mm.groups())
    return out


def extents_from_box(b):
    """(xmin,xmax,ymin,ymax,zmin,zmax) -> hx, hz, radius, plus the centre offset.

    THE CENTRE OFFSET MATTERS AND IS EASY TO DROP. These boxes are not symmetric about the origin
 -- ak47mag runs x[-104,104] but plenty do not -- so the prop's PAD POSITION is not the centre
    of its box. Emitting only half-extents would silently assume it is, and put the box in the
    wrong place by exactly the asymmetry.
    """
    xmin, xmax, ymin, ymax, zmin, zmax = b
    hx = abs(xmax - xmin) / 2.0
    hz = abs(zmax - zmin) / 2.0
    return {
        "hx": round(hx, 2),
        "hz": round(hz, 2),
        "hy": round(abs(ymax - ymin) / 2.0, 2),
        # XZ circumradius: the smallest circle containing the footprint at ANY rotation. This is
        # the number to use when the prop's orientation is unknown, and it is deliberately
        # conservative -- it over-reserves for a square box by ~41%.
        "radius": round((hx * hx + hz * hz) ** 0.5, 2),
        "cx": round((xmin + xmax) / 2.0, 2),
        "cz": round((zmin + zmax) / 2.0, 2),
    }


def main():
    order = prop_enum_order()
    boxes = model_boxes()

    # Join the enum to the directories. Reported rather than assumed: a silent miss here shows up
    # much later as a prop with no extents and no reason given.
    # matched case-INSENSITIVELY. The enumerators are all upper case and most directories are all
    # lower, so a plain .lower() looks right and silently drops the four that are not: ICBM,
    # ICBM_nose, console_sev_GEa, console_sev_GEb. Four props out of 340 is small enough to never
    # notice and large enough to matter to whoever walks into an ICBM.
    fold = {d.lower(): d for d in boxes}
    by_id, unmatched_enum, unmatched_dir = {}, [], set(boxes)
    for name, idx in order.items():
        d = fold.get(name[len("PROP_"):].lower())
        if d is not None:
            by_id[idx] = dict(extents_from_box(boxes[d]), model=d)
            unmatched_dir.discard(d)
        else:
            unmatched_enum.append(name)

    print("prop enum members      %d" % len(order))
    print("model dirs with a box  %d" % len(boxes))
    print("joined by name         %d" % len(by_id))
    print("enum with no model     %d" % len(unmatched_enum))
    print("model with no enum     %d" % len(unmatched_dir))
    if unmatched_dir:
        print("  e.g. %s" % ", ".join(sorted(unmatched_dir)[:8]))

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "levels")
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    dest = os.path.join(out, "_prop_extents.json")
    with open(dest, "w", encoding="utf-8") as fh:
        # The caveat travels IN the data, not only in this file. A consumer reads the JSON and
        # never opens the script, and the one thing they must not do is treat these as world
        # lengths -- so "unusable" is a field, not a comment.
        json.dump({"space": "model",
                   "usable_as_world_lengths": False,
                   "why_not": "MODEL space, not world. The engine multiplies these by "
                              "model->scale before use (see chrobjGetBboxFromObjectRecord's "
                              "caller). Implied scale is roughly 0.1-0.15: Train spans ~4241x109 "
                              "units but the median model box there is hx=221, max 2275. Using "
                              "these unscaled overstates props by ~10x and makes a bot refuse "
                              "gaps it fits through.",
                   "note": "hx/hz are axis-aligned half-extents in MODEL space and are UNROTATED; "
                           "radius is the XZ circumradius and is the orientation-safe one",
                   "by_obj": {str(k): v for k, v in sorted(by_id.items())}}, fh, indent=1)
    print("\nwrote %s (%d props)" % (dest, len(by_id)))

    # Coverage against real levels, which is the number that decides whether this is worth
    # shipping. Counting the join is not the same as counting the props it reaches.
    lv = os.path.join(out, "train.json")
    if os.path.isfile(lv):
        k = json.load(open(lv, encoding="utf-8"))
        props = k.get("props", [])
        hit = sum(1 for p in props if p.get("obj") in by_id)
        print("train: %d of %d props resolve to a model box (%.0f%%)"
              % (hit, len(props), 100.0 * hit / max(1, len(props))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
