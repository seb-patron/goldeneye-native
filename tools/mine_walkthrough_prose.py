#!/usr/bin/env python3
"""Reflow the prose into paragraphs, and take the API vocabulary out of it.

After the structural, claims, specs and flow passes, 6,945 lines still yield nothing, and the
sample says why: they are not a new kind of content, they are the SAME content broken across lines.
A statement wrapped over two lines is two fragments, and each fragment matches nothing on its own:

    CanCompleteMission()
    should evaluate the mission state.

The claims miner reads a line at a time, so it sees an identifier and a sentence beginning with
"should" and correctly rejects both. Joined, it is one clear statement about an API function.

So this pass does two things that only make sense over joined text:

  paragraph   consecutive prose lines joined into one unit, with the heading they sit under. This
              is the honest shape of the source: the author wrote paragraphs, and the line breaks
              are typography, not structure.
  identifier  the API vocabulary. Function names, constants and typed symbols -- CanCompleteMission,
              ON_SCRIPT -- are the corpus naming the interface it is describing, and they are worth
              having as a list because they say what the author expected the engine to expose.

WHY THIS DOES NOT DUPLICATE THE CLAIMS PASS. That pass matched claim SHAPES against single lines.
This one produces the units those shapes should have been matched against, and records the
paragraph itself rather than re-running the same patterns: a paragraph is evidence in its own right
and can be quoted with its context, which a matched fragment cannot.

A PARAGRAPH ENDS AT A BLANK LINE, A HEADING, A DRAWING OR A LIST, and not merely at a full stop.
Running paragraphs together across those boundaries would staple unrelated statements into one
block and give a claim a context it never had, which is worse than leaving the lines uncovered.

AND SINGLE-LINE PARAGRAPHS ARE STILL PARAGRAPHS. A one-line statement between two blanks is a
complete thought and the author wrote plenty of them. Requiring two or more lines would drop
exactly the terse declarative statements that are the most quotable thing in the corpus.

Prescriptive text is marked and author caveats are inherited, as in every other pass. Attribution
from the attribution audit. Byte-identical copies read once. Output to build/ only.
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

from mine_walkthroughs_deep import (BOX, CAVEAT, CAVEAT_WINDOW, ENUM, HEADING, IMPERATIVE,
                                    KV, PRESCRIPTIVE, load_attribution)

# Symbols the corpus uses to name an interface: CamelCase calls, CONSTANT_NAMES, snake_case fields.
IDENTIFIER = re.compile(r"\b(?:[A-Z][a-z0-9]+(?:[A-Z][a-z0-9]*)+\s*\(\s*\)|"
                        r"[A-Z][A-Z0-9]{2,}(?:_[A-Z0-9]+)+|"
                        r"[a-z]+(?:_[a-z0-9]+){1,4}\s*\(\s*\))")
# Words that are ordinary prose in caps, not constants. Without this every shouted heading word
# becomes an API symbol.
NOT_IDENT = re.compile(r"^(?:THE_|AND_|BUT_|NOT_)")


def is_break(line):
    """Does this line end a paragraph?"""
    s = line.strip()
    if not s:
        return True
    if BOX.search(line) or HEADING.match(s) or ENUM.match(line) or KV.match(line):
        return True
    if s.endswith(":"):
        return True                 # a lead-in belongs to the block miner, not to a paragraph
    return False


def mine(path, level, source):
    lines = open(path, "rb").read().decode("utf-8", errors="replace").splitlines()
    caveats = [n for n, l in enumerate(lines) if CAVEAT.search(l)]
    out, heading = [], None

    buf, start = [], None
    def flush():
        if not buf:
            return
        text = " ".join(x.strip() for x in buf)
        # A paragraph of pure punctuation or a stray fragment is not worth a record.
        if len(text) >= 12 and re.search(r"[A-Za-z]{3}", text):
            out.append({"level": level, "source": source, "kind": "paragraph",
                        "line": start + 1, "lines": len(buf), "heading": heading,
                        "text": text[:1200],
                        "prescriptive": bool(PRESCRIPTIVE.search(text)
                                             or IMPERATIVE.match(text)),
                        "derived": any(abs(c - start) <= CAVEAT_WINDOW for c in caveats)})

    for i, raw in enumerate(lines):
        s = raw.strip()
        if s and HEADING.match(s):
            flush(); buf, start = [], None
            heading = s
            continue
        if is_break(raw):
            flush(); buf, start = [], None
            continue
        if not buf:
            start = i
        buf.append(raw)
    flush()

    # Identifiers are taken from the whole file, not only from paragraphs: the corpus names symbols
    # inside lists and drawings too, and a symbol is a symbol wherever it is written.
    seen_id = {}
    for i, raw in enumerate(lines):
        for m in IDENTIFIER.finditer(raw):
            name = re.sub(r"\s+", "", m.group(0))
            if NOT_IDENT.match(name):
                continue
            if name not in seen_id:
                seen_id[name] = i + 1
    for name, ln in seen_id.items():
        out.append({"level": level, "source": source, "kind": "identifier",
                    "line": ln, "name": name})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "walkthroughs"))
    ap.add_argument("--attribution", default=os.path.join(ROOT, "build", "levels",
                                                          "_walkthrough.attribution.json"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "levels",
                                                  "_walkthrough.prose.json"))
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

    paras = [x for x in records if x["kind"] == "paragraph"]
    ids = [x for x in records if x["kind"] == "identifier"]
    print("%d paragraphs, %d identifiers" % (len(paras), len(ids)))
    if paras:
        w = collections.Counter(x["lines"] for x in paras)
        print("   lines per paragraph: %s%s"
              % (dict(sorted(w.items())[:8]), " ..." if len(w) > 8 else ""))
        print("   %d prescriptive, %d under an author caveat"
              % (sum(1 for x in paras if x["prescriptive"]),
                 sum(1 for x in paras if x["derived"])))

    names = collections.Counter(x["name"] for x in ids)
    print("\nmost-named symbols:")
    for n, c in names.most_common(14):
        print("   %-34s %d file(s)" % (n, c))

    by_level = collections.Counter(x["level"] for x in paras)
    print("\nparagraphs by level:")
    for lv, n in sorted(by_level.items(), key=lambda x: -x[1])[:10]:
        print("   %-10s %5d" % (lv, n))

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump({"paragraphs": len(paras), "identifiers": len(ids),
                   "symbol_frequency": dict(names.most_common()),
                   "items": records}, fh, indent=1)
    print("\n-> %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
