#!/usr/bin/env python3
"""Extract mission briefings, objectives and status text from the game's own string banks.

WHY THIS BEATS A WALKTHROUGH
----------------------------
The obvious source of "human nuance" -- objective order, what to do first, what the traps are --
is a FAQ. But Rare wrote all of that down and it shipped inside the ROM:

    "Neutralize all alarms"
    "Install covert modem"
    "Intercept data backup"
    "Bungee jump from platform"

    "As for getting down the dam, use the bungee rope. At the bottom of the jump, use the
     piton gun. Simple."

    "Covert modem not installed. Data cannot be intercepted by MI6!"

That last one is the interesting kind: a failure message that states an ORDERING CONSTRAINT
between two objectives. The game tells you its own dependency graph, in English, and it is
first-party text rather than someone's copyrighted walkthrough.

This is a companion to gen_level_knowledge.py, which extracts the geometry -- pads, the waypoint
graph, adjacency. Together they are what a bot needs: where things are, and what it is for.

HOW THE IDS RESOLVE
-------------------
`langGet(slotID)` (src/game/language.c:379) splits the id:

    bank  = slotID >> 10
    index = slotID & 0x3FF

`bank` indexes the L* enum in src/bondconstants.h (LNULL, LAME, LARCH, LARK, ...), and each
bank is an ordinary C array of string literals in assets/obseg/text/L<name>E.c. So a slot id of
9242 is bank 9 (LCRAD, Cradle) index 26 -- verified against the Cradle setup, whose first
objective text really does carry 9242.

The setup files name their objective strings as propDefs of type 35, WatchMenuObjectiveText,
each with an index and a slot id, which is what puts the objectives in the game's own order
rather than a guessed one.

    python3 tools/gen_level_tactics.py --out build/levels
    python3 tools/gen_level_tactics.py --level dam --print
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "vendor", "ge-decomp", "assets", "obseg")
TEXT_DIR = os.path.join(ASSETS, "text")
SETUP_DIRS = [os.path.join(ASSETS, "setup", "u"), os.path.join(ASSETS, "setup")]

# The L* bank enum from src/bondconstants.h:2829-, in declaration order. The ORDINAL is the
# bank number langGet indexes with, so this list must not be reordered or padded -- it is the
# same ordinal hazard the binding tables carry, and a wrong entry here silently returns another
# level's text, which reads as plausible and is wrong.
#
# This was a hand-copied list, and it was SHORT: it stopped at LSEV (30), so every bank from 31
# up resolved to nothing and Silo, Statue, Surface 1, Surface 2 and Train shipped with blank
# objective text. Nothing looked broken -- the objectives parsed, the counts were right, the
# text was just absent. Copying an ordinal list by hand has exactly one failure mode and this
# was it, so the list is now read from the enum instead of transcribed from it.
def parse_banks():
    """The L* bank enum, read from the game's own header in declaration order."""
    path = os.path.join(ROOT, "vendor", "ge-decomp", "src", "bondconstants.h")
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = re.search(r"typedef\s+enum\s+TEXTBANK_LEVEL_INDEX\s*\{(.*?)\}\s*TEXTBANK_LEVEL_INDEX",
                  text, re.S)
    if not m:
        raise SystemExit("cannot find TEXTBANK_LEVEL_INDEX in %s" % path)
    return re.findall(r"^\s*(L\w+)\s*,?", m.group(1), re.M)


BANKS = parse_banks()

# level -> (setup stem, stage id, mission, text bank name)
LEVELS = {
    "dam":       ("Usetupdam",       33, 1,  "LDAM"),
    "facility":  ("Usetupark",       34, 2,  "LARK"),
    "runway":    ("Usetuprun",       35, 3,  "LRUN"),
    # Surface 1 and 2 were both pointed at LSEV, which the enum says is BUNKER 1. Their
    # objectives were unaffected (those resolve through each slot's own bank id) so nothing
    # looked wrong -- but their briefings and status text were Bunker 1's, which is why both
    # levels reported an identical 43 messages. LSEVX and LSEVXB are theirs.
    "surface":   ("Usetupsevx",      36, 4,  "LSEVX"),
    "bunker1":   ("Usetupsevbunker",  9, 5,  "LSEV"),
    "silo":      ("Usetupsilo",      20, 6,  "LSILO"),
    "frigate":   ("Usetupdest",      26, 7,  "LDEST"),
    "surface2":  ("Usetupsevxb",     43, 8,  "LSEVXB"),
    "bunker2":   ("Usetupsevb",      27, 9,  "LSEVB"),
    "statue":    ("Usetupstatue",    22, 10, "LSTAT"),
    "archives":  ("Usetuparch",      24, 11, "LARCH"),
    "streets":   ("Usetuppete",      29, 12, "LPETE"),
    "depot":     ("Usetupdepo",      30, 13, "LDEPO"),
    "train":     ("Usetuptra",       25, 14, "LTRA"),
    "jungle":    ("Usetupjun",       37, 15, "LJUN"),
    "control":   ("Usetupcontrol",   23, 16, "LAREC"),
    "caverns":   ("Usetupcave",      39, 17, "LCAVE"),
    "cradle":    ("Usetupcrad",      41, 18, "LCRAD"),
    "aztec":     ("Usetupazt",       28, 19, "LAZT"),
    "egypt":     ("Usetupcryp",      32, 20, "LCRYP"),
}

