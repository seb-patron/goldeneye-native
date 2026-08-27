#!/usr/bin/env python3
"""Extract the vertical flow chains: pipelines, state machines and ordered sequences.

The corpus draws its processes as vertical chains -- a label, an arrow, a label, an arrow -- and
they are the clearest statements of ORDER anywhere in it. The engine documents draw the AI
perception chain this way, from an event through a sensory filter to perception, memory and
interpretation. Level documents draw traversal order the same way.

NEITHER EARLIER MINER CAPTURES THEM, and the reason is worth stating because it is a general trap.
The structural miner does treat an arrow as a drawing character, so it starts a block ON the first
arrow -- which throws away the label ABOVE it, the head of the chain, and then reduces the rest to
an unordered label set. A pipeline with its first stage missing and its order discarded is not a
pipeline. The claims miner sees single words on their own lines and correctly ignores them as
fragments, because in isolation that is exactly what they are.

The value here is entirely in the sequence. EVENT, SENSORY FILTER, PERCEPTION, MEMORY,
INTERPRETATION as a set says almost nothing; in that order it is a design.

HOW A CHAIN IS RECOGNISED. A run of lines where connector-only lines alternate with short label
lines. The connector may be an arrow glyph or an ascii equivalent. The scan starts from the FIRST
CONNECTOR and walks BACKWARDS to pick up the head label, which is the part the structural miner
loses, then forwards to the tail.

⚠️ A CHAIN OF ONE IS NOT A CHAIN. Two labels minimum, and the labels must be short enough to be
labels rather than sentences that happen to sit near an arrow -- otherwise a paragraph containing a
stray arrow is emitted as a five-stage pipeline, which reads as a finding and is noise.

Attribution from the attribution audit, never filenames. Byte-identical copies read once.
Author material stays out of git; output goes to build/ only.
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

from mine_walkthroughs_deep import CAVEAT, CAVEAT_WINDOW, HEADING, load_attribution

# A line that is nothing but a connector. Vertical chains use these between their stages.
# A line that is nothing but a connector.
#
# THE VERTICAL BAR HERE IS U+2502, NOT THE ASCII PIPE. The first version matched only "|", which
# the corpus never uses -- it draws with box-drawing verticals. Every branching diagram therefore
# failed at its first connector and was left entirely uncovered, which is why the fan-out graphs
# were still sitting in the remainder after four passes.
CONNECTOR = re.compile(r"^[\s|│┃║↓↑▼▲v^]+$"
                       r"|^[\s|│]*[-=]{0,4}>?[\s|│]*$")
# A fan-out or fan-in rule: a horizontal run carrying a junction. These bracket a parallel group.
JUNCTION = re.compile("[┌┐└┘┬┴┼├┤]")
DOWN = re.compile(r"^[\s|│]*[↓▼v][\s|│]*$", re.I)
# A stage label: short, has letters, is not a sentence and is not a heading rule.
LABEL_OK = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 '/_\-\(\)&\.]{1,44}$")
# Widened alongside the structural miner: a line carrying a solid block or a geometric marker is
# part of a floor plan, not a stage label, and would otherwise be read as one.
BOXCHAR = re.compile("[─-╿▀-▟■-◿]")

MAX_LABEL_WORDS = 7


def is_connector(s):
    t = s.strip()
    return bool(t) and bool(CONNECTOR.match(t)) and not LABEL_OK.match(t)


def is_label(s):
    t = s.strip()
    if not t or BOXCHAR.search(t) or is_connector(t):
        return False
    if not LABEL_OK.match(t):
        return False
    # A sentence is not a stage name. Trailing punctuation and length both give it away.
    if t.endswith((".", ",", ";", ":")) and len(t.split()) > 3:
        return False
    return len(t.split()) <= MAX_LABEL_WORDS


def parallel_labels(line):
    """Several labels on one line, separated by column gaps: a parallel group, or None."""
    if BOXCHAR.search(line):
        return None
    parts = [p.strip() for p in re.split(r"\s{2,}", line.strip()) if p.strip()]
    if len(parts) < 2:
        return None
    return parts if all(is_label(p) for p in parts) else None


def chain_at(lines, i):
    """Walk a chain whose first connector is at line i. Returns (start, end, stages)."""
    # BACKWARDS FIRST. The head label sits above the first connector, and taking it is the whole
    # reason this exists: starting at the arrow silently drops the first stage of every pipeline.
    head = i - 1
    while head >= 0 and not lines[head].strip():
        head -= 1
    if head < 0:
        return None
    # The head gets the SAME parallel split as every other stage. Without it a fan-out whose top
    # row holds three labels is recorded as one stage literally named
    # "LEVEL          ACTORS        SIMULATION", which is not a stage name and not anything else
    # either. The forward walk already split these; the head walk did not, so the two ends of one
    # chain disagreed about what a row means.
    head_par = parallel_labels(lines[head])
    if not head_par and not is_label(lines[head]):
        return None
    stages = [head_par if head_par else lines[head].strip()]
    start = head

    j = i
    end = i
    while j < len(lines):
        if not lines[j].strip():
            j += 1
            continue
        if is_connector(lines[j]):
            j += 1
            continue
        # DO NOT EXCLUDE HEADING-SHAPED LINES HERE. HEADING matches any ALL-CAPS line of five or
        # more characters, and these chains label their stages in caps: CONTACT, RETREAT, CORNER,
        # REPOSITION. Excluding them rejected every stage after the head, dropped each chain below
        # the two-stage minimum, and discarded it whole -- so entire combat and traversal loops
        # were invisible while the arrows between them sat in the uncovered remainder.
        #
        # A stage is only accepted when the preceding non-blank line was a connector, and that is
        # what distinguishes it from a section heading. The shape of the text never could.
        if is_label(lines[j]) or parallel_labels(lines[j]):
            # A FAN-OUT PUTS SEVERAL STAGES ON ONE LINE, and they run in parallel rather than in
            # sequence. CAMERA, WEAPON and ANIMATION under one junction are three branches of the
            # same step, and flattening them into three sequential stages would assert an order the
            # drawing explicitly does not show.
            par = parallel_labels(lines[j])
            stages.append(par if par else lines[j].strip())
            end = j
            j += 1
            # A chain ends when the next non-blank line is not a connector.
            k = j
            while k < len(lines) and not lines[k].strip():
                k += 1
            if k >= len(lines) or not is_connector(lines[k]):
                break
            j = k
            continue
        break
    return (start, end, stages) if len(stages) >= 2 else None


def mine(path, level, source):
    lines = open(path, "rb").read().decode("utf-8", errors="replace").splitlines()
    caveats = [n for n, l in enumerate(lines) if CAVEAT.search(l)]
    out, heading, i = [], None, 0
    while i < len(lines):
        s = lines[i].strip()
        if s and HEADING.match(s):
            heading = s
        if s and is_connector(s):
            got = chain_at(lines, i)
            if got:
                start, end, stages = got
                # Dedupe consecutive repeats: a drawing sometimes repeats a label on the way down.
                seq = [x for n, x in enumerate(stages) if n == 0 or x != stages[n - 1]]
                # A parallel group counts as one stage, which is what it is.
                if len(seq) >= 2:
                    out.append({"level": level, "source": source, "line": start + 1,
                                "heading": heading, "kind": "flow", "stages": seq,
                                "length": len(seq),
                                "direction": "down" if DOWN.match(s) else "linear",
                                "derived": any(abs(c - start) <= CAVEAT_WINDOW for c in caveats)})
                i = max(end + 1, i + 1)
                continue
        i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "walkthroughs"))
    ap.add_argument("--attribution", default=os.path.join(ROOT, "build", "levels",
                                                          "_walkthrough.attribution.json"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "levels",
                                                  "_walkthrough.flows.json"))
    a = ap.parse_args()

    usable, _ = load_attribution(a.attribution)
    seen, records = set(), []
    for f in sorted(os.listdir(a.dir)):
        if not f.lower().endswith(".txt"):
            continue
        p = os.path.join(a.dir, f)
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()
        if h in seen:
            continue
        seen.add(h)
        if f.lower().startswith("goldeneye64"):
            level = "_engine"
        elif f in usable:
            level = usable[f]
        else:
            continue
        records.extend(mine(p, level, f))

    print("%d flow chain(s)" % len(records))
    if records:
        lens = collections.Counter(x["length"] for x in records)
        print("   stages per chain: %s" % dict(sorted(lens.items())))
        print("   longest: %d stages" % max(x["length"] for x in records))
    by_level = collections.Counter(x["level"] for x in records)
    print("\nby level:")
    for lv, n in sorted(by_level.items(), key=lambda x: -x[1])[:12]:
        print("   %-10s %4d" % (lv, n))

    print("\nlongest chains:")
    for x in sorted(records, key=lambda y: -y["length"])[:6]:
        print("   %-9s line %-5d %s" % (x["level"], x["line"], " -> ".join(x["stages"])[:96]))

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump({"chains": len(records), "by_level": dict(by_level), "items": records},
                  fh, indent=1)
    print("\n-> %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
