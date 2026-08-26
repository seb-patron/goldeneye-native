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

The input record is {s8 stick_x, s8 stick_y, u8 button_low, u8 button_high}, which is the
SAME SHAPE as GePlayerInput. A decoded demo can be fed straight through gePlayerPost.

These are ROM-derived data. The decoded output stays out of git, like every other asset.
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
FIELDS_LEN = struct.calcsize(HDR)

# sizeof(ramromfilestructure). DERIVED, then confirmed against the data three ways -- see
# tools/audit_ramrom_header.py, which is the regression check for this number.
#
# ramromreplay.c:453 advances the read cursor by sizeof(struct ramromfilestructure), so the header
# length IS that struct's size. Computing it by hand from ramromreplay.h under MIPS alignment gives
# 228 padded to 232, with s32 filesize at offset 128 and enum LEVELID stagenum at 16. Hand
# arithmetic over twenty fields and a nested struct is not evidence, so both offsets were then
# searched for across all fourteen demos: 128 holds the filesize in every one, 16 holds the correct
# level id in every one.
HDR_LEN = 232
OFF_FILESIZE = 128

# The stored filesize is the demo's own length. The file on disk is padded up to a 16-byte ROM
# boundary, so the two are equal in only 3 of the 14 files -- use the stored value, not len(blob).
def align16(n):
    return (n + 15) & ~15

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
    filesize = struct.unpack_from(">I", blob, OFF_FILESIZE)[0]
    blocks, inputs, pos, overran = walk(blob, size_cmds)
    return {
        "file": os.path.basename(path),
        "bytes": len(blob),
        "randomseed": seed,
        "randomizer": rnd,
        "stage": stage,
        "level": STAGE_NAMES.get(stage, "?"),
        "difficulty": diff,
        "size_cmds": size_cmds,
        "filesize": filesize,
        "blocks": blocks,
        # An EXACT count now, not a ceiling. The old figure divided the whole file by 4, which
        # counts the interleaved seed records as though they were input, overstating every demo by
        # about a fifth.
        "records": inputs,
        # The walk must finish on the stored filesize. It ends 4 bytes short in all 14 files,
        # which is the terminator block (count and speedframes both zero) that ends the loop
        # before it is consumed. Anything else means the block structure is misread.
        "lands": (pos + 4 == filesize) and not overran,
    }


# N64 pad bits (PR/os.h) -> GE_IN_* (ge_player_api.h), obtained by INVERTING the forward mapping
# the port already uses in ge_player_api.c rather than by inventing one.
#
# byte order IS derived, not assumed. The record is {s8 stick_x, s8 stick_y, u8 button_low,
# u8 button_high} and the u16 is (button_high << 8) | button_low -- low byte FIRST in memory, which
# is not what a big-endian target would lead you to expect. Two independent checks settle it:
#   * bits 0x0040 and 0x0080 are assigned to no button on a real N64 pad, so they must never be set
#     in recorded play. Under this order they are clear in all 32,469 records; under the other,
#     173 records set one, which is physically impossible.
#   * under this order Z is the most-held button at 11.6% and A, B and START are each under 0.5%.
#     Z is fire. The other order puts the fire button essentially unused, which no play looks like.
N64_TO_GE = [
    (0x2000, "GE_IN_FIRE"),         # Z_TRIG
    (0x8000, "GE_IN_WEAPON_NEXT"),  # A_BUTTON is inventory in the port's mapping
    (0x4000, "GE_IN_USE"),          # B_BUTTON
    (0x1000, "GE_IN_START"),        # START_BUTTON
    (0x0008, "GE_IN_LOOK_UP"),      # U_CBUTTONS
    (0x0004, "GE_IN_LOOK_DOWN"),    # D_CBUTTONS
    (0x0002, "GE_IN_STEP_LEFT"),    # L_CBUTTONS
    (0x0001, "GE_IN_STEP_RIGHT"),   # R_CBUTTONS
    (0x0800, "GE_IN_DPAD_UP"),      # U_JPAD
    (0x0400, "GE_IN_DPAD_DOWN"),    # D_JPAD
    (0x0200, "GE_IN_DPAD_LEFT"),    # L_JPAD
    (0x0100, "GE_IN_DPAD_RIGHT"),   # R_JPAD
]

