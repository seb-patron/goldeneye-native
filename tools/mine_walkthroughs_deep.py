#!/usr/bin/env python3
"""Mine the STRUCTURES in the walkthroughs, not just the sentences.

The earlier extractors read one line at a time and matched prose patterns. Measured against the
corpus that reaches about 3.9% of it -- 1,646 items from 34,857 distinct lines -- and the reason is
not that the patterns are weak. It is that most of what these documents know is not in sentences:

    ascii diagrams        6,550 lines   18.8%    topology, drawn
    ALL-CAPS headings     3,189 lines    9.1%    document structure
    numbered list items   2,610 lines    7.5%    ordered enumerations
    prose sentences       3,820 lines   11.0%    what was already being read

A diagram of six carriages joined front to back states the level's connectivity more precisely than
any sentence in the file, and a line-at-a-time reader sees only box-drawing characters. An
enumeration states an ORDER. A heading states what the section below it is about, which is the
context every item under it needs. All three span multiple lines, so all three were invisible.

WHAT IS EXTRACTED, and why each one is worth having:

  section     heading plus its span. Gives every other record a subject, so a claim can be quoted
              with the context it was made in rather than as a floating sentence.
  enum        an ordered list with its lead-in. The order is the content: "1 rear cargo car,
              2 forward cargo car ..." is a traversal sequence, not a set.
  diagram     a run of box-drawing lines, reduced to the labels inside it and the direction arrows
              between them. This is the level's own topology, drawn by someone who walked it.
  kv          KEY: value lines, which is how the dimensional data is written.
  dimension   a number with a unit. Already known to be checkable and already known to be wrong by
              a constant: the documents' metre measures 1.46x the engine's.
  constraint  negative claims -- no branches, no lateral traversal, no alternate route. These are
              the most checkable statements in the corpus because the tile graph can confirm or
              refute them directly.

ATTRIBUTION COMES FROM audit_walkthrough_attribution.py, NOT FROM FILENAMES. Eight of the 36 files
are misfiled copies naming a level their text never mentions, and eight levels have no document at
all. Mining those under their filenames would file Bunker 2's text under runway and silo, which is
what the previous pass did.

Third-party and author material stays out of git. This writes to build/ only.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Every character the corpus draws with. The first version covered Box Drawing (U+2500-U+257F)
# and arrows only, which left 678 lines of diagram invisible: the documents draw floor plans with
# BLOCK ELEMENTS (U+2580-U+259F, the solid blocks marking walls and filled space) and mark
# positions with GEOMETRIC SHAPES (U+25A0-U+25FF). Temple, Depot, Statue and Streets are drawn
# almost entirely this way, so their maps were being read as prose fragments and dropped.
BOX = re.compile("[─-╿▀-▟■-◿←-⇿]")
HEADING = re.compile(r"^[A-Z][A-Z0-9 '/&,\.\-\(\)]{4,}$")
ENUM = re.compile(r"^\s*(?:\t)?(\d+)\s*(?:\t|[\.\)]\s)\s*(\S.*)$")
KV = re.compile(r"^\s*([A-Z][A-Z /_-]{2,20}):\s*(\S.*)$")
DIM = re.compile(r"\b(\d+(?:\.\d+)?)\s*(m|meters?|metres?|ft|feet|units?)\b", re.I)
NEG = re.compile(r"\b(?:there (?:is|are) (?:essentially )?no|no (?:meaningful|alternate|exterior|"
                 r"lateral|real)|cannot|never|impossible to)\b", re.I)
# A label inside a diagram: letters/digits with no box characters, short enough to be a label.
LABEL = re.compile(r"[A-Za-z][A-Za-z0-9 '\-]{1,28}")

# THESE DOCUMENTS CONTAIN TWO KINDS OF STATEMENT AND CONFLATING THEM WOULD BE THE WORST ERROR
# AVAILABLE HERE. Most of the text describes a level that exists, and "there is no lateral
# traversal" is checkable against the tile graph. But stretches of it are DESIGN GUIDANCE about
# how such a space should be built -- attaching rooms to corridors, breaking up large chambers.
# Both are worth keeping and they are not the same kind of thing. Advice recorded as description
# becomes a false claim about the level the moment anyone queries it, and it would be this tool
# inventing the claim rather than the author making it.
#
# So constraints carry a mode instead of being filtered out.
# Imperatives are the other half: advice frequently arrives as a bare instruction with no modal
# verb at all, and a should/could test files it as a description of a level that exists.
IMPERATIVE = re.compile(r"^(?:attach|break|place|add|keep|make|give|use|avoid|ensure|"
                        r"treat|split|widen|narrow|position|design)\b", re.I)
PRESCRIPTIVE = re.compile(r"\b(?:should|could|consider|try to|aim to|ought to|you want|"
                          r"the goal is to|when building|if you are building)\b", re.I)

# COORDINATES, WRITTEN INSIDE THE DIAGRAMS. The Train document records a per-carriage X for each
# car ("CAR 5: X = 116"). That is the most directly checkable thing in the whole corpus: it can be
# compared against the tile map without any unit conversion, unlike the metre figures which are
# already known to be 1.46x the engine's.
COORD = re.compile(r"\b([XYZ])\s*=\s*(-?\d+(?:\.\d+)?)")

# THE DOCUMENTS QUALIFY THEIR OWN NUMBERS, AND A MINER THAT DROPS THE QUALIFICATION IS WORSE THAN
# ONE THAT MISSES THE NUMBER. The Train file lists a per-carriage X for each car and then says, in
# the next line: do not interpret those derived intervals as extracted game coordinates, they are
# a normalized reconstruction from the documented ~29 m car length and ~239 m total.
#
# Read without that line, those six values look like the most directly checkable data in the whole
# corpus and would have been compared against the tile map as though the author had measured them.
# The author says plainly that they did not. So a caveat within a short window marks the records
# around it as derived, and a derived value is kept but never treated as evidence.
CAVEAT = re.compile(r"\b(?:do not interpret|not (?:be )?(?:actual|extracted|real)|"
                    r"reconstruction|reconstructed|normali[sz]ed|derived interval|approximation|"
                    r"illustrative|not to scale|rough(?:ly)? estimate)\b", re.I)
CAVEAT_WINDOW = 6      # lines either side; the caveat sits just below the block it qualifies


def load_attribution(path):
    if not os.path.isfile(path):
        sys.exit("no %s -- run tools/audit_walkthrough_attribution.py first" % path)
    d = json.load(open(path, encoding="utf-8"))
    return d.get("usable", {}), d


def diagram_labels(block):
    """Text labels inside a drawn block, and whether the block shows a direction."""
    labels, arrows = [], set()
    for line in block:
        for ch, name in (("→", "right"), ("←", "left"), ("↓", "down"),
                         ("↑", "up"), ("▼", "down"), ("▲", "up")):
            if ch in line:
                arrows.add(name)
        # Strip the drawing characters, then whatever readable text is left is a label.
        # Split on runs of two or more spaces. That is what separates cells in a drawn table, and
        # it is why the first version produced "CAR 2    CA" and "R 3    CAR 4": a length cap cuts
        # wherever the limit lands, which is mid-word whenever a row holds several labels.
        txt = BOX.sub("  ", line)
        for cell in re.split(r"\s{2,}", txt):
            s = cell.strip()
            if len(s) > 2 and not s.isdigit() and re.search(r"[A-Za-z]", s):
                labels.append(s)
    return labels, sorted(arrows)


def near_caveat(caveats, n):
    """Is line n inside the window of an author caveat?"""
    return any(abs(c - n) <= CAVEAT_WINDOW for c in caveats)


def mine(path, level, source):
    text = open(path, "rb").read().decode("utf-8", errors="replace")
    lines = text.splitlines()
    # Scanned up front rather than as the reader passes, because a caveat almost always sits
    # BELOW the block it qualifies -- by the time a streaming reader met it, the numbers it
    # applies to would already have been emitted unqualified.
    caveats = [n for n, l in enumerate(lines) if CAVEAT.search(l)]
    out = []
    heading = None
    in_diagram = False       # set while the reader is within a few lines of a drawn block
    last_box = -99
    i = 0
    while i < len(lines):
        raw = lines[i]
        line = raw.strip()
        if not line:
            i += 1
            continue
        # Captions and stray cells sit within a line or two of the drawing they belong to.
        in_diagram = (i - last_box) <= 2

        # A run of drawn lines is one diagram, not many lines. Consecutive box-drawing lines are
        # gathered together, because a single row of a box says nothing on its own.
        if BOX.search(raw):
            start = i
            block = []
            while i < len(lines) and (BOX.search(lines[i]) or
                                      (lines[i].strip() and not HEADING.match(lines[i].strip())
                                       and block and len(lines[i]) < 120)):
                block.append(lines[i])
                i += 1
                if len(block) > 60:
                    break
            last_box = i
            # Coordinates and dimensions written inside a drawing are still coordinates and
            # dimensions. Extracted from the block's own rows before it is reduced to labels,
            # because the reduction throws the numbers away.
            # A CAVEAT QUALIFIES THE WHOLE BLOCK, NOT A RADIUS OF LINES. The author wrote "those
            # derived intervals" about a list of six carriages; a fixed line window reached four
            # of them and left CAR 1 and CAR 2 marked as though they were measured. Splitting one
            # list like that is worse than not marking it at all, because the split looks
            # deliberate -- it invites quoting the first two as data and the rest as reconstruction.
            block_derived = any(near_caveat(caveats, start + off) for off in range(len(block)))
            for off, brow in enumerate(block):
                for c in COORD.finditer(brow):
                    out.append({"level": level, "source": source, "kind": "coord",
                                "line": start + off + 1, "heading": heading,
                                "axis": c.group(1), "value": float(c.group(2)),
                                "text": brow.strip()[:120],
                                "in_diagram": True,
                                "derived": block_derived})
            labels, arrows = diagram_labels(block)
            if labels:
                out.append({"level": level, "source": source, "kind": "diagram",
                            "line": start + 1, "heading": heading,
                            "rows": len(block), "arrows": arrows,
                            # Deduped but order kept: the order labels appear in a drawing is
                            # usually the order they occur in the level.
                            "labels": list(dict.fromkeys(labels))[:24]})
            continue

        if HEADING.match(line) and not in_diagram:
            heading = line
            out.append({"level": level, "source": source, "kind": "section",
                        "line": i + 1, "heading": heading})
            i += 1
            continue

        m = ENUM.match(raw)
        if m:
            out.append({"level": level, "source": source, "kind": "enum", "line": i + 1,
                        "heading": heading, "n": int(m.group(1)), "text": m.group(2).strip()})
            i += 1
            continue

        m = KV.match(raw)
        if m:
            out.append({"level": level, "source": source, "kind": "kv", "line": i + 1,
                        "heading": heading, "key": m.group(1).strip(), "value": m.group(2).strip()})
            i += 1
            continue

        for c in COORD.finditer(line):
            out.append({"level": level, "source": source, "kind": "coord", "line": i + 1,
                        "heading": heading, "axis": c.group(1), "value": float(c.group(2)),
                        "text": line.strip()[:120],
                        "derived": near_caveat(caveats, i)})

        for d in DIM.finditer(line):
            out.append({"level": level, "source": source, "kind": "dimension", "line": i + 1,
                        "heading": heading, "value": float(d.group(1)), "unit": d.group(2).lower(),
                        "text": line[:160],
                        "derived": near_caveat(caveats, i)})

        if NEG.search(line):
            out.append({"level": level, "source": source, "kind": "constraint", "line": i + 1,
                        "heading": heading, "text": line[:200],
                        "mode": "prescriptive" if (PRESCRIPTIVE.search(line) or
                                                  IMPERATIVE.match(line))
                                else "descriptive"})
        i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "walkthroughs"))
    ap.add_argument("--attribution", default=os.path.join(ROOT, "build", "levels",
                                                          "_walkthrough.attribution.json"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "levels", "_walkthrough.deep.json"))
    a = ap.parse_args()

    usable, attrib = load_attribution(a.attribution)
    engine_files = [f for f in sorted(os.listdir(a.dir))
                    if f.lower().endswith(".txt") and f.lower().startswith("goldeneye64")]

    seen_hash, records, per_file = set(), [], {}
    for f in sorted(os.listdir(a.dir)):
        if not f.lower().endswith(".txt"):
            continue
        p = os.path.join(a.dir, f)
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()
        # Byte-identical copies are mined once. Counting the same text under three names is how
        # runway, silo and bunker2 came out with identical figures.
        if h in seen_hash:
            continue
        seen_hash.add(h)

        if f in engine_files:
            level = "_engine"
        elif f in usable:
            level = usable[f]
        else:
            continue                # misfiled: its text belongs to a level it does not name
        r = mine(p, level, f)
        records.extend(r)
        per_file[f] = (level, len(r))

    derived = sum(1 for x in records if x.get("derived"))
    modes = collections.Counter(x.get("mode") for x in records if x["kind"] == "constraint")
    by_kind = collections.Counter(x["kind"] for x in records)
    by_level = collections.Counter(x["level"] for x in records)

    print("mined %d files, %d records" % (len(per_file), len(records)))
    print("\nby kind:")
    for k, n in by_kind.most_common():
        print("   %-12s %6d" % (k, n))
    print("\nconstraints by mode: %s" % dict(modes))
    print("   descriptive claims are checkable against the level; prescriptive ones are the")
    print("   author reasoning about design and must not be quoted as fact about it.")
    print("\n%d numeric record(s) sit under an author caveat and are marked derived." % derived)
    print("   The Train per-carriage X values are the clearest case: the document lists them and")
    print("   then says not to read them as extracted game coordinates.")
    print("\nby level:")
    for lv, n in sorted(by_level.items(), key=lambda x: -x[1]):
        print("   %-10s %6d" % (lv, n))

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump({"records": len(records), "by_kind": dict(by_kind),
                   "by_level": dict(by_level), "items": records}, fh, indent=1)
    print("\n-> %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
