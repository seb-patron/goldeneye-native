#!/usr/bin/env python3
"""Which level does each walkthrough actually describe, and which names are lying?

Mining harder on mislabelled documents does not produce more knowledge, it produces more
CONFIDENTLY WRONG knowledge, filed under levels the text never mentions. So this runs first.

WHAT IT FOUND. Of 36 files only 28 are distinct: seven groups are byte-identical copies under
different names. They are not shared documents, they are misfiled ones, and the content says so
without ambiguity:

    RunwayVerbose.txt          says "runway"   0 times, "bunker"   25 times
    SiloVerbose.txt            says "silo"     0 times, "bunker"   25 times
    StackMultiplayerVerbose    says "stack"    0 times, "basement" 43 times
    EgyptianMultiplayerVerbose says "egypt"    0 times, "caverns"  27 times

A document that never once names the level it is filed under is not about that level. The
downstream damage was already visible before this tool existed: runway, silo and bunker2 had
IDENTICAL extraction counts -- 12 entities, 13 relations, 7 conditions each -- because all three
were the same Bunker 2 text counted three times.

HOW A FILE'S REAL SUBJECT IS DECIDED. Not by its name. Every level name is counted in the body and
the leader wins, with the first line used as a tie-break because these documents open by naming
their subject. Where the leader is not the filename's level, the file is reported as misfiled and
the level it claims to cover is reported as HAVING NO DOCUMENT, which is the honest state -- better
than silently inheriting another level's text.

WHAT THIS DOES NOT DO: it renames nothing and deletes nothing. The corpus is the author's. It
emits the mapping that a miner should use, and says plainly which levels have no source at all.

Third-party and author material both stay out of git; this prints and writes only to build/.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Level names as they appear in prose, mapped to the level keys the rest of the pipeline uses.
# "bunker" is deliberately NOT mapped: it is ambiguous between bunker1 and bunker2 and guessing
# would reintroduce exactly the misattribution this file exists to catch.
ALIASES = {
    "dam": "dam", "facility": "facility", "runway": "runway", "silo": "silo",
    "frigate": "frigate", "statue": "statue", "archives": "archives", "streets": "streets",
    "depot": "depot", "train": "train", "jungle": "jungle", "control": "control",
    "caverns": "caverns", "cradle": "cradle", "aztec": "aztec", "egypt": "egypt",
    "egyptian": "egypt", "surface": "surface", "basement": "basement", "stack": "stack",
    "complex": "complex", "temple": "temple", "library": "library", "caves": "caves",
    "bunker 1": "bunker1", "bunker 2": "bunker2", "bunker1": "bunker1", "bunker2": "bunker2",
    "surface 2": "surface2", "surface2": "surface2",
}

# Filename stem -> the level it CLAIMS to be about, or None for a document that is not about a
# level at all.
#
# The Goldeneye64* files are ENGINE-WIDE prose -- the bible, mechanics, camera and controls,
# collision. They name whichever level they happen to use as an example, so scoring them as level
# documents reports every one of them as misfiled against a level they were never claiming. That
# is a bug in the question, not a finding, and the first run of this tool produced four of them.
ENGINE_PREFIX = "goldeneye64"

def claimed(name):
    s = name[:-4] if name.lower().endswith(".txt") else name
    if s.lower().startswith(ENGINE_PREFIX):
        return None                      # engine-wide, not a level document
    # Order matters: strip the suffixes before splitting, or "ControlVerbose_Two" survives as
    # "control verbose.txt" and is then reported as a level that does not exist.
    for suf in ("_Two", "_two", "_2"):
        if s.endswith(suf):
            s = s[: -len(suf)]
    if s.endswith("Verbose"):
        s = s[: -len("Verbose")]
    s = s.replace("Multiplayer", "")
    key = s.lower()
    # Exact keys first, so Surface2 does not degrade into "surface" and Bunker1 into "bunker".
    direct = {"surface2": "surface2", "surface": "surface", "bunker1": "bunker1",
              "bunker2": "bunker2", "egyptian": "egypt",
              # A typo in the corpus: this file is a byte-identical copy of the Facility MP text.
              "bubnker": None, "bunker": None}
    if key in direct:
        return direct[key]
    spaced = re.sub(r"(?<=[a-z])(?=[A-Z0-9])", " ", s).strip().lower()
    return ALIASES.get(key, ALIASES.get(spaced))


def subject(text):
    """Which level the body actually talks about, by count."""
    low = text.lower()
    tally = collections.Counter()
    for alias, key in ALIASES.items():
        n = len(re.findall(r"\b" + re.escape(alias) + r"\b", low))
        if n:
            tally[key] += n
    return tally


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "walkthroughs"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "levels",
                                                  "_walkthrough.attribution.json"))
    a = ap.parse_args()
    if not os.path.isdir(a.dir):
        sys.exit("no %s" % a.dir)

    files = sorted(f for f in os.listdir(a.dir) if f.lower().endswith(".txt"))
    by_hash, recs = collections.defaultdict(list), {}
    for f in files:
        raw = open(os.path.join(a.dir, f), "rb").read()
        txt = raw.decode("utf-8", errors="replace")
        by_hash[hashlib.sha256(raw).hexdigest()].append(f)
        tally = subject(txt)
        recs[f] = {
            "claims": claimed(f),
            "tally": tally,
            "first": next((l.strip() for l in txt.splitlines() if l.strip()), "")[:70],
            "lines": sum(1 for l in txt.splitlines() if l.strip()),
        }

    dup_groups = [g for g in by_hash.values() if len(g) > 1]
    print("%d files, %d distinct by content, %d duplicate group(s)"
          % (len(files), len(by_hash), len(dup_groups)))

    misfiled, orphaned, good, engine = [], [], [], []
    for f, r in recs.items():
        top = r["tally"].most_common(1)
        lead = top[0][0] if top else None
        own = r["tally"].get(r["claims"], 0)
        # A file that never names its own level is misfiled. Using "never once" rather than a
        # ratio keeps this from firing on a document that legitimately compares two levels --
        # SurfaceVerbose talks about Bunker more than Surface, but it does open by naming Surface
        # and mentions it 50 times, so it is a comparison, not a mislabel.
        if r["claims"] is None:
            engine.append((f, lead, r["tally"].get(lead, 0) if lead else 0))
        elif own == 0 and lead and lead != r["claims"]:
            misfiled.append((f, r["claims"], lead, r["tally"].get(lead, 0)))
        else:
            good.append((f, r["claims"], own))
    print("\nENGINE-WIDE PROSE, not about any one level (scored separately):")
    for f, lead, n in sorted(engine):
        print("   %-46s most-named level: %-9s x%d" % (f, lead or "-", n))

    print("\nMISFILED -- the file never once names the level it is filed under:")
    for f, cl, lead, n in sorted(misfiled):
        print("   %-34s filed as %-9s but says %-9s x%d, %s x0" % (f, cl, lead, n, cl))

    # A level is covered when a file filed under it actually talks about it. Taken from the good
    # list directly rather than re-derived, which is what the first version did and got wrong.
    # None is excluded: engine prose claims no level, and leaving it in put a None in a set that
    # then failed to sort.
    covered = {cl for _f, cl, _n in good if cl}
    claimed_all = {r["claims"] for r in recs.values() if r["claims"]}
    orphaned = sorted(claimed_all - covered)
    print("\nLEVELS WHOSE ONLY FILE IS ANOTHER LEVEL TEXT (no source of their own):")
    print("   %s" % (", ".join(orphaned) if orphaned else "none"))

    print("\nDUPLICATE GROUPS (byte-identical):")
    for g in sorted(dup_groups):
        print("   %s" % ", ".join(sorted(g)))

    out = {
        "files": len(files),
        "distinct": len(by_hash),
        "duplicate_groups": [sorted(g) for g in dup_groups],
        "misfiled": [{"file": f, "filed_as": c, "actually": l, "mentions": n}
                     for f, c, l, n in sorted(misfiled)],
        "levels_without_document": orphaned,
        "usable": {f: r["claims"] for f, r, in
                   ((f, recs[f]) for f in sorted(recs))
                   if recs[f]["tally"].get(recs[f]["claims"], 0) > 0},
    }
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)
    print("\n-> %s" % a.out)
    print("\n%d of %d files are usable as evidence about the level they are filed under."
          % (len(out["usable"]), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
