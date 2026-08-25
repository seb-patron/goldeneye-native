#!/usr/bin/env python3
"""Validate and resolve the human play-nuance layer.

WHAT THIS LAYER IS FOR

The extracted data says where everything is. It does not say that a guard who spots you runs
for the alarm, that the modem has to be applied from arm's length, or that shooting the server
stacks fails the objective you came for. That is the knowledge players have and the setup file
does not, and without it a bot is accurate and lifeless -- it knows the position of every alarm
and still trips all of them.

So: facts read from community walkthroughs, rewritten as actions a bot can execute. No guide
prose is stored. Prose would not help a bot anyway; "wait by the door until the patrol passes"
is only useful once it is a verb, a target and a condition.

WHY THE VALIDATION MATTERS MORE THAN THE CONTENT

Authored data rots. A nuance entry naming tag 3 stays readable forever after tag 3 stops being
an alarm, and a JSON file full of confident sentences about the wrong props is worse than no
file at all, because nothing about it looks broken. So every reference an entry makes -- tag,
objective, navigation node -- is resolved against the generated level data, and an entry that
cannot be resolved is an error rather than a warning. The build fails loudly instead of the
bots quietly getting worse.

Entries also carry their confidence and where they came from. "verified" means the extracted
data or the game's own text supports it; "community" means it is play knowledge that the data
cannot confirm on its own. Both are useful, and a bot that wants to weight them differently
can, but only because the distinction is recorded rather than blurred.

Usage:
    python3 tools/gen_level_nuance.py --out build/levels
"""

import argparse
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NUANCE_DIR = os.path.join(ROOT, "data", "nuance")

KINDS = {"order", "timing", "positioning", "threat", "route", "weapon", "secret", "mp"}
CONFIDENCE = {"verified", "community"}

# The verb vocabulary is closed on purpose. A bot dispatches on these, so a typo that invents a
# new verb is a silently ignored instruction -- exactly the failure this file exists to prevent.
VERBS = {
    "do_last", "do_before", "do_after", "skip_below_difficulty",
    "sweep_along", "collect_enroute", "collect_during", "approach_close", "approach_from",
    "prefer_weapon", "acquire_first", "no_fire_at", "prioritise_target",
    "clear_room_before", "fight_from_chokepoint", "destroy_container_first",
    "crouch_at", "wait_for", "avoid",
    # Added as the levels demanded them. GoldenEye objectives are verbs far more varied than
    # "shoot the thing": you photograph, you plant, you escort, you copy and then discard.
    "equip_and_fire", "photograph", "plant_explosive", "shoot_lock", "use_vehicle",
    "escort_npc", "stay_close_to", "discard_after", "loot_from", "avoid_destroying",
    "expect_alarm", "exit_immediately", "beat_timer", "space_explosives",
}


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def check(level, doc, know, tact):
    """Every reference resolved against generated data. Returns (errors, resolved_entries)."""
    errs = []
    tags = know.get("tags", {})
    graph = know.get("graph", {})
    props = {p.get("tag"): p for p in know.get("props", []) if p.get("tag") is not None}
    objs = {o["objective"] for o in tact.get("objectives", [])} if tact else set()

    seen = set()
    out = []
    for e in doc.get("nuance", []):
        eid = e.get("id")
        where = "%s/%s" % (level, eid or "<no id>")
        if not eid:
            errs.append("%s: entry has no id" % level)
            continue
        if eid in seen:
            errs.append("%s: duplicate id" % where)
        seen.add(eid)

        if e.get("kind") not in KINDS:
            errs.append("%s: unknown kind %r" % (where, e.get("kind")))
        if e.get("confidence") not in CONFIDENCE:
            errs.append("%s: unknown confidence %r" % (where, e.get("confidence")))
        if not e.get("claim"):
            errs.append("%s: no claim" % where)

        action = e.get("action") or {}
        verb = action.get("verb")
        if verb not in VERBS:
            errs.append("%s: unknown verb %r (add it to VERBS if it is real)" % (where, verb))

        # A "verified" entry has to say what verifies it, or the label means nothing.
        if e.get("confidence") == "verified" and not e.get("because"):
            errs.append("%s: claims verified but gives no reason" % where)

        # Collect every tag and objective the entry names, from either applies or action.
        ref_tags, ref_objs, ref_nodes = [], [], []
        for src in (e.get("applies") or {}, action):
            for k, v in src.items():
                if k in ("tag",):
                    ref_tags.append(v)
                elif k in ("tags", "tag_group"):
                    ref_tags.extend(v)
                # "then" is an objective; "followed_by" is a next action. They were one key
                # until the checker resolved an action name as an objective number and failed
                # the build -- which is the whole point of it existing.
                elif k in ("objective", "then", "for_objective", "fails_objective"):
                    ref_objs.append(v)
                elif k in ("nav_node", "at_node"):
                    ref_nodes.append(v)

        located = {}
        for t in ref_tags:
            if str(t) not in tags:
                errs.append("%s: names tag %s, which the level does not define" % (where, t))
                continue
            p = props.get(t)
            if p is None:
                errs.append("%s: tag %s resolves to no positioned prop" % (where, t))
            else:
                located[str(t)] = {
                    "type": p.get("type"), "pos": p.get("pos"),
                    "nav_node": p.get("nav_node"), "pad_name": p.get("pad_name"),
                }
        for o in ref_objs:
            if objs and o not in objs:
                errs.append("%s: names objective %s, which the level does not have" % (where, o))
        for n in ref_nodes:
            if str(n) not in graph:
                errs.append("%s: names node %s, which is not in the graph" % (where, n))

        item = dict(e)
        if located:
            item["resolved"] = located
        out.append(item)

    return errs, out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join("build", "levels"))
    args = ap.parse_args()

    out_dir = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    files = sorted(glob.glob(os.path.join(NUANCE_DIR, "*.json")))
    if not files:
        print("no nuance files in %s" % NUANCE_DIR)
        return 0

    all_errs = 0
    for path in files:
        level = os.path.splitext(os.path.basename(path))[0]
        doc = load(path)

        kp = os.path.join(out_dir, level + ".json")
        tp = os.path.join(out_dir, level + ".tactics.json")
        if not os.path.exists(kp):
            print("%-10s SKIP  no %s -- run gen_level_knowledge.py first" % (level, kp))
            all_errs += 1
            continue
        know = load(kp)
        tact = load(tp) if os.path.exists(tp) else None

        errs, entries = check(level, doc, know, tact)
        for e in errs:
            print("  ERROR %s" % e)
        all_errs += len(errs)

        verified = sum(1 for e in entries if e.get("confidence") == "verified")
        with open(os.path.join(out_dir, level + ".nuance.json"), "w", encoding="utf-8") as fh:
            json.dump({"level": level, "notes": doc.get("notes"),
                       "counts": {"entries": len(entries), "verified": verified,
                                  "community": len(entries) - verified},
                       "nuance": entries}, fh, indent=1)
        print("%-10s entries=%-4d verified=%-4d community=%-4d %s"
              % (level, len(entries), verified, len(entries) - verified,
                 "OK" if not errs else "%d ERRORS" % len(errs)))

    return 1 if all_errs else 0


if __name__ == "__main__":
    sys.exit(main())
