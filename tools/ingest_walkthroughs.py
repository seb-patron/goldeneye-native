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
import subprocess
import sys

# Page title fragment -> the extractor's level name. Explicit rather than fuzzy-matched: "Surface"
# appears in two different walkthroughs for two different levels, and a wrong mapping silently
# files one level's tactics under another.
LEVEL_HINTS = [
    # Both spacings: the wiki pages write "Bunker 1" and the reconstructions write "Bunker1".
    # A hint list that only knows one silently drops the other, and a level nobody notices is
    # missing is the failure this whole ingest is meant to avoid.
    ("bunker 1", "bunker1"), ("bunker1", "bunker1"),
    ("bunker 2", "bunker2"), ("bunker2", "bunker2"), ("silo", "silo"),
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


def read_any(path):
    """Plain text, HTML or RTF -- whichever this file turns out to be.

    RTF is not text with markup, it is a control-word format, and reading one as UTF-8 yields
    thousands of \\fonttbl and \\colortbl tokens that look enough like prose to pass a length
    check and be useless afterwards. textutil is macOS's own converter and gets it right; the
    fallback strips control words so a non-mac run degrades rather than fails.
    """
    low = path.lower()
    if low.endswith((".rtf", ".rtfd")):
        try:
            out = subprocess.run(["textutil", "-convert", "txt", "-stdout", path],
                                 capture_output=True, text=True, timeout=60)
            if out.returncode == 0 and len(out.stdout) > 200:
                return out.stdout
        except (OSError, subprocess.SubprocessError):
            pass
        with open(path, "rb") as fh:
            raw = fh.read().decode("utf-8", "replace")
        raw = re.sub(r"\\[a-zA-Z]+-?\d* ?", " ", raw)
        return re.sub(r"[{}]", " ", raw)

    with open(path, "rb") as fh:
        raw = fh.read().decode("utf-8", "replace")
    return strip_html(raw) if "<" in raw[:2000] else raw


def extract_blocks(text):
    """Pull any JSON object out of the prose.

    The verbose reconstructions embed machine-readable blocks -- Train's carries topology,
    principal_cars, traversal_axis, player_start. That is the half a tool can act on, and leaving
    it inside a wall of prose means every consumer re-reads the prose.

    ⚠️ Parsed, not regex-scraped. A block that does not parse is DROPPED rather than half-read:
    a partially understood spatial model is worse than none, because a consumer cannot tell.
    """
    out = []
    for m in re.finditer(r"\{[^{}]*\}", text, re.S):
        chunk = m.group(0)
        if len(chunk) < 40 or '"' not in chunk:
            continue
        try:
            out.append(json.loads(chunk))
        except ValueError:
            continue
    return out


def level_for(name):
    low = name.lower()
    # ⚠️ Multiplayer arenas share names with solo levels and are DIFFERENT PLACES -- Facility the
    # mission and Facility the arena have different geometry and different rules. Filing an arena
    # guide under the solo level would put wrong tactics on a level that already has right ones.
    if "multiplayer" in low:
        for hint, lvl in LEVEL_HINTS:
            if hint in low:
                return lvl + "_mp"
        return "_mp"
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
            # Engine-wide documents -- mechanics, camera and controls, collision and level space,
            # multiplayer rules -- are not about a level and were being discarded as unmapped.
            # They are the most broadly useful of the lot: a level guide helps one level, a
            # collision writeup helps every consumer of the navigation layer.
            low = entry.lower()
            if "goldeneye64" in low or "mechanic" in low or "collision" in low or "camera" in low:
                level = "_engine"
            else:
                unmapped.append(entry)
                continue

        text = read_any(path)
        if text is None:
            unmapped.append(entry + " (unreadable)")
            continue

        # Below this the page is navigation, comments and boilerplate rather than the guide.
        if len(text) < 400:
            unmapped.append(entry + " (too short)")
            continue

        blocks = extract_blocks(text)
        by_level.setdefault(level, []).append({"source": entry, "text": text,
                                               "structured": blocks})

    os.makedirs(args.out, exist_ok=True)
    for level, docs in sorted(by_level.items()):
        with open(os.path.join(args.out, level + ".walkthrough.json"), "w",
                  encoding="utf-8") as fh:
            json.dump({"level": level, "documents": docs,
                       "note": "third-party prose, for analysis only; not ground truth"},
                      fh, indent=1)
        chars = sum(len(d["text"]) for d in docs)
        blocks = sum(len(d["structured"]) for d in docs)
        print("  %-10s %d document(s), %6d chars, %d structured block(s)"
              % (level, len(docs), chars, blocks))

    print("\n%d level(s) covered" % len(by_level))
    if unmapped:
        # Named, because a silently skipped page is a level nobody notices is missing.
        print("UNMAPPED (%d): %s" % (len(unmapped), ", ".join(unmapped[:8])))


if __name__ == "__main__":
    main()
