#!/usr/bin/env python3
"""Score the documents' COUNT claims against the setup data.

The documents' distances are already known wrong -- gen_level_facts.py measured their metre at
1.46x the engine's. Counts are the other kind of number they assert, and unlike a distance a count
is checkable exactly: "three guards" either matches the Guard records in that level's setup file or
it does not. This is the pass that decides whether the corpus is trustworthy on anything numeric.

A CLAIM ON `_engine` OR `_mp` IS NOT CHECKABLE AND IS NOT COUNTED AGAINST THE DOCUMENTS. Those
buckets are engine-wide and multiplayer-wide prose; "twenty guards" there is not a statement about
any particular level's setup file, so scoring it would be inventing a disagreement. Reported
separately rather than dropped, because how many claims are unanchored is itself worth knowing.

AND THE NOUN MAP IS DELIBERATELY NARROW. guard, door, room, alarm, camera and objective have
exact counterparts in data we extract. `car`, `tank`, `brake`, `floor` and `level` do not -- a
Train carriage is not a record type -- so they are reported UNMAPPED rather than approximated. A
wrong mapping would manufacture agreement or disagreement out of nothing, which is worse than
admitting the claim cannot be reached.
"""
import argparse
import collections
import re
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# noun -> how to count it from our own extraction
NOUN_MAP = {
    "guard": ("prop", "Guard"), "guards": ("prop", "Guard"),
    "enemy": ("prop", "Guard"), "enemies": ("prop", "Guard"),
    "door": ("prop", "Door"), "doors": ("prop", "Door"),
    "doorway": ("prop", "Door"), "doorways": ("prop", "Door"),
    "alarm": ("prop", "Alarm"), "alarms": ("prop", "Alarm"),
    "camera": ("prop", "Cctv"), "cameras": ("prop", "Cctv"),
    "objective": ("objectives", None), "objectives": ("objectives", None),
    "room": ("rooms", None), "rooms": ("rooms", None),
}


# Cues that a sentence is describing the WHOLE level rather than one spot. "the four alarms are:"
# introduces a complete list; "there are two guards inside" does not. Deliberately conservative --
# a claim wrongly treated as local is merely unscored, while one wrongly treated as total produces
# a false disagreement and impugns the source.
SCOPE_CUES = re.compile(
    # `the (\w+ ){1,3}(are|is):` allows one to three words, not one. Written with a single \w+
    # it failed on "The four alarms are:" -- two words, "four" and "alarms" -- which is the ONE
    # level-wide claim in the whole corpus that had already been shown to be correct. A scope
    # filter that rejects the only true positive is worse than no filter, and it reported a
    # confident zero.
    r"\b(in total|altogether|all (?:of )?the|every|the level (?:has|contains)|"
    r"level contains|throughout the level|across the level|there are only|"
    r"the (?:\w+\s+){1,3}(?:are|is):)", re.I)

_TEXT_CACHE = {}


def level_scoped(levels, lv, line_no):
    """Does the claim's own line mark it as a whole-level statement?"""
    if lv not in _TEXT_CACHE:
        p = os.path.join(levels, "%s.walkthrough.json" % lv)
        lines = []
        if os.path.isfile(p):
            d = json.load(open(p, encoding="utf-8"))
            for doc in d.get("documents", []):
                lines = doc.get("text", "").splitlines()
                break        # counts carry a line number into the first document
        _TEXT_CACHE[lv] = lines
    lines = _TEXT_CACHE[lv]
    if not (1 <= line_no <= len(lines)):
        return False
    return bool(SCOPE_CUES.search(lines[line_no - 1]))