# The objectives are ObjectiveStart records (propDef type 23), not WatchMenuObjectiveText.
#
# That distinction cost an hour and is worth writing down: WatchMenuObjectiveText (type 35)
# resolves to the LEVEL TITLE -- Dam's points at "B Y E L O M O R Y E D A M" and Cradle's at
# "A N T E N N A C R A D L E". Both parse cleanly and look like objectives until you read
# them, which is exactly the sort of wrong that ships.
#
#  /* Type = ObjectiveStart; index = 5 */
#  _mkword(0, _mkshort(0, 23)), 0, 11286, 1,
#  ^objnum ^textid ^min difficulty
#
# The fourth field is the minimum difficulty: objectives are cumulative supersets filtered by
# `MinDificulty <= selected`, so 0 appears on Agent and 2 only on 00 Agent. That is real
# tactical information -- it says which objectives exist at all at a given difficulty.
OBJ_START_TYPE = "ObjectiveStart"
TITLE_TYPE = "WatchMenuObjectiveText"

DIFFICULTY = {0: "agent", 1: "secret agent", 2: "00 agent"}


def find_setup(stem):
    for d in SETUP_DIRS:
        p = os.path.join(d, stem + "Z.c")
        if os.path.exists(p):
            return p
    return None


def find_text(bank):
    """LDAM -> assets/obseg/text/LdamE.c. English only; the J files are the same ids."""
    if not bank:
        return None
    name = "L" + bank[1:].lower() + "E.c"
    p = os.path.join(TEXT_DIR, name)
    return p if os.path.exists(p) else None


def parse_strings(path):
    """char *LdamE[] = { "...", "...", 0, ... };  ->  list, with None for the 0 entries.

    C escapes are decoded so the JSON carries real newlines rather than backslash-n, which
    matters because several strings are two lines and the second line is usually the useful
    half ("...use the piton gun. Simple.")."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = re.search(r"char\s*\*\s*\w+\s*\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return []
    body = m.group(1)
    out = []
    # Walk entries in order: either a quoted string (possibly adjacent-concatenated) or a bare 0.
    for tok in re.finditer(r'((?:"(?:[^"\\]|\\.)*"\s*)+)|(\b0\b)', body):
        if tok.group(2):
            out.append(None)
            continue
        raw = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', tok.group(1)))
        out.append(raw.replace("\\n", "\n").replace('\\"', '"').replace("\\\\", "\\"))
    return out


def _records(setup_text, type_name):
    """Every propDef of one type, in file order.

    The comment carries the type and the propDef index; the line under it carries the payload.
    Anchoring on `))` skips the _mkword(0, _mkshort(0, NN)) blob without trying to parse its
    nested parens, and the three integers after it are the record's remaining words."""
    pat = re.compile(
        r"/\*\s*Type\s*=\s*" + re.escape(type_name) + r"\s*;\s*index\s*=\s*(\d+)\s*\*/[^\n]*\n"
        r"\s*[^\n]*?\)\)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)")
    out = []
    for mm in pat.finditer(setup_text):
        out.append((int(mm.group(1)), int(mm.group(2)), int(mm.group(3)), int(mm.group(4))))
    return out


def parse_objectives(setup_text):
    """ObjectiveStart records: (propdef index, objective number, text slot, min difficulty)."""
    return [{"propdef": p, "objective": o, "slot": t, "min_difficulty": d,
             "difficulty_name": DIFFICULTY.get(d, str(d))}
            for (p, o, t, d) in _records(setup_text, OBJ_START_TYPE)]


