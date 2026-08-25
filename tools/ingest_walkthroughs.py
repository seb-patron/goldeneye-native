#!/usr/bin/env python3
"""Turn saved walkthrough pages into per-level notes the tools and bots can read.

WHY

The extraction knows where everything IS. It does not know what a level is FOR: which order the
objectives are done in, which door needs a key, that Train is a linear chain of carriages, that a
guard comes from behind at a particular point. People wrote all of that down years ago, and it is
the layer no amount of asset reading recovers.

It is also a check on our own data. Train's walkthrough describes seven brake units in a linear
run of cars; if the route graph cannot reproduce that shape, the graph is wrong and the
walkthrough is how we find out.

⚠️ THIS IS THIRD-PARTY PROSE, NOT GROUND TRUTH. It is written for humans, it covers difficulty
tiers we may not model, and some of it is folklore. Treat a claim here the way the research docs
treat a FOLKLORE tag: useful, and not a measurement. The extracted text keeps its source so a
surprising line can be traced back.

Copyright: these are saved pages from other people's sites. The text is ingested for local
analysis only and the output stays out of git -- see .gitignore. Nothing derived from it should
be published without permission.

USAGE
    python3 tools/ingest_walkthroughs.py --src ~/Desktop/GoldeneyeWalkthrough
"""
import argparse
import html
import json
import os
import re
import sys

# Page title fragment -> the extractor's level name. Explicit rather than fuzzy-matched: "Surface"
# appears in two different walkthroughs for two different levels, and a wrong mapping silently
# files one level's tactics under another.
LEVEL_HINTS = [
    ("bunker 1", "bunker1"), ("bunker 2", "bunker2"), ("silo", "silo"),
    ("statue", "statue"), ("control", "control"), ("archives", "archives"),
    ("train", "train"), ("frigate", "frigate"), ("aztec", "aztec"),
    ("streets", "streets"), ("depot", "depot"), ("egypt", "egypt"),
    ("dam", "dam"), ("facility", "facility"), ("runway", "runway"),
    ("surface 2", "surface2"), ("5-1: surface", "surface2"), ("2-1: surface", "surface"),
    ("surface", "surface"), ("jungle", "jungle"), ("caverns", "caverns"),
    ("cradle", "cradle"), ("temple", "temple"), ("cuba", "cuba"),
]

TAG = re.compile(r"<[^>]+>")
WS = re.compile(r"[ \t\r\f\v]+")
BLANK = re.compile(r"\n{3,}")


def strip_html(raw):
    # Script and style first: their contents are text to a tag stripper and noise to a reader.
    raw = re.sub(r"(?is)<(script|style)[^>]*>.*?</\1>", " ", raw)
    raw = re.sub(r"(?i)<(br|/p|/div|/li|/h[1-6])[^>]*>", "\n", raw)
    text = html.unescape(TAG.sub(" ", raw))
    text = WS.sub(" ", text)
    return BLANK.sub("\n\n", "\n".join(ln.strip() for ln in text.splitlines())).strip()


def level_for(name):
    low = name.lower()
    for hint, level in LEVEL_HINTS:
        if hint in low:
            return level
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.expanduser("~/Desktop/GoldeneyeWalkthrough"))
    ap.add_argument("--out", default=os.path.join("build", "levels"))
    args = ap.parse_args()

    if not os.path.isdir(args.src):
        sys.exit("no walkthrough folder at %s" % args.src)

    by_level = {}
    unmapped = []
    for entry in sorted(os.listdir(args.src)):
        path = os.path.join(args.src, entry)
        if os.path.isdir(path) and entry.endswith(".rtfd"):
            path = os.path.join(path, "TXT.rtf")
            if not os.path.exists(path):
                continue
        if not os.path.isfile(path):
            continue

        level = level_for(entry)
        if level is None:
            unmapped.append(entry)
            continue

        with open(path, "rb") as fh:
            raw = fh.read().decode("utf-8", "replace")
        text = strip_html(raw) if "<" in raw[:2000] else raw

        # Below this the page is navigation, comments and boilerplate rather than the guide.
        if len(text) < 400:
            unmapped.append(entry + " (too short)")
            continue

        by_level.setdefault(level, []).append({"source": entry, "text": text})

    os.makedirs(args.out, exist_ok=True)
    for level, docs in sorted(by_level.items()):
        with open(os.path.join(args.out, level + ".walkthrough.json"), "w",
                  encoding="utf-8") as fh:
            json.dump({"level": level, "documents": docs,
                       "note": "third-party prose, for analysis only; not ground truth"},
                      fh, indent=1)
        chars = sum(len(d["text"]) for d in docs)
        print("  %-10s %d document(s), %d chars" % (level, len(docs), chars))

    print("\n%d level(s) covered" % len(by_level))
    if unmapped:
        # Named, because a silently skipped page is a level nobody notices is missing.
        print("UNMAPPED (%d): %s" % (len(unmapped), ", ".join(unmapped[:8])))


if __name__ == "__main__":
    main()
