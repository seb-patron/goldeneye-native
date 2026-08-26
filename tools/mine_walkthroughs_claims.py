#!/usr/bin/env python3
"""Mine the ARGUMENT in the walkthroughs: theses, definitions, comparisons, rules and rationale.

mine_walkthroughs_deep.py takes the typographic structure -- headings, enumerations, drawings. It
reaches 36.5% of the corpus and stops there because the rest is prose, and the prose is where the
author does the actual reasoning. Measured over the 18,342 lines that pass through the structural
miner untouched:

    short labels             13,542   73.8%   diagram fragments and list lead-ins
    lines ending in a colon   3,518   19.2%   each one introduces the block beneath it
    full sentences            2,780   15.2%   the claims themselves
    conditional                 236    1.3%   rules: if, when, once, unless
    causal                      196    1.1%   rationale: because, therefore, which means
    comparison                  180    1.0%   cross-level: unlike, whereas, rather than

WHAT IS EXTRACTED AND WHY EACH EARNS ITS PLACE:

  thesis      a characterisation of a level. "Jungle is natural terrain." "The cave itself is the
              architecture." These are the author's structural model of a space, and they are the
              single most useful thing in the corpus for anything generating or reasoning about
              levels, because they say what a level IS rather than what is in it.
  definition  a colon lead-in bound to the block it introduces. On its own "So the fundamental idea
              is:" carries nothing; with its block it is a definition. 3,518 of these were being
              dropped as fragments precisely because the value is in the pairing.
  contrast    a definition by negation. The author repeatedly writes "Not:" followed by what a
              level is not, which bounds a claim in a way the positive statement alone does not.
  comparison  a statement relating one level to another. These are the only cross-level assertions
              in the corpus and they cannot be recovered from any single level's file.
  causal      a because/therefore claim. This is design rationale, and it explains WHY a space is
              shaped the way it is.
  rule        a conditional. If/when/once statements are gameplay mechanics stated as rules and are
              the closest thing here to something a bot could act on.

TWO RULES CARRIED OVER FROM THE STRUCTURAL MINER, both learned the hard way:

  * Prescriptive text IS marked, not mixed IN. These documents contain design guidance as well as
    description, and advice recorded as a claim about a level becomes a falsehood the moment
    anyone queries it -- invented by the tool, not asserted by the author.
  * Author caveats are carried. Where the author qualifies a passage, everything in that passage
    inherits the qualification. The Train carriage coordinates are the standing example: listed,
    then explicitly disclaimed as a reconstruction on the next line.

Attribution comes from the attribution audit, never from filenames. Output goes to build/ only.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from mine_walkthroughs_deep import (BOX, CAVEAT, CAVEAT_WINDOW, HEADING, IMPERATIVE,
                                    PRESCRIPTIVE, load_attribution)

# "X is/are Y" where X is a level or a structural noun. Anchored at the start so it catches the
# author's declarative openings and not every incidental "is" inside a longer sentence.
THESIS = re.compile(
    r"^(?:the\s+)?([A-Z][A-Za-z0-9 '\-]{2,38}?)\s+(?:is|are)\s+(?:essentially\s+|basically\s+|"
    r"fundamentally\s+|not\s+)?(.{6,180})$")
# "Think of X as Y" is the same move in a different sentence shape and is used constantly here.
THINK_OF = re.compile(r"^think of\s+(.{2,40}?)\s+as\s+(.{6,180})$", re.I)
LEADIN = re.compile(r"^(.{3,110}):$")
NEGATIVE_LEADIN = re.compile(r"^(?:and\s+)?not:?$", re.I)
COMPARE = re.compile(r"\b(?:unlike|whereas|compared (?:to|with)|rather than|versus|vs\.?|"
                     r"in contrast|the opposite of|more .{3,30} than|less .{3,30} than)\b", re.I)
CAUSAL = re.compile(r"\b(?:because|therefore|which means|the result is|this is why|so that|"
                    r"as a result|consequently|hence)\b", re.I)
RULE = re.compile(r"^(?:if|when|once|unless|as soon as|whenever)\b", re.I)

# Structural nouns worth a thesis even when the subject is not a level name.
STRUCTURAL = re.compile(r"\b(?:level|map|space|architecture|layout|geometry|terrain|corridor|"
                        r"room|structure|topology|traversal|environment|cave|building|axis)\b", re.I)


def near_caveat(caveats, n):
    return any(abs(c - n) <= CAVEAT_WINDOW for c in caveats)


def block_after(lines, i, limit=12):
    """The indented or listed block a colon lead-in introduces.

    Stops at a blank run, a heading, or another lead-in: a definition's block is what immediately
    follows it, and running past that would staple unrelated text onto the claim.
    """
    out = []
    j = i + 1
    blanks = 0
    while j < len(lines) and len(out) < limit:
        s = lines[j].strip()
        if not s:
            blanks += 1
            if blanks >= 2 or out:
                break
            j += 1
            continue
        if HEADING.match(s) or s.endswith(":"):
            break
        out.append(s[:140])
        j += 1
    return out


def mine(path, level, source):
    lines = open(path, "rb").read().decode("utf-8", errors="replace").splitlines()
    caveats = [n for n, l in enumerate(lines) if CAVEAT.search(l)]
    out, heading = [], None

    for i, raw in enumerate(lines):
        line = raw.strip()
        if not line or BOX.search(raw):
            continue
        if HEADING.match(line):
            heading = line
            continue

        base = {"level": level, "source": source, "line": i + 1, "heading": heading}
        pres = bool(PRESCRIPTIVE.search(line) or IMPERATIVE.match(line))
        derived = near_caveat(caveats, i)

        m = THINK_OF.match(line)
        if m:
            out.append(dict(base, kind="thesis", subject=m.group(1).strip(),
                            claim=m.group(2).strip().rstrip("."), form="think-of",
                            prescriptive=pres, derived=derived))
            continue

        m = THESIS.match(line)
        # A thesis has to be ABOUT something structural, or every "It is useful" in the file
        # becomes a claim about the level. The subject or the predicate must name a level, a space
        # or a piece of architecture.
        if m and (STRUCTURAL.search(m.group(1)) or STRUCTURAL.search(m.group(2))
                  or m.group(1).lower() == level):
            out.append(dict(base, kind="thesis", subject=m.group(1).strip(),
                            claim=m.group(2).strip().rstrip("."), form="is",
                            prescriptive=pres, derived=derived))
            continue

        if NEGATIVE_LEADIN.match(line):
            blk = block_after(lines, i)
            if blk:
                out.append(dict(base, kind="contrast", block=blk,
                                prescriptive=pres, derived=derived))
            continue

        m = LEADIN.match(line)
        if m:
            blk = block_after(lines, i)
            # A lead-in with nothing under it is just a fragment, which is what it was being
            # dropped as. The pairing is the whole point.
            if blk:
                out.append(dict(base, kind="definition", lead=m.group(1).strip(), block=blk,
                                prescriptive=pres, derived=derived))
            continue

        if COMPARE.search(line):
            out.append(dict(base, kind="comparison", text=line[:220],
                            prescriptive=pres, derived=derived))
            continue
        if CAUSAL.search(line):
            out.append(dict(base, kind="causal", text=line[:220],
                            prescriptive=pres, derived=derived))
            continue
        if RULE.match(line):
            out.append(dict(base, kind="rule", text=line[:220],
                            prescriptive=pres, derived=derived))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "walkthroughs"))
    ap.add_argument("--attribution", default=os.path.join(ROOT, "build", "levels",
                                                          "_walkthrough.attribution.json"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "levels",
                                                  "_walkthrough.claims.json"))
    a = ap.parse_args()

    usable, _ = load_attribution(a.attribution)
    seen, records = set(), []
    for f in sorted(os.listdir(a.dir)):
        if not f.lower().endswith(".txt"):
            continue
        p = os.path.join(a.dir, f)
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()
        if h in seen:
            continue                       # byte-identical copy, mined once
        seen.add(h)
        if f.lower().startswith("goldeneye64"):
            level = "_engine"
        elif f in usable:
            level = usable[f]
        else:
            continue                       # misfiled: text belongs to a level it never names
        records.extend(mine(p, level, f))

    by_kind = collections.Counter(x["kind"] for x in records)
    pres = sum(1 for x in records if x["prescriptive"])
    der = sum(1 for x in records if x["derived"])
    print("%d claim records" % len(records))
    print("\nby kind:")
    for k, n in by_kind.most_common():
        print("   %-12s %6d" % (k, n))
    print("\n%d marked prescriptive (design guidance, not description of a level)" % pres)
    print("%d marked derived (inside an author caveat)" % der)

    by_level = collections.Counter(x["level"] for x in records)
    print("\nby level:")
    for lv, n in sorted(by_level.items(), key=lambda x: -x[1])[:12]:
        print("   %-10s %6d" % (lv, n))

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump({"records": len(records), "by_kind": dict(by_kind),
                   "by_level": dict(by_level), "items": records}, fh, indent=1)
    print("\n-> %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
