#!/usr/bin/env python3
"""Longest contiguous stretch confined to a 400-unit patch, from a GETV_BOT_LOG TSV.

Written to verify (or refute) the stuck-cycle escalation fix in ge_bot_route.c against the
pre-fix baseline, using the same method the finding itself was measured with -- a loose windowed
heuristic produced a meaningless 56% hit rate the first time this was checked by hand; this is
the corrected version, kept as a tool rather than a one-off so the comparison is reproducible and
so the same check can be re-run after any future change to the stuck/detour logic.
"""
import argparse
import csv
import os
import sys


def longest_confined_stretch(posts, radius=400.0):
    xs = [float(r["x"]) for r in posts]
    zs = [float(r["z"]) for r in posts]
    n = len(posts)
    best_len, best_start, i = 0, 0, 0
    while i < n:
        x0, z0 = xs[i], zs[i]
        j = i
        while j < n and ((xs[j] - x0) ** 2 + (zs[j] - z0) ** 2) ** 0.5 <= radius:
            j += 1
        if j - i > best_len:
            best_len, best_start = j - i, i
        i = j if j > i else i + 1
    return best_len, best_start


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--radius", type=float, default=400.0)
    a = ap.parse_args()

    print("%-18s %8s %10s %8s %10s" % ("run", "frames", "confined", "share", "start_frame"))
    for p in a.paths:
        if not os.path.isfile(p):
            print("%-18s MISSING" % os.path.basename(p))
            continue
        with open(p, encoding="utf-8") as fh:
            rows = list(csv.DictReader(fh, delimiter="\t"))
        posts = [r for r in rows if r["event"] == "post"]
        if not posts:
            print("%-18s no post rows" % os.path.basename(p))
            continue
        best_len, best_start = longest_confined_stretch(posts, a.radius)
        name = os.path.basename(p).replace(".tsv", "")
        print("%-18s %8d %10d %7.0f%% %10s"
              % (name, len(posts), best_len, 100.0 * best_len / len(posts),
                 posts[best_start]["frame"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