def level_counts(levels, lv):
    """What our extraction says the level actually contains."""
    out = {}
    k = os.path.join(levels, "%s.json" % lv)
    if os.path.isfile(k):
        d = json.load(open(k, encoding="utf-8"))
        byt = collections.Counter(p.get("type") for p in d.get("props", []))
        out["prop"] = byt
        g = d.get("counts", {})
        out["objectives"] = g.get("objectives") if isinstance(g, dict) else None
    r = os.path.join(levels, "%s.rooms.json" % lv)
    if os.path.isfile(r):
        fl = json.load(open(r, encoding="utf-8")).get("floors") or []
        if fl:
            out["rooms"] = len({f.get("r") for f in fl})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    ap.add_argument("--tolerance", type=float, default=0.25,
                    help="fractional slack before a claim counts as disagreeing")
    a = ap.parse_args()

    ent = os.path.join(a.levels, "_entities.json")
    if not os.path.isfile(ent):
        sys.exit("no _entities.json -- run tools/gen_level_entities.py first")
    doc = json.load(open(ent, encoding="utf-8"))

    agree, disagree, unmapped, unanchored, nodata, local = [], [], [], [], [], []
    for lv, rec in doc.get("by_level", {}).items():
        anchored = not lv.startswith("_")
        actual = level_counts(a.levels, lv) if anchored else {}
        for c in rec.get("counts", []):
            claim = {"level": lv, "n": c["n"], "of": c["of"], "line": c["line"]}
            if not anchored:
                unanchored.append(claim)
                continue
            m = NOUN_MAP.get(c["of"])
            if not m:
                unmapped.append(claim)
                continue
            kind, sub = m
            if kind == "prop":
                have = actual.get("prop", {}).get(sub)
            else:
                have = actual.get(kind)
            if have is None:
                nodata.append(claim)
                continue
            # SCOPE FIRST. A bare "two guards" in prose is almost always a LOCAL observation --
            # "there are two guards positioned inside" -- not a census of the level. Comparing it
            # against a level-wide total is a category error, and it is MY error, not the
            # document's.
            #
            # The first version of this file did exactly that and reported "12% of count claims
            # agree", which would have read as the documents being unreliable. Sampling the source
            # lines showed four of five disagreements were local statements: two guards inside one
            # structure against 36 on the level, two doors off one corridor against 46. The only
            # ones that matched were the level-wide ones.
            #
            # So a count is only checked when its own line marks it as total. Everything else is
            # LOCAL and unverifiable from a level census -- which is a fact about what the claim
            # says, not a mark against it.
            if not level_scoped(a.levels, lv, c["line"]):
                local.append(claim)
                continue
            claim["actual"] = have
            claim["as"] = sub or kind
            # Slack, because prose rounds: "about twenty guards" against 23 is agreement, and
            # demanding exactness would score honest description as error.
            if have == 0:
                ok = c["n"] == 0
            else:
                ok = abs(c["n"] - have) / float(have) <= a.tolerance
            (agree if ok else disagree).append(claim)

    checked = len(agree) + len(disagree)
    print("count claims: %d total" % sum(len(x) for x in (agree, disagree, unmapped, unanchored, nodata, local)))
    print("   checkable and CHECKED : %d" % checked)
    print("      agree              : %d" % len(agree))
    print("      disagree           : %d" % len(disagree))
    print("   not level-anchored    : %d  (_engine / _mp prose)" % len(unanchored))
    print("   noun has no counterpart: %d" % len(unmapped))
    print("   level lacks the data  : %d" % len(nodata))
    print("   LOCAL, not a census   : %d  (a claim about one spot, not the level)" % len(local))

    if checked:
        pct = 100.0 * len(agree) / checked
        # A PERCENTAGE OF ONE IS NOT A RATE. "100% agree" reads as vindication and would be
        # quoted as one; with a single sample it says only that the single sample matched. The
        # sample size travels with the figure so it cannot be separated from it.
        if checked < 5:
            print("\n   -> %d of %d checkable claims agree. TOO FEW TO BE A RATE -- this is a"
                  % (len(agree), checked))
            print("      handful of observations, not a verdict on the corpus.")
        else:
            print("\n   -> %.0f%% of %d checkable count claims agree with the setup data"
                  % (pct, checked))

    # The headline is not the agreement rate, it is how little of the corpus was ever a census.
    if local:
        print("\n %d claims describe ONE SPOT, not the level." % len(local))
        print("   The documents mostly say \"there are two guards inside\", which no level-wide")
        print("   total can confirm or contradict. Their counts therefore neither corroborate nor")
        print("   impugn them -- a different kind of answer from the distances, which were")
        print("   checkable and came out 1.46x wrong.")
    if disagree:
        print("\ndisagreements (claim vs actual):")
        for c in disagree[:12]:
            print("   %-10s claims %3d %-12s actual %3d %-12s (line %d)"
                  % (c["level"], c["n"], c["of"], c["actual"], c["as"], c["line"]))
    if agree:
        print("\nagreements:")
        for c in agree[:8]:
            print("   %-10s claims %3d %-12s actual %3d %-12s (line %d)"
                  % (c["level"], c["n"], c["of"], c["actual"], c["as"], c["line"]))
    if unmapped:
        seen = collections.Counter(c["of"] for c in unmapped)
        print("\nnouns with no counterpart in our data: %s"
              % ", ".join("%s x%d" % (k, v) for k, v in seen.most_common(8)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

