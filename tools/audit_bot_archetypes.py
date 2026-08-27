#!/usr/bin/env python3
"""Phase 2: are the bot archetypes actually playable on the levels we measured?

Eighteen archetypes exist and were only ever exercised against a placeholder arena. Their dials --
aggression, speed, accuracy, hearing -- are abstract numbers, and an abstract number cannot be
wrong. This checks them against MEASURED level structure instead, so a personality's requirements
either hold on a real level or do not.

THE CHECK THAT MATTERS: A BEHAVIOUR NEEDS SOMEWHERE TO HAPPEN. An archetype that breaks contact
and runs needs somewhere to run TO. Train is measured at topology=linear, branching=0,
lateral_escape=false, and a 53:1 corridor ratio along one axis (docs/PERFORMANCE.md, S2). On that
level "retreat" is not a strategy, it is walking backwards down a tube. A sniper archetype wants
long sightlines and gets them there; a flanker wants alternatives and has none.

That is the difference between a personality and a number: the number is always satisfiable, the
behaviour is not.

REQUIREMENTS ARE INFERRED FROM THE ARCHETYPE'S OWN FIELDS, not hand-assigned per level. A
`weapon_policy` of disarm_only implies it must reach weapons it does not fight for; high aggression
with low accuracy implies it must close distance. Inferring them keeps this honest when an
archetype changes -- a hand-written table would drift silently.

AND A FAILED CHECK IS A FACT ABOUT THE PAIRING, NOT A DEFECT IN EITHER. "PeaceSim is unplayable
on Train" is useful: it says do not evaluate that archetype there and do not conclude the archetype
is broken when it loses. Nothing here edits an archetype.
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def level_shape(levels, lv):
    """Measured structure for one level, from our own extraction only."""
    shape = {"level": lv}
    r = os.path.join(levels, "%s.rooms.json" % lv)
    if os.path.isfile(r):
        d = json.load(open(r, encoding="utf-8"))
        fl = d.get("floors") or []
        if fl:
            xs = [f["c"][0] for f in fl]
            zs = [f["c"][2] for f in fl]
            dx, dz = max(xs) - min(xs), max(zs) - min(zs)
            shape["tiles"] = len(fl)
            shape["rooms"] = len({f.get("r") for f in fl})
            shape["ratio"] = round(max(dx, dz) / max(1.0, min(dx, dz)), 1)
            # Mean adjacency is our proxy for route choice: a tile with many neighbours sits in
            # open ground, one with two sits in a corridor.
            deg = [len(f.get("l", [])) for f in fl]
            shape["mean_adjacency"] = round(sum(deg) / len(deg), 1)
    n = os.path.join(levels, "%s.nodes.json" % lv)
    if os.path.isfile(n):
        d = json.load(open(n, encoding="utf-8"))
        shape["doors"] = d.get("counts", {}).get("door", 0)
        shape["waypoints"] = d.get("counts", {}).get("waypoint", 0)

    # Measured occlusion and sightlines, from ray-casting the wall set.
    v = os.path.join(levels, "_visibility.json")
    if os.path.isfile(v):
        rec = json.load(open(v, encoding="utf-8")).get("levels", {}).get(lv)
        if rec:
            shape["vis"] = rec
            shape["sight_median"] = rec["sight_median"]
            shape["cover_mean"] = rec["cover_mean"]

    # Claimed topology, from the walkthrough documents. Marked as a claim: its distances are known
    # wrong (1.46x), but its topology has not been contradicted by anything we measured.
    lf = os.path.join(levels, "_level.facts.json")
    if os.path.isfile(lf):
        d = json.load(open(lf, encoding="utf-8"))
        rec = d.get("by_level", {}).get(lv)
        if rec:
            for b in rec.get("claims", []):
                c = b.get("claim", {})
                for k in ("topology", "branching", "lateral_escape"):
                    if k in c and k not in shape:
                        shape["claimed_" + k] = c[k]
    return shape


def requirements(a):
    """What an archetype needs from a level, inferred from its own fields."""
    req = set()
    d = a.get("dials", {}) or {}
    agg = d.get("aggression")
    acc = d.get("accuracy")
    pol = (a.get("weapon_policy") or "").lower()
    tgt = (a.get("targeting") or "").lower()
    beh = " ".join(a.get("behaviour", [])).lower() + " " + (a.get("summary") or "").lower()

    if "run" in beh or "flee" in beh or "avoid" in beh or (agg is not None and agg <= 20):
        req.add("escape_routes")
    if "disarm" in pol or "strip" in beh or "collect" in beh:
        req.add("multiple_pickups")
    if agg is not None and acc is not None and agg >= 60 and acc <= 40:
        req.add("close_distance")
    if "snipe" in beh or (acc is not None and acc >= 80):
        req.add("long_sightlines")
    if "flank" in beh or "ambush" in beh or "circle" in beh:
        req.add("alternate_paths")
    if "cover" in beh:
        req.add("cover")
    return req


def satisfies(shape, need):
    """Does the measured level provide it? None = cannot tell from what we have."""
    ratio = shape.get("ratio")
    adj = shape.get("mean_adjacency")
    branching = shape.get("claimed_branching")
    lateral = shape.get("claimed_lateral_escape")

    if need in ("escape_routes", "alternate_paths"):
        if branching == 0 or lateral is False:
            return False
        if ratio is not None and ratio >= 8:
            return False           # a corridor: the only retreat is back down it
        if adj is not None:
            return adj >= 6.0
        return None
    if need == "close_distance":
        return True                # always possible; distance can be closed on any topology
    if need == "multiple_pickups":
        return None                # needs a prop-kind census per level; not wired here

    # cover and long_sightlines are now MEASURED, by ray-casting the derived wall set
    # (tools/gen_level_visibility.py), rather than inferred from a proxy.
    #
    # THE PROXIES THEY REPLACE WERE MEASURING THE WRONG QUANTITY, which is why they are gone
    # rather than merely tuned. `cover` was mean tile adjacency < 8 -- adjacency is mesh
    # CONNECTIVITY, so a room full of crates scored like an empty one. `long_sightlines` was aspect
    # ratio >= 3 -- ratio is ELONGATION, so a large square hall failed and a narrow corridor
    # passed. Across bunker1, archives and dam they returned identical verdicts, which is how they
    # were caught: a check that cannot distinguish three different levels is not testing anything.
    vis = shape.get("vis")
    if need == "long_sightlines":
        if not vis:
            return None
        # Measured medians: Train 4000 (capped -- you see clear down the train), bunker1 262. The
        # threshold sits far above the warren levels and far below the corridor ones, so it is not
        # doing fine discrimination it has not earned.
        return vis["sight_median"] >= 800.0
    if need == "cover":
        if not vis:
            return None
        # Fraction of directions blocked within a short radius. Train 0.57, bunker1 0.76.
        return vis["cover_mean"] >= 0.35
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    ap.add_argument("--bots", default=os.path.join(ROOT, "build", "bots"))
    ap.add_argument("--level", default="train")
    a = ap.parse_args()

    arch = []
    for f in ("personalities.json", "skill_tiers.json"):
        p = os.path.join(a.bots, f)
        if os.path.isfile(p):
            arch += json.load(open(p, encoding="utf-8")).get("archetypes", [])
    if not arch:
        sys.exit("no archetypes -- run tools/gen_bot_archetypes.py first")

    shape = level_shape(a.levels, a.level)
    print("%s: %s" % (a.level, ", ".join("%s=%s" % (k, v) for k, v in shape.items() if k != "level")))
    print("\n%-14s %-22s %s" % ("archetype", "needs", "verdict"))

    unplayable = []
    for x in arch:
        req = requirements(x)
        if not req:
            continue
        bad = [n for n in sorted(req) if satisfies(shape, n) is False]
        unk = [n for n in sorted(req) if satisfies(shape, n) is None]
        if bad:
            verdict = "UNPLAYABLE: no " + ", ".join(bad)
            unplayable.append((x["name"], bad))
        elif unk and len(unk) == len(req):
            verdict = "cannot tell"
        else:
            verdict = "playable"
        print("   %-12s %-22s %s" % (x["name"][:12], ",".join(sorted(req))[:22], verdict))

    print("\n%d of %d archetypes with requirements are UNPLAYABLE on %s"
          % (len(unplayable), len([x for x in arch if requirements(x)]), a.level))
    if unplayable:
        print("   Not a defect in either. It means: do not evaluate these here, and do not read")
        print("   a loss on this level as the archetype being broken.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
