#!/usr/bin/env python3
"""Validate the bot archetype definitions against the game's own AI instruction set.

WHY THIS EXISTS

Perfect Dark's simulants are the reference design for what we are building, and the useful
discovery is that we do not have to reimplement them. GoldenEye already ships the same machine:
a 225-opcode AI scripting VM (src/bondaicommands.h), driven per character by bytecode lists in
every setup file, with named opcodes for exactly the dials Rare later exposed as simulant
options -- guard_set_accuracy_rating, guard_set_speed_rating, guard_set_armour,
guard_set_hearing_scale, plus conditionals on health, ammunition, sight, hearing and a random
seed.

So an archetype here is not a description of a bot. It is a specification that names real
opcodes, and the whole point of this tool is that those names are checked. An archetype
referencing an opcode that does not exist is a bot that silently does nothing at all, which is
the worst possible failure for behaviour code: it looks implemented.

WHAT IS CHECKED

  - every opcode named by an archetype exists in bondaicommands.h
  - dials come from a closed vocabulary, and their values are in range
  - target selection and weapon policy come from closed vocabularies
  - skill tiers are ordered and unique, so "harder than" is meaningful
  - each archetype says which game it came from, so borrowed design is attributable

Usage:
    python3 tools/gen_bot_archetypes.py --out build/bots
"""

import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOTS_DIR = os.path.join(ROOT, "data", "bots")
AI_HEADER = os.path.join(ROOT, "vendor", "ge-decomp", "src", "bondaicommands.h")

# The dials a bot archetype may turn. Closed, because each one has to correspond to something
# the engine can actually do -- an invented dial is a knob wired to nothing.
DIALS = {
    "accuracy":   (0, 100),
    "speed":      (0, 200),
    "health":     (0, 400),
    "armour":     (0, 400),
    "hearing":    (0, 200),
    "aggression": (0, 100),
}

TARGETING = {
    "nearest", "weakest", "leader", "last_attacker", "fixed_rival",
    "random", "none", "strongest",
}

WEAPON_POLICY = {"best_available", "melee_only", "explosive_only", "disarm_only", "none"}


def parse_opcodes():
    """Every AI opcode name the game defines.

    Both spellings are accepted because the header uses both: most opcodes are function-like
    macros, but the terminators are bare defines. Matching only the parenthesised form would
    reject ai_list_end, which is in every single AI list in the game.
    """
    if not os.path.exists(AI_HEADER):
        raise SystemExit("cannot find %s (vendor/ is gitignored -- generate it first)" % AI_HEADER)
    with open(AI_HEADER, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    return set(re.findall(r"^#define\s+([a-z_][a-z_0-9]*)\s*[\(\s]", text, re.M))


def check(doc, opcodes, path):
    errs, seen, tiers = [], set(), []
    kind = doc.get("kind")
    if kind not in ("skill_tiers", "personalities"):
        errs.append("%s: unknown kind %r" % (path, kind))

    for a in doc.get("archetypes", []):
        name = a.get("name")
        where = "%s/%s" % (kind, name or "<unnamed>")
        if not name:
            errs.append("%s: archetype with no name" % path)
            continue
        if name in seen:
            errs.append("%s: duplicate name" % where)
        seen.add(name)

        if not a.get("summary"):
            errs.append("%s: no summary" % where)
        if not a.get("origin"):
            errs.append("%s: no origin -- say where the design came from" % where)

        for dial, value in (a.get("dials") or {}).items():
            if dial not in DIALS:
                errs.append("%s: unknown dial %r" % (where, dial))
                continue
            lo, hi = DIALS[dial]
            if not isinstance(value, (int, float)) or not (lo <= value <= hi):
                errs.append("%s: dial %s out of range: %r (expected %d..%d)"
                            % (where, dial, value, lo, hi))

        t = a.get("targeting")
        if t is not None and t not in TARGETING:
            errs.append("%s: unknown targeting %r" % (where, t))
        w = a.get("weapon_policy")
        if w is not None and w not in WEAPON_POLICY:
            errs.append("%s: unknown weapon policy %r" % (where, w))

        # The part that actually matters. A misspelled opcode is a behaviour that never runs.
        ops = a.get("opcodes") or []
        if not ops:
            errs.append("%s: names no opcodes, so nothing implements it" % where)
        for op in ops:
            if op not in opcodes:
                errs.append("%s: opcode %r is not defined by the game" % (where, op))

        if kind == "skill_tiers":
            r = a.get("rank")
            if not isinstance(r, int):
                errs.append("%s: skill tier needs an integer rank" % where)
            else:
                tiers.append((r, name))

    if kind == "skill_tiers":
        ranks = [r for r, _ in tiers]
        if len(set(ranks)) != len(ranks):
            errs.append("%s: skill tier ranks are not unique, so ordering is meaningless" % path)

    return errs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join("build", "bots"))
    args = ap.parse_args()

    out_dir = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(out_dir, exist_ok=True)

    opcodes = parse_opcodes()
    print("game defines %d AI opcodes" % len(opcodes))

    files = sorted(glob.glob(os.path.join(BOTS_DIR, "*.json")))
    if not files:
        print("no archetype files in %s" % BOTS_DIR)
        return 0

    total_errs = 0
    for path in files:
        base = os.path.splitext(os.path.basename(path))[0]
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        errs = check(doc, opcodes, base)
        for e in errs:
            print("  ERROR %s" % e)
        total_errs += len(errs)

        used = sorted({op for a in doc.get("archetypes", []) for op in (a.get("opcodes") or [])})
        doc["opcodes_used"] = used
        doc["counts"] = {"archetypes": len(doc.get("archetypes", [])), "opcodes_used": len(used)}
        with open(os.path.join(out_dir, base + ".json"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1)
        print("%-14s archetypes=%-3d opcodes=%-3d %s"
              % (base, doc["counts"]["archetypes"], len(used),
                 "OK" if not errs else "%d ERRORS" % len(errs)))

    return 1 if total_errs else 0


if __name__ == "__main__":
    sys.exit(main())
