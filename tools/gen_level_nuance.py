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

KINDS = {"order", "timing", "positioning", "threat", "route", "weapon", "secret", "mp",
         "mode", "character", "meta", "control"}
CONFIDENCE = {"verified", "community"}

# The multiplayer scenarios, by their in-game names. Closed for the same reason the verbs are:
# an entry that applies to a mode nobody dispatches on is an entry that never fires.
MODES = {
    "normal", "you_only_live_twice", "the_living_daylights", "the_man_with_the_golden_gun",
    "licence_to_kill", "any",
}

# The selectable weapon sets. A great deal of multiplayer knowledge is set-specific rather than
# arena-specific -- proximity mines change a map more than the map changes them.
WEAPON_SETS = {
    "slappers_only", "pistols", "throwing_knives", "automatics", "power_weapons",
    "sniper_rifles", "grenades", "remote_mines", "grenade_launchers", "timed_mines",
    "proximity_mines", "rockets", "lasers", "golden_gun", "any",
}

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
    # Multiplayer. Arena play is a different game from the campaign: nothing is being completed,
    # so the verbs are about denial, control and reading an opponent rather than about progress.
    "camp_at", "control_area", "deny_spawn", "rotate_between", "contest_pickup",
    "break_line_of_sight", "lean_from_cover", "listen_for", "mine_chokepoint", "bait_into",
    "pick_character", "disable_radar", "hold_high_ground", "hold_low_ground",
    "strafe_unpredictably", "aim_manually", "flee_unarmed", "hoard_pickup", "deny_pickup",
}


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def check(level, doc, know, tact, arena=False):
    """Every reference resolved against generated data. Returns (errors, resolved_entries).

    `arena` marks a multiplayer stage. Arenas have no objectives at all, so an entry that names
    one is not merely unresolvable -- it is a campaign habit that leaked into a deathmatch file,
    and it fails rather than passing quietly the way an empty objective set would.
    """
    errs = []
    know = know or {}
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

        applies = e.get("applies") or {}
        mode = applies.get("mode")
        if mode is not None and mode not in MODES:
            errs.append("%s: unknown mode %r" % (where, mode))
        wset = applies.get("weapon_set")
        if wset is not None and wset not in WEAPON_SETS:
            errs.append("%s: unknown weapon set %r" % (where, wset))
        if not arena and (mode or wset):
            errs.append("%s: names a multiplayer mode or weapon set in a campaign file" % where)

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
            if arena:
                errs.append("%s: names objective %s, but a multiplayer arena has none" % (where, o))
            elif objs and o not in objs:
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
    # Campaign files sit in data/nuance; multiplayer files in data/nuance/mp. They are kept
    # apart because they are genuinely different data: a campaign entry is about completing
    # something, an arena entry is about denying it to somebody else. Several stages appear in
    # both, and a shared file would have to pretend those are the same knowledge.
    files = ([(p, False) for p in sorted(glob.glob(os.path.join(NUANCE_DIR, "*.json")))] +
             [(p, True) for p in sorted(glob.glob(os.path.join(NUANCE_DIR, "mp", "*.json")))])
    if not files:
        print("no nuance files in %s" % NUANCE_DIR)
        return 0

    all_errs = 0
    for path, arena in files:
        level = os.path.splitext(os.path.basename(path))[0]
        doc = load(path)
        label = ("mp/" + level) if arena else level

        # A converted stage has BOTH a campaign file and an arena file, with different pickups,
        # spawns and props. An arena entry must be checked against the arena data or it is being
        # validated against a stage nobody plays in that mode.
        kp = os.path.join(out_dir, level + (".mp.json" if arena else ".json"))
        if arena and not os.path.exists(kp):
            kp = os.path.join(out_dir, level + ".json")
        tp = os.path.join(out_dir, level + ".tactics.json")
        # A leading underscore marks arena knowledge that is not tied to one stage -- aiming,
        # spawn behaviour, mode rules. There is no level data to check it against, and that is
        # legitimate rather than a missing file.
        shared = arena and level.startswith("_")
        if not shared and not os.path.exists(kp):
            print("%-12s SKIP  no %s -- run gen_level_knowledge.py first" % (label, kp))
            all_errs += 1
            continue
        know = load(kp) if os.path.exists(kp) else None
        tact = load(tp) if os.path.exists(tp) else None

        errs, entries = check(label, doc, know, tact, arena=arena)
        for e in errs:
            print("  ERROR %s" % e)
        all_errs += len(errs)

        verified = sum(1 for e in entries if e.get("confidence") == "verified")
        # Facility, Archives, Caverns, Egypt and the Bunkers are BOTH campaign missions and
        # multiplayer arenas. Writing arena output to <level>.nuance.json would have one silently
        # overwrite the other depending on which was processed last.
        suffix = ".mp-nuance.json" if arena else ".nuance.json"
        with open(os.path.join(out_dir, level + suffix), "w", encoding="utf-8") as fh:
            json.dump({"level": level, "arena": arena, "notes": doc.get("notes"),
                       "counts": {"entries": len(entries), "verified": verified,
                                  "community": len(entries) - verified},
                       "nuance": entries}, fh, indent=1)
        print("%-12s entries=%-4d verified=%-4d community=%-4d %s"
              % (label, len(entries), verified, len(entries) - verified,
                 "OK" if not errs else "%d ERRORS" % len(errs)))

    return 1 if all_errs else 0


if __name__ == "__main__":
    sys.exit(main())
