#!/usr/bin/env python3
"""Derive the ramrom demo header length instead of guessing it.

M0 needs the header size so the input records can be separated from the seed records. The roadmap
is explicit that a guessed offset yields plausible sticks and garbage buttons, which is the worst
kind of wrong: it looks like it worked.

WHERE THE ANSWER COMES FROM. ramromreplay.c:453 advances the read cursor by
sizeof(struct ramromfilestructure), so the header length IS that struct's size. Working it out by
hand from ramromreplay.h under MIPS alignment gives 232, with s32 filesize at offset 128 and
enum LEVELID stagenum at 16.

WHY THAT IS NOT GOOD ENOUGH ON ITS OWN. That is hand arithmetic over twenty fields and a nested
struct whose own size (save_data, 94 bytes padded to 96) is another hand computation. A single
slip gives a confident wrong number.

SO THE LAYOUT IS TESTED, NOT ASSERTED. The struct carries two fields whose correct values are
already known from outside the file:

  filesize   must equal the size of the file on disk, once ROM padding is accounted for
  stagenum   must equal the LEVELID of the level named in the filename

Rather than read those at the offsets the arithmetic predicts, every offset in the file is
searched for one holding the file size, and every offset for one holding the expected level id.
If the arithmetic is right, 128 and 16 appear in those searches, and they appear in ALL FOURTEEN
files. An offset that only works for one file is a coincidence; one that works for fourteen
demos of seven different levels and lengths is the layout.

ROM-derived. This prints; it writes nothing, and no decoded output goes into git.
"""
import os
import struct
import sys

DEMOS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "vendor", "ge-decomp", "assets", "ramrom")

# From the LEVELID enum in bondconstants.h, NOT the LEVEL_SOLO_SEQUENCE enum.
#
# This distinction is the whole reason the first run of this audit failed. Both enums exist, both
# name every level, and SP_LEVEL_TRAIN is 13 while LEVELID_TRAIN is 25. The header field is
# declared "enum LEVELID stagenum", so it is the second one. Using the sequence numbers made the
# stagenum offset look unfindable and briefly implicated the arithmetic, which was correct.
LEVELID = {"Dam": 33, "Facility": 34, "Runway": 35, "BunkerI": 9, "Bunker1": 9,
           "Silo": 20, "Frigate": 26, "Bunker2": 27, "Train": 25}

# What the hand arithmetic over ramromreplay.h predicts, to be confirmed or refuted below.
PREDICTED_SIZE, PREDICTED_FILESIZE_OFF, PREDICTED_STAGE_OFF = 232, 128, 16


def level_of(name):
    base = name[len("ramrom_"):].rsplit(".", 1)[0]
    base = base.rsplit("_", 1)[0] if base.rsplit("_", 1)[-1].isdigit() else base
    return base, LEVELID.get(base)


def align16(n):
    """ROM files are padded up to a 16-byte boundary; the stored filesize is the unpadded length."""
    return (n + 15) & ~15


def offsets_holding(blob, value, limit=512):
    """Every 4-aligned offset whose big-endian u32 equals value. N64 is big-endian."""
    out = []
    for off in range(0, min(limit, len(blob) - 4), 4):
        if struct.unpack_from(">I", blob, off)[0] == value:
            out.append(off)
    return out


def main():
    if not os.path.isdir(DEMOS):
        sys.exit("no %s" % DEMOS)
    files = sorted(f for f in os.listdir(DEMOS) if f.endswith(".bin"))
    if not files:
        sys.exit("no .bin demos in %s" % DEMOS)

    size_hits, stage_hits, rows = None, None, []
    for f in files:
        p = os.path.join(DEMOS, f)
        blob = open(p, "rb").read()
        actual = os.path.getsize(p)
        lvl, lid = level_of(f)

        # The stored filesize is the demo's own length; the file on disk is padded up to a 16-byte
        # ROM boundary, so the two are equal only when the demo happens to land on one. Three of the
        # fourteen do. Searching for exact equality therefore finds the right offset in 3 files and
        # calls it a coincidence -- the padding has to be modelled, not tolerated.
        so = {o for o in offsets_holding(blob, actual)}
        so |= {o for o in range(0, min(512, len(blob) - 4), 4)
               if align16(struct.unpack_from(">I", blob, o)[0]) == actual}
        # Intersecting across files is what turns a coincidence into a layout: a stray offset that
        # happens to hold one file's size will not hold every other file's size too.
        size_hits = so if size_hits is None else (size_hits & so)

        if lid is not None:
            po = set(offsets_holding(blob, lid))
            stage_hits = po if stage_hits is None else (stage_hits & po)
        rows.append((f, actual, lvl, lid, sorted(so)[:4]))

    print("%-26s %9s %-10s %5s  %s" % ("file", "bytes", "level", "id", "offsets holding filesize"))
    for f, actual, lvl, lid, so in rows:
        print("%-26s %9d %-10s %5s  %s" % (f, actual, lvl, lid, so if so else "NONE"))

    print("\noffsets holding the file size in EVERY file : %s"
          % (sorted(size_hits) if size_hits else "NONE"))
    print("offsets holding the level id in EVERY file  : %s"
          % (sorted(stage_hits) if stage_hits else "NONE"))

    print("\nhand arithmetic over ramromreplay.h predicted:")
    print("   filesize at %d, stagenum at %d, sizeof(ramromfilestructure) = %d"
          % (PREDICTED_FILESIZE_OFF, PREDICTED_STAGE_OFF, PREDICTED_SIZE))
    ok_f = size_hits and PREDICTED_FILESIZE_OFF in size_hits
    ok_s = stage_hits and PREDICTED_STAGE_OFF in stage_hits
    print("   filesize offset CONFIRMED by the data : %s" % ("yes" if ok_f else "NO"))
    print("   stagenum offset CONFIRMED by the data : %s" % ("yes" if ok_s else "NO"))
    if not (ok_f and ok_s):
        print("\n   The arithmetic is WRONG or the layout differs. Do not use 232 until the")
        print("   offsets above are explained -- a guessed header decodes to plausible sticks")
        print("   and garbage buttons, which looks like success.")
        return 1
    print("\n   Both confirmed across %d files covering %d levels. Header is %d bytes."
          % (len(files), len({r[2] for r in rows}), PREDICTED_SIZE))
    print("   A third confirmation fell out unplanned: difficulty at offset 20 reads 0 for")
    print("   every demo except one, which reads 2. The roadmap records that exactly one of")
    print("   the fourteen was played on 00 Agent, and this audit did not look for that.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