# the completion flags are off by eight bits, and it is not yet known whose fault that is.
#
# 48 of the game's 80 objectives have no tagged target, so they cannot be routed to. Their
# completion is a flag instead, and the chain that would resolve them is:
#
#  objective's complete flag -> the AI list that sets it -> what that list is attached to -> a
#  position a bot can walk to
#
# The machinery is all there. objective_status.c evaluates a complete condition with
# chrHasStageFlag, which reads objectiveregisters1 (chraction.c:10053). That register has
# exactly one writer, chrSetStageFlags, called only from the AI opcode SetObjectiveBitfield. So
# every completion flag in the game is set by a line of AI bytecode, which is readable.
#
# The obstacle: the numbers do not line up. Dam's objectives test 0x100, 0x200, 0x400, 0x800,
# 0x1000, 0x2000 while its AI lists set 0x10000 through 0x200000 -- EXACTLY A FACTOR of 256.
# Across the levels that have both, 18 of 27 objective flags equal an AI-set flag shifted eight
# bits; Dam is 6 of 6, Bunker 1 is 5 of 5, Depot 1 of 1, and Caverns 0 of 7.
#
# That is far too consistent to be coincidence and too incomplete to paper over. Either this
# parser reads the ObjRefID at the wrong byte offset, or the two sides genuinely use different
# conventions and Caverns is a third case. Shifting the value to make the numbers agree would be
# fitting to a pattern rather than explaining it, and the same instinct has produced three wrong
# answers already this session.
#
# Settling it needs the AiSetObjectiveBitfieldRecord and the complete-condition record laid side
# by side, byte for byte. GETV_OBJ_DEBUG=1 already exists in chrai.c and dumps exactly that at
# runtime -- which needs a build, and is the cheapest way to know rather than guess.
def parse_objective_conditions(setup_text):
    """What each objective actually requires, from the records nested inside it.

    The stream is nested rather than flat: ObjectiveStart(23), then one record per requirement,
    then ObjectiveEnd(24). ObjectiveDestroyObject names a TAG, not a prop -- and a tag resolves
    to a prop, a pad and a position through the tag table (see gen_level_knowledge.parse_tags).
    That is the whole objective -> place chain, and this is the step that carries the target.

    Verified on Dam: objective 0 holds four ObjectiveDestroyObject records naming tags 0,1,2,3,
    which resolve to Dam's four Alarm props -- "neutralize all alarms", stated by the data.
    """
    rec = re.compile(
        r"/\*\s*Type\s*=\s*(\w+)\s*;\s*index\s*=\s*(\d+)\s*\*/[^\n]*\n\s*[^\n]*?\)\)\s*([^\n]*)")
    out, cur = {}, None
    for mm in rec.finditer(setup_text):
        kind, payload = mm.group(1), mm.group(3)
        nums = [int(t) for t in re.findall(r"-?\d+", payload)]
        if kind == "ObjectiveStart":
            cur = {"objective": nums[0] if nums else None,
                   "destroy_tags": [], "target_tags": [], "target_kinds": [],
                   "rooms": [], "complete_flags": [], "fail_flags": []}
        elif kind == "ObjectiveEnd":
            if cur is not None and cur["objective"] is not None:
                out[cur["objective"]] = cur
            cur = None
        elif cur is not None and nums:
            if kind == "ObjectiveDestroyObject":
                cur["destroy_tags"].append(nums[0])
                cur["target_tags"].append(nums[0])
            # Collect, photograph and copy all name a TAG in their first operand, exactly as
            # destroy does. Reading only destroy left most objectives with no target at all --
            # 62 of 80 -- and so unroutable, when the target was sitting in the record.
            elif kind in ("ObjectiveCollectObject", "ObjectivePhotograph", "ObjectiveCopy_Item"):
                cur["target_tags"].append(nums[0])
                cur["target_kinds"].append(kind)
            # EnterRoom names a ROOM rather than a tag, so it cannot resolve through the tag
            # table. Recorded as what it is instead of silently dropped.
            elif kind == "ObjectiveEnterRoom":
                cur["rooms"].append(nums[0])
            elif kind == "ObjectiveCompleteCondition":
                cur["complete_flags"].append(nums[0])
            elif kind == "ObjectiveFailCondition":
                cur["fail_flags"].append(nums[0])
    return out


def parse_title_slot(setup_text):
    """WatchMenuObjectiveText -> the level title. Useful, just not the objectives."""
    recs = _records(setup_text, TITLE_TYPE)
    return recs[0][2] if recs else None


def resolve(slot, banks_cache):
    """slotID -> (bank name, index, string or None). langGet, language.c:379."""
    bank_no = slot >> 10
    idx = slot & 0x3FF
    if bank_no < 0 or bank_no >= len(BANKS):
        return None, idx, None
    name = BANKS[bank_no]
    if name not in banks_cache:
        p = find_text(name)
        banks_cache[name] = parse_strings(p) if p else []
    arr = banks_cache[name]
    s = arr[idx] if 0 <= idx < len(arr) else None
    return name, idx, s


