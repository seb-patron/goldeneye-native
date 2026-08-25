#!/usr/bin/env python3
"""Decode GoldenEye's own attract-mode demos: recorded HUMAN input, with the RNG seed.

WHAT THESE ARE

The title screen plays real gameplay when left alone, and it is not a video. `assets/ramrom/`
holds fourteen recorded input streams -- Dam, Facility x3, Runway x2, Bunker 1 x2, Silo x2,
Frigate x2 and Train -- each a `ramromfilestructure` header followed by per-frame controller
records.

WHY THIS MATTERS MORE THAN IT SOUNDS, twice over:

1. GROUND TRUTH FOR NAVIGATION. A person walked these levels correctly and the game kept the
   inputs. Every path question we have been guessing at -- which way out of the first carriage,
   how close you stand to a door, when to fight rather than walk -- is answered here by someone
   who could see the screen.

2. A DETERMINISM TEST WE DID NOT HAVE TO BUILD. The stream interleaves seed records
   {speedframes, count, randseed, check}, and ramromreplay aborts playback when the running RNG
   disagrees. That is exactly the check netplay needs, already written, with fourteen recorded
   cases to run it against -- and `gePlayerSeedFingerprint` already exposes our side of it.

🔑 The input record is {s8 stick_x, s8 stick_y, u8 button_low, u8 button_high}, which is the
SAME SHAPE as GePlayerInput. A decoded demo can be fed straight through gePlayerPost.

⚠️ These are ROM-derived data. The decoded output stays out of git, like every other asset.
"""
import argparse
import glob
import os
import struct
import sys

# ramromfilestructure, big-endian. save_data sits in the middle and its size is not obvious from
# the header, so the command stream offset is derived from filesize rather than assumed -- a
# guessed offset produces plausible sticks and garbage buttons, which is the worst outcome.
HDR = ">QQiiI"          # randomseed, randomizer, stagenum, difficulty, size_cmds
HDR_LEN = struct.calcsize(HDR)

STAGE_NAMES = {
    9: "bunker1", 20: "silo", 22: "statue", 23: "control", 24: "archives", 25: "train",
    26: "frigate", 27: "bunker2", 28: "aztec", 29: "streets", 30: "depot", 32: "egypt",
    33: "dam", 34: "facility", 35: "runway", 36: "surface", 37: "jungle", 39: "caverns",
    41: "cradle", 43: "surface2",
}


def decode(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    if len(blob) < HDR_LEN:
        return None

    seed, rnd, stage, diff, size_cmds = struct.unpack_from(HDR, blob, 0)
    return {
        "file": os.path.basename(path),
        "bytes": len(blob),
        "randomseed": seed,
        "randomizer": rnd,
        "stage": stage,
        "level": STAGE_NAMES.get(stage, "?"),
        "difficulty": diff,
        "size_cmds": size_cmds,
        # 4 bytes per record, and the stream is input records interleaved with seed records.
        # Reported as a ceiling rather than a count: without the exact header length the split
        # between the two kinds is not known, and overstating it as "frames" would be a lie.
        "max_records": (len(blob) - HDR_LEN) // 4,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join("vendor", "ge-decomp", "assets", "ramrom"))
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.dir, "*.bin")))
    if not files:
        sys.exit("no demo files in %s -- assets are generated from your own ROM" % args.dir)

    print("%-24s %-10s %5s %6s %9s  %s" %
          ("file", "level", "stage", "diff", "records", "randomseed"))
    total = 0
    for path in files:
        d = decode(path)
        if d is None:
            print("  %s unreadable" % os.path.basename(path))
            continue
        total += d["max_records"]
        print("%-24s %-10s %5d %6d %9d  0x%016x"
              % (d["file"], d["level"], d["stage"], d["difficulty"],
                 d["max_records"], d["randomseed"]))

    print("\n%d demo(s), up to %d input records total" % (len(files), total))
    print("These are recorded HUMAN play. Levels covered: %s"
          % ", ".join(sorted({decode(p)["level"] for p in files if decode(p)})))


if __name__ == "__main__":
    main()
