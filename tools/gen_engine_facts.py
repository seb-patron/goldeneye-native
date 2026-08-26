#!/usr/bin/env python3
"""S8: pull the checkable NUMBERS out of the engine reference prose.

Prose is for people; facts are for us. This reads build/levels/_engine.walkthrough.json and emits
build/levels/_engine.facts.json as name / value / unit / source, so an engine can compare a claim
against what the decompilation actually does.

EVERY FACT HERE IS A CLAIM, NOT A MEASUREMENT, AND THE FILE SAYS SO IN EVERY RECORD.
The source is community-written documentation. The ingester's own note is "third-party prose, for
analysis only; not ground truth", and that property has to survive extraction: a tidy JSON table
of numbers looks authoritative in a way the paragraph it came from did not, and the danger is
somebody trusting a fan-written figure over `chr.c`. So each record carries status="unverified"
and the source line it came from, and the file is called facts ABOUT THE DOCUMENT rather than
facts about the game.

The useful ones are those a decomp lookup can settle. `chrwidth 20.0f` is in chr.c:1936; if the
prose says a different number, one of them is wrong and it is worth knowing which.

OUTPUT STAYS OUT OF GIT. It is derived from material we may not redistribute -- build/ is
ignored, and .gitignore now names the intent explicitly rather than relying on that.

THE LABEL IS A LABEL, NOT AN EXCERPT. `name` is a short slug built from the few words adjacent
to the number, enough to say what the value refers to. This deliberately does not copy sentences:
the point is the number and where to look it up, and a facts file that quietly became a copy of
the document would defeat the reason it is kept out of git.
"""
import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Units worth extracting, and their canonical spelling. Chosen because each is something the
# decompilation can be asked about: a distance, a duration, a rate, an angle, a count of damage.
# Percentages and bare numbers are not harvested -- a document of this size yields
# thousands of them, almost all page references and list indices, and the noise would bury the
# handful that mean anything.
UNITS = [
    (r"units?", "units"),
    (r"seconds?|secs?\b", "s"),
    (r"minutes?|mins?\b", "min"),
    (r"milliseconds?|ms\b", "ms"),
    (r"frames?", "frames"),
    (r"degrees?|deg\b", "deg"),
    (r"rounds?|bullets?|shots?", "rounds"),
    (r"damage|hp\b|health", "damage"),
    (r"metres?|meters?|m\b", "m"),
    (r"feet|ft\b", "ft"),
    (r"mph\b", "mph"),
]
UNIT_RE = "|".join("(?:%s)" % u for u, _ in UNITS)
NUM = r"(-?\d+(?:\.\d+)?)"
FACT_RE = re.compile(r"\b%s\s*(%s)\b" % (NUM, UNIT_RE), re.I)

# Words that make a label useless. Stripped from the context window so the slug carries the
# meaningful term rather than filler.
STOP = set("""a an the of to in on at for with and or is are was were be been it its this that
these those you your he she they them from by as if then than so but not no do does did can will
would should could may might must have has had there here when where which who whom whose what
about into over under up down out off then also just only very more most some any each""".split())


def canon_unit(raw):
    low = raw.lower()
    for pat, name in UNITS:
        if re.fullmatch(pat, low, re.I):
            return name
    return low