def build(level, stem, stage_id, mission, bank, banks_cache):
    setup = find_setup(stem)
    if setup is None:
        return None
    with open(setup, encoding="utf-8", errors="replace") as fh:
        setup_text = fh.read()

    objectives = []
    for s in parse_objectives(setup_text):
        bname, idx, text = resolve(s["slot"], banks_cache)
        objectives.append({
            "objective": s["objective"],
            "slot": s["slot"],
            "bank": bname,
            "bank_index": idx,
            "min_difficulty": s["min_difficulty"],
            "difficulty_name": s["difficulty_name"],
            # Absent rather than empty when the bank has no such string: an objective silently
            # reported as "" is indistinguishable from one the game genuinely leaves blank.
            "text": text.strip() if text else None,
        })
    objectives.sort(key=lambda o: o["objective"])

    # Attach each objective's requirements. destroy_tags is the link out to geometry: a tag
    # resolves to a prop, a prop to a pad, a pad to a position and a navigation node.
    conditions = parse_objective_conditions(setup_text)
    for o in objectives:
        c = conditions.get(o["objective"])
        if c:
            o["destroy_tags"]    = c["destroy_tags"]
            o["target_tags"]     = c["target_tags"]
            o["target_kinds"]    = c["target_kinds"]
            o["rooms"]           = c["rooms"]
            o["complete_flags"]  = c["complete_flags"]
            o["fail_flags"]      = c["fail_flags"]

    title_slot = parse_title_slot(setup_text)
    _, _, title = resolve(title_slot, banks_cache) if title_slot else (None, None, None)

    strings = banks_cache.get(bank) if bank else None
    if strings is None and bank:
        p = find_text(bank)
        strings = parse_strings(p) if p else []
        banks_cache[bank] = strings
    strings = strings or []

    # The first non-empty strings in a level bank are the briefing pages: MI6 background, the
    # tactical brief (Q), and Moneypenny. Rare kept that order across every level, but it is a
    # convention rather than a guarantee, so they are labelled by position and not asserted.
    briefing = [s.strip() for s in strings[:4] if s]

    # Everything else, minus the objective lines themselves, is status and failure text. The
    # failure lines are the valuable ones -- they state dependencies between objectives.
    obj_texts = {o["text"] for o in objectives if o["text"]}
    messages = [s.strip() for s in strings[4:] if s and s.strip() not in obj_texts]

    return {
        "level": level,
        "stage_id": stage_id,
        "mission": mission,
        "text_bank": bank,
        "counts": {
            "objectives": len(objectives),
            "objectives_resolved": sum(1 for o in objectives if o["text"]),
            "briefing_pages": len(briefing),
            "messages": len(messages),
        },
        "briefing": briefing,
        "objectives": objectives,
        "messages": messages,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "levels"))
    ap.add_argument("--level")
    ap.add_argument("--print", dest="show", action="store_true",
                    help="print the level instead of writing it")
    args = ap.parse_args()

    if not os.path.isdir(TEXT_DIR):
        print("error: text assets not found at %s" % TEXT_DIR, file=sys.stderr)
        print("       vendor/ is gitignored; generate the assets on this machine first.",
              file=sys.stderr)
        return 2

    wanted = {args.level: LEVELS[args.level]} if args.level else LEVELS
    if args.level and args.level not in LEVELS:
        print("error: unknown level %r" % args.level, file=sys.stderr)
        return 2

    banks_cache = {}
    if not args.show:
        os.makedirs(args.out, exist_ok=True)

    unresolved = []
    for name, (stem, sid, mis, bank) in sorted(wanted.items(), key=lambda kv: kv[1][2]):
        doc = build(name, stem, sid, mis, bank, banks_cache)
        if doc is None:
            print("%-10s setup not found" % name)
            continue
        c = doc["counts"]
        if args.show:
            print(json.dumps(doc, indent=1))
            continue
        with open(os.path.join(args.out, name + ".tactics.json"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1, ensure_ascii=False)
        print("%-10s objectives=%d/%d  briefing=%d  messages=%-3d %s"
              % (name, c["objectives_resolved"], c["objectives"], c["briefing_pages"],
                 c["messages"], "" if doc["text_bank"] else "(no text bank mapped)"))
        if c["objectives"] and not c["objectives_resolved"]:
            unresolved.append(name)

    if unresolved:
        # Named, because an objective list that resolved to nothing is the failure mode that
        # looks like success -- the JSON is still well-formed and every text field is null.
        print("\nOBJECTIVES DID NOT RESOLVE (wrong bank, or text asset absent): %s"
              % ", ".join(unresolved))
    return 0


if __name__ == "__main__":
    sys.exit(main())
