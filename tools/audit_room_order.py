#!/usr/bin/env python3
"""M6: is the transparent pass actually drawn back to front?

Run the game with GETV_ORDER=1 and feed the log to this. bg.c buckets visible rooms by
`s_bound_info.unk1` and walks the buckets ascending for the primary (opaque) pass and descending
for the secondary (blended) pass. Blended geometry has no depth-buffer answer to fall back on, so
if the secondary pass is not back to front the result depends on draw order -- and "which surface
wins varies with view angle" is exactly what that looks like.

WHAT THIS FOUND ON TRAIN: 163 of 699 frames draw the blended pass out of depth order, and
159 of those inversions are ACROSS buckets rather than within one.

    4 rooms visible    0% of frames out of order
    3 rooms visible  100%
    2 rooms visible   47%

THE BUCKETS INTERLEAVE IN DEPTH. Frame 389 draws b1/r1 at z=-127, b1/r3 at z=-114, then b0/r2 at
z=-122 -- room 2 is depth-wise BETWEEN the two rooms in bucket 1, but the bucket walk must emit all
of bucket 1 before any of bucket 0. No amount of sorting within a bucket fixes that: `unk1` simply
does not encode a depth order.

It reads as correct at the spawn because looking straight down a linear train gives four rooms
whose bucket numbers happen to ascend with distance. Turn far enough that the camera sees rooms
whose bucket assignment disagrees with their depth, and the order inverts.

I GOT THIS WRONG TWICE BEFORE THE TOOL WAS WRITTEN, WHICH IS WHY IT EXISTS. Reading one frame
suggested the bucket sequence was fine and only intra-bucket ties were unsorted; reading three
suggested the same. Across 699 frames the split is 159 across-bucket against 4 within. Three frames
is not a sample, and an eyeballed pattern from a scrolling log is a hypothesis, not a measurement.

THIS DOES NOT BY ITSELF PROVE IT CAUSES THE STEEL-PLATE ARTEFACT. It proves the blended pass is
depth-inverted on 23% of frames, and that the inversions are INTRA-bucket. Whether the plate is
blended geometry in a shared bucket is a separate question that needs the asset checked. Reported
as a mechanism with a measured rate, not as a diagnosis.

AND IT MAY BE FAITHFUL. The buckets come from the game's own data, so hardware had the same
intra-bucket ambiguity. Before "fixing" it, establish whether the N64 result differs -- a port that
sorts more carefully than the original is still a port that renders something the original did not.
"""
import argparse
import collections
import re
import sys

FRAME = re.compile(r"\[getv\]\[order\] FRAME rooms=(\d+) bucketspan=\[(\d+)\.\.(\d+)\]")
SEC = re.compile(r"\[getv\]\[order\]\s+SEC bucket=(\d+) room=(\d+) camz=(?:\(novtx\))?(-?[\d.]+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", help="stdout of a run with GETV_ORDER=1")
    a = ap.parse_args()

    frames, cur = [], None
    for line in open(a.log, encoding="utf-8", errors="replace"):
        m = FRAME.match(line)
        if m:
            cur = {"rooms": int(m.group(1)), "lo": int(m.group(2)), "hi": int(m.group(3)), "sec": []}
            frames.append(cur)
            continue
        m = SEC.match(line)
        if m and cur is not None:
            cur["sec"].append((int(m.group(1)), int(m.group(2)), float(m.group(3))))

    if not frames:
        sys.exit("no GETV_ORDER frames in %s -- was it run with GETV_ORDER=1?" % a.log)

    tot, bad, intra, inter = collections.Counter(), collections.Counter(), 0, 0
    for f in frames:
        # camz 0.0 means the depth could not be computed for that room this frame ("novtx"), so it
        # carries no ordering information and must not be compared -- including it would invent
        # inversions against a value that was never measured.
        s = [(b, r, z) for (b, r, z) in f["sec"] if z != 0.0]
        if len(s) < 2:
            continue
        tot[f["rooms"]] += 1
        inverted = False
        for j in range(len(s) - 1):
            if s[j][2] > s[j + 1][2]:      # nearer drawn before further, in a back-to-front pass
                inverted = True
                if s[j][0] == s[j + 1][0]:
                    intra += 1             # same bucket: no sort exists between them
                else:
                    inter += 1             # different buckets: the bucket walk itself is wrong
        if inverted:
            bad[f["rooms"]] += 1

    n_tot, n_bad = sum(tot.values()), sum(bad.values())
    print("frames with comparable depths: %d" % n_tot)
    print("out of order:                  %d (%.0f%%)" % (n_bad, 100.0 * n_bad / max(1, n_tot)))
    print("\nby number of rooms visible:")
    for r in sorted(tot):
        print("   %d rooms: %4d of %4d out of order (%3.0f%%)"
              % (r, bad.get(r, 0), tot[r], 100.0 * bad.get(r, 0) / tot[r]))

    # The distinction that says whose bug it is.
    print("\ninversions by kind:")
    print("   within one bucket : %d   -- rooms sharing a bucket, unsorted against each other" % intra)
    print("   across buckets    : %d   -- bucket order disagrees with depth order" % inter)
    if inter > intra:
        print("\n   THE BUCKETS INTERLEAVE IN DEPTH: a room in a later bucket sits nearer than one")
        print("   in an earlier bucket, so walking buckets in order cannot be back-to-front no")
        print("   matter how each bucket is sorted internally. `unk1` does not encode depth.")
    elif intra:
        print("\n   Inversions are confined to rooms sharing a bucket, where no order exists.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