def label_for(line, span):
    """A short slug naming what the number refers to, from the words around it."""
    before = re.findall(r"[A-Za-z][A-Za-z0-9'_-]*", line[: span[0]])[-6:]
    after = re.findall(r"[A-Za-z][A-Za-z0-9'_-]*", line[span[1] :])[:3]
    words = [w for w in (before + after) if w.lower() not in STOP and len(w) > 1]
    # Capped at five words: enough to identify a value, short enough that the field is a name
    # rather than a sentence lifted out of the source.
    return "_".join(w.lower() for w in words[-5:]) or "unnamed"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    a = ap.parse_args()

    # gathered by document name, not by which bundle they landed in.
    #
    # Two of the five engine-wide documents do not reach _engine.walkthrough.json at all:
    #
    #  Goldeneye64CameraAndControlsVerbose.txt -> control.walkthrough.json
    #  Goldeneye64MultiplayerMechanicsVerbose.txt -> _mp.walkthrough.json
    #
    # The first is a NAME COLLISION: the filename contains "Control" and GoldenEye has a level
    # called Control, so engine-wide camera documentation is filed as a walkthrough for the
    # Control facility. That is the same failure the ingester already guards against for arenas
    # (<level>_mp, "so an arena guide can never be mistaken for the solo level"), on a different
    # axis. Together the two misfiled documents are ~55 KB, a quarter of the engine reference.
    #
    # Reading by document name rather than by bucket means this extractor is correct whether or
    # not the mapping is fixed, and does not silently lose a quarter of its input to somebody
    # else's routing bug. Reported at the end rather than repaired here: the mapping is
    # ingest_walkthroughs.py's business.
    ENGINE_DOC = re.compile(r"^Goldeneye64.*Verbose\.txt$", re.I)
    docs, misfiled = [], []
    for path in sorted(glob.glob(os.path.join(a.levels, "*.walkthrough.json"))):
        try:
            b = json.load(open(path, encoding="utf-8"))
        except Exception:
            continue
        bucket = os.path.basename(path)[: -len(".walkthrough.json")]
        for doc in b.get("documents", []):
            if ENGINE_DOC.match(os.path.basename(doc.get("source", ""))):
                docs.append(doc)
                if bucket != "_engine":
                    misfiled.append((os.path.basename(doc["source"]), bucket))
    if not docs:
        sys.exit("no engine documents found -- run tools/ingest_walkthroughs.py first")

    facts, seen = [], set()
    for doc in docs:
        name = os.path.basename(doc.get("source", "?"))
        for ln, line in enumerate(doc.get("text", "").splitlines(), 1):
            if len(line) > 400:
                continue          # tables and ASCII art: numbers here are layout, not values
            for m in FACT_RE.finditer(line):
                val = float(m.group(1))
                unit = canon_unit(m.group(2))
                # 0 and 1 with a unit are almost always prose ("1 second", "0 damage") rather
                # than a parameter worth checking, and they dominate the output otherwise.
                if val in (0.0, 1.0):
                    continue
                key = (label_for(line, m.span()), val, unit)
                if key in seen:
                    continue
                seen.add(key)
                facts.append({
                    "name": key[0],
                    "value": val,
                    "unit": unit,
                    "source": name,
                    "line": ln,
                    # Not a measurement. See the module docstring: this is what a community
                    # document asserts, kept so it can be CHECKED against the decompilation.
                    "status": "unverified",
                })

    facts.sort(key=lambda f: (f["unit"], -abs(f["value"])))
    out = {
        "note": ("CLAIMS extracted from third-party prose, NOT measurements. Every record is "
                 "status=unverified: check against the decompilation before relying on one."),
        "source_bundle": "_engine.walkthrough.json",
        "count": len(facts),
        "by_unit": {},
        "facts": facts,
    }
    for f in facts:
        out["by_unit"][f["unit"]] = out["by_unit"].get(f["unit"], 0) + 1

    dest = os.path.join(a.levels, "_engine.facts.json")
    with open(dest, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)

    print("%d engine document(s), %d facts -> %s" % (len(docs), len(facts), dest))
    for u, n in sorted(out["by_unit"].items(), key=lambda kv: -kv[1]):
        print("   %-8s %d" % (u, n))
    if misfiled:
        print("\n %d engine document(s) are NOT in the _engine bundle and were read from "
              "elsewhere:" % len(misfiled))
        for src, bucket in misfiled:
            print("     %-46s found in %s" % (src, bucket))
        print("   ingest_walkthroughs.py maps these by filename; 'Control' collides with the "
              "level of that name.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
