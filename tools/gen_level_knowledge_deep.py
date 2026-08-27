#!/usr/bin/env python3
"""Mine the FULL structure of the per-level walkthrough documents, not just their JSON blocks.

WHAT WAS LEFT ON THE FLOOR. ingest_walkthroughs.py recovers embedded JSON blocks -- 199 of them --
and gen_level_facts.py consolidated those. But the blocks are a small part of the material: Train's
document is 43,831 characters of which the JSON is a fraction. The rest is STRUCTURED PROSE, and
the structure is machine-readable if you look at it:

    109  ALLCAPS labels          FORWARD, IN COVER, ENTER CAR, LOCATE BRAKE, DESTROY BRAKE
     59  numbered items          "4. CAR 1 - REAR CARGO CAR", "5. CARGO CRATES ARE NOT DECORATION"
     44  bullets
     13  embedded JSON blocks
     10  key: value attributes   "LENGTH: ~29 m"

🔑 THE LABEL SEQUENCE IS A PROCEDURE. Train's runs ENTER CAR -> CLEAR COMBAT SPACE -> LOCATE BRAKE
-> DESTROY BRAKE -> PROGRESS. That is a bot's objective loop for the level, written down, and
nothing was reading it. The numbered items are the level's own outline. Together they give a
per-level index of what the document knows and where to find it.

⚠️ THIS EMITS AN INDEX, NOT A COPY. Sections carry a title, a line range and a size; labels carry
their text and how often they occur; attributes carry a name and a value. The prose stays in the
source document -- the point is to make it addressable, so a later tool or a person can go to
train.walkthrough.json line 412 rather than re-read 43,831 characters.

⚠️ AND NOTHING HERE IS GROUND TRUTH. Same rule as the rest of this pipeline: these are a
document's assertions. gen_level_facts.py already established that its metres are 1.46x the
engine's, so its topology and vocabulary are usable and its distances are not.

⚠️ OUTPUT STAYS OUT OF GIT -- build/ is ignored and .gitignore names the intent.
"""
import argparse
import collections
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RE_NUMBERED = re.compile(r"^\s*(\d+)[.)]\s+(\S.*)$")
RE_CAPS = re.compile(r"^[A-Z][A-Z0-9 ,'&/()\-]{5,}$")
RE_ATTR = re.compile(r"^\s*([A-Za-z][A-Za-z0-9 _/-]{2,40}):\s+(\S.*)$")
RE_BULLET = re.compile(r"^\s*[-*•]\s+(\S.*)$")


def mine(text):
    lines = text.splitlines()
    out = {"sections": [], "labels": [], "attributes": [], "bullets": 0, "lines": len(lines)}
    caps = collections.Counter()
    caps_first = {}

    cur = None
    for i, raw in enumerate(lines, 1):
        line = raw.rstrip()
        m = RE_NUMBERED.match(line)
        if m and len(m.group(2)) > 3:
            # A numbered item that is ALSO all-caps is an outline heading; one that is not is a
            # step inside a procedure. Both are kept, distinguished, because "4. CAR 1 - REAR
            # CARGO CAR" and "1. Occlusion" are different kinds of thing.
            title = m.group(2).strip()
            kind = "heading" if RE_CAPS.match(title) else "step"
            cur = {"n": int(m.group(1)), "title": title[:120], "kind": kind,
                   "line": i, "chars": 0}
            out["sections"].append(cur)
            continue
        if cur is not None:
            cur["chars"] += len(line)

        if RE_CAPS.match(line):
            t = line.strip()
            # ASCII diagram rows ("CAR A       CAR B") collapse runs of spaces so the label reads
            # as one token rather than as accidental whitespace structure.
            t = re.sub(r"\s{2,}", " | ", t)
            caps[t] += 1
            caps_first.setdefault(t, i)
            continue

        m = RE_ATTR.match(line)
        if m:
            out["attributes"].append({"name": m.group(1).strip(), "value": m.group(2).strip()[:80],
                                      "line": i})
            continue
        if RE_BULLET.match(line):
            out["bullets"] += 1

    out["labels"] = [{"text": t, "count": c, "line": caps_first[t]}
                     for t, c in caps.most_common()]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    a = ap.parse_args()

    total = collections.Counter()
    index = {}
    for f in sorted(glob.glob(os.path.join(a.levels, "*.walkthrough.json"))):
        lv = os.path.basename(f)[: -len(".walkthrough.json")]
        d = json.load(open(f, encoding="utf-8"))
        per_doc = []
        for doc in d.get("documents", []):
            m = mine(doc.get("text", ""))
            m["source"] = os.path.basename(doc.get("source", "?"))
            m["blocks"] = len(doc.get("structured", []))
            per_doc.append(m)
            total["sections"] += len(m["sections"])
            total["labels"] += len(m["labels"])
            total["attributes"] += len(m["attributes"])
            total["bullets"] += m["bullets"]
            total["blocks"] += m["blocks"]
            total["lines"] += m["lines"]
        if per_doc:
            index[lv] = per_doc

    dest = os.path.join(a.levels, "_walkthrough.index.json")
    with open(dest, "w", encoding="utf-8") as fh:
        json.dump({
            "note": ("An INDEX of what each document contains and where -- not a copy. Assertions, "
                     "not ground truth; its distances are known wrong (see _level.facts.json)."),
            "levels": len(index),
            "totals": dict(total),
            "by_level": index,
        }, fh, indent=1, ensure_ascii=False)

    print("%d level bucket(s) indexed -> %s" % (len(index), dest))
    for k in ("lines", "sections", "labels", "attributes", "bullets", "blocks"):
        print("   %-11s %d" % (k, total[k]))

    # The vocabulary across every level: the recurring labels are the documents' own ontology of
    # states and actions, and a bot's behaviour model could be built from it.
    vocab = collections.Counter()
    for lv, docs in index.items():
        for m in docs:
            for l in m["labels"]:
                vocab[l["text"]] += l["count"]
    print("\nmost common labels across all levels (the documents' own vocabulary):")
    for t, c in vocab.most_common(18):
        print("   %-42s %d" % (t[:42], c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