# L_TRIG IS the aim button IN these demos, and that is settled by the decomp, not inferred from
# frequency alone. bondview2.c:5546-5558 assigns the single-controller styles two ways:
#
#   KISSY / GOODNIGHT   shoot = A_BUTTON, aim = Z_TRIG,          inv = L_TRIG | R_TRIG
#   every other style   shoot = Z_TRIG,   aim = L_TRIG | R_TRIG, inv = A_BUTTON
#
# The measured corpus matches the second branch exactly: Z is the most-held button at 11.6%
# (shoot), A is 0.1% (inventory), L_TRIG is 2.8% (aim). So the demos were recorded on a default
# style and L_TRIG carries GE_IN_AIM.
#
# the port already does this correctly. ge_player_api.c:126 sets
# aim = swapped ? Z_TRIG : (L_TRIG | R_TRIG), which is right for a default single-controller
# style. The Z_TRIG assignment further down sits inside the GE_STYLE_IS_TWO_PAD branch and
# applies only to the 2.x styles. This table exists to DECODE recorded demos into GE_IN_*,
# not to replace that mapping.
#
# R_TRIG is never pressed in any of the fourteen, so the (L_TRIG | R_TRIG) pair is only ever seen
# as L_TRIG alone. Kept as a pair here because the game accepts either.
N64_AIM = [(0x0020, "GE_IN_AIM"), (0x0010, "GE_IN_AIM")]   # L_TRIG, R_TRIG

# Nothing is unmapped any more. Retained as an explicit empty list so that a future bit turning up
# in recorded play is reported rather than silently dropped.
UNMAPPED = []


def buttons_of(lo, hi):
    """The pad word for one record. See N64_TO_GE for why the order is this way round."""
    return (hi << 8) | lo


def translate(word):
    """(GE_IN_* names, unmapped names) for one pad word."""
    ge = [n for m, n in N64_TO_GE if word & m]
    # AIM is appended separately because L_TRIG and R_TRIG both carry it and would otherwise be
    # listed twice. Deduped against what is already there rather than by set(), which would lose
    # the stable order the table defines.
    for m, n in N64_AIM:
        if (word & m) and n not in ge:
            ge.append(n)
    return (ge, [n for m, n in UNMAPPED if word & m])


def walk(blob, size_cmds):
    """Walk the block stream: a 4-byte seed record, then size_cmds*4*count bytes of input.

    From iterate_ramrom_entries_handle_camera_out. The advance is written there as
    align_addr_even(size_cmds * 4 * count + 5), and align_addr_even rounds DOWN to even
    (((X|1)^1)). size_cmds*4*count is always a multiple of 4, so +5 rounded down to even is simply
    +4: the seed record itself. The "5 is ??" note in the decomp resolves to nothing mysterious.
    """
    pos, blocks, inputs, overran = HDR_LEN, 0, 0, False
    while pos + 4 <= len(blob):
        speedframes, count = struct.unpack_from(">BB", blob, pos)[:2]
        if count == 0 and speedframes == 0:
            break                       # terminator
        n = size_cmds * 4 * count
        if pos + 4 + n > len(blob):
            overran = True
            break
        blocks += 1
        inputs += size_cmds * count
        pos += 4 + n
    return blocks, inputs, pos, overran


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
        total += d["records"]
        print("%-24s %-10s %5d %6d %9d  0x%016x"
              % (d["file"], d["level"], d["stage"], d["difficulty"],
                 d["records"], d["randomseed"]))

    bad = [d["file"] for d in (decode(q) for q in files) if d and not d["lands"]]
    print("")
    print("%d demos, %d input records total (exact, seed records excluded)"
          % (len(files), total))
    if bad:
        print("STREAM MISREAD in: %s" % ", ".join(bad))
    else:
        print("Every stream walks to its stored filesize; block structure confirmed.")
    print("These are recorded HUMAN play. Levels covered: %s"
          % ", ".join(sorted({decode(p)["level"] for p in files if decode(p)})))


if __name__ == "__main__":
    main()
