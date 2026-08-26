#!/usr/bin/env python3
"""Turn coordinates into places a person would recognise.

A route reads as "node 3818 to node 3814", which says nothing about where either is or whether
going from one to the other makes sense. The walkthrough material describes the same levels the
way a player sees them: Train is six carriages on one axis, about 29 metres each, four wide, no
branching. Combining the two gives a position a name.

The scale comes out of the measurement rather than being assumed. Train's floor mesh spans 35,786
game units along its traversal axis, and the reconstruction says six cars, so a car is 5,964 units
and a metre is about 206. The width agrees independently: 739 units across against a stated four
metres is 185 per metre, within a tenth of the length figure, which is the sort of corroboration
worth having before trusting either number.

Emits build/levels/<level>.places.json: the axis, the divisions along it, and units per metre.
Only levels whose brief describes a division are covered; the rest get an extent and a scale,
which is still more than a bare coordinate.
"""
import json, os, sys

LEVELS_DIR = os.path.join("build", "levels")


def load(level, kind):
    p = os.path.join(LEVELS_DIR, "%s.%s.json" % (level, kind))
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8") as fh:
        return json.load(fh)


def level_scale(level):
    """asset = runtime * levelscale, parsed from the decomp so there is one copy."""
    import re
    p = os.path.join("vendor", "ge-decomp", "src", "game", "bg.c")
    if not os.path.exists(p):
        return 1.0
    with open(p, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    for m in re.finditer(r'\{LEVELID_(\w+),\s*"[^"]+",\s*"[^"]+",\s*([0-9.]+),', src):
        if m.group(1).lower() == level:
            return float(m.group(2))
    return 1.0


def spawn_along(level, axis, scale):
    """Where the player actually starts, in runtime units along the traversal axis."""
    p = os.path.join(LEVELS_DIR, "spawns.json")
    if os.path.exists(p):
        with open(p, encoding="utf-8") as fh:
            sp = json.load(fh)
        e = sp.get(level) if isinstance(sp, dict) else None
        if isinstance(e, dict) and "pos" in e:
            pos = e["pos"]
            return pos[0] if axis == "x" else pos[2]
    routes = load(level, "routes")
    if routes and routes.get("spawn_pos"):
        pos = routes["spawn_pos"]
        return pos[0] if axis == "x" else pos[2]
    return None


def places_for(level):
    rooms = load(level, "rooms")
    brief = load(level, "brief")
    if not rooms:
        return None

    floors = [f for f in (rooms.get("floors") or []) if f.get("c")]
    if not floors:
        return None

    scale = level_scale(level) or 1.0
    xs = [f["c"][0] / scale for f in floors]
    zs = [f["c"][2] / scale for f in floors]

    out = {
        "level": level,
        "space": "runtime",
        "extent": {"x": [min(xs), max(xs)], "z": [min(zs), max(zs)]},
    }

    if not brief:
        return out

    axis = str(brief.get("traversal_axis", "")).lstrip("+-").lower() or "x"
    along = xs if axis == "x" else zs
    across = zs if axis == "x" else xs
    span = max(along) - min(along)
    width = max(across) - min(across)

    # A division is only meaningful if the reconstruction names one.
    count = brief.get("principal_cars") or brief.get("principal_rooms")
    length_m = brief.get("car_length_m")
    width_m = brief.get("car_width_m")
    if not count or count < 2:
        return out

    seg = span / float(count)
    out["axis"] = axis
    out["divisions"] = count
    out["division_name"] = "car" if brief.get("principal_cars") else "section"
    out["units_per_division"] = seg

    if length_m:
        out["units_per_metre"] = seg / float(length_m)
        if width_m:
            # Stated independently, so it is a check rather than a second assumption.
            out["units_per_metre_from_width"] = width / float(width_m)

    # Numbered from where the player actually starts, so car 1 is where the mission begins.
    #
    # The brief's traversal_axis is taken from a human reconstruction and disagrees with the
    # measurement on Train: it reads "+X" while the spawn sits at x=779, the high end, and the
    # objective at x=-13868. The brief itself says the extraction wins on conflict, so the
    # measured spawn decides which end is the front.
    lo, hi = min(along), max(along)
    spawn = spawn_along(level, axis, scale)
    from_high = spawn is not None and abs(spawn - hi) < abs(spawn - lo)

    bounds = []
    for i in range(count):
        a, b = lo + i * seg, lo + (i + 1) * seg
        idx = (count - i) if from_high else (i + 1)
        # entry is the end the player walks in through, so "12m into car 3" counts from the door
        # they came through rather than from whichever end happens to be numerically lower.
        bounds.append({"index": idx, "from": a, "to": b, "entry": (b if from_high else a)})
    bounds.sort(key=lambda e: e["index"])
    out["numbered_from"] = ("high" if from_high else "low") + " end of " + axis
    out["bounds"] = bounds
    return out


def main():
    names = sys.argv[1:] or sorted(
        n[: -len(".rooms.json")] for n in os.listdir(LEVELS_DIR) if n.endswith(".rooms.json"))
    for name in names:
        pl = places_for(name)
        if pl is None:
            print("  %-10s no floor data" % name)
            continue
        with open(os.path.join(LEVELS_DIR, "%s.places.json" % name), "w", encoding="utf-8") as fh:
            json.dump(pl, fh, indent=1)
        if "divisions" in pl:
            upm = pl.get("units_per_metre")
            print("  %-10s %d %s(s), %.0f units each%s"
                  % (name, pl["divisions"], pl["division_name"], pl["units_per_division"],
                     (", %.0f units/m" % upm) if upm else ""))
        else:
            print("  %-10s extent only" % name)


main()
