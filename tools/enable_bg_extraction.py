#!/usr/bin/env python3
"""
Turn on ROM extraction for the level background files.

Why this exists
---------------
`scripts/filelist.u.csv` drives extract_baserom.u.sh. Each row is

    offset,size,name,compressed,extract

and the extractor skips any row whose `extract` column is 0. Of the 34
assets/obseg/bg/*.bin rows, 25 ship with that column set to 0.

Upstream can afford that: it assembles those backgrounds from checked-in .c
files and never needs the raw blobs. This port compiles the blobs directly, so
a clean checkout links against 25 symbols that were never written to disk. The
failure is a wall of undefined-symbol errors at link time with nothing earlier
in the build to explain them.

The offsets and sizes in those rows are correct - every one of the 25 extracts
byte-for-byte from an unmodified NTSC ROM. Only the flag is wrong for our
purposes. So flip it, rather than carrying 5 MB of ROM-derived blobs in git or
asking anyone to hand-extract them.

Idempotent: re-running on an already-patched file reports 0 changes.
"""

import sys
from pathlib import Path

CSV = Path("scripts/filelist.u.csv")
TARGET = "assets/obseg/bg/"


def main() -> int:
    if not CSV.is_file():
        sys.stderr.write(
            f"{CSV} not found - run this from the decomp root (vendor/ge-decomp).\n"
        )
        return 1

    # Read as bytes and rewrite line by line: the extractor is picky about
    # line endings and a trailing newline, so preserve the file verbatim
    # apart from the one column being changed.
    lines = CSV.read_bytes().split(b"\n")
    changed = 0
    already = 0

    for i, line in enumerate(lines):
        if TARGET.encode() not in line:
            continue
        cols = line.split(b",")
        if len(cols) < 5:
            continue
        if cols[4].strip() == b"0":
            cols[4] = b"1" + cols[4][len(cols[4].rstrip()):]
            lines[i] = b",".join(cols)
            changed += 1
        elif cols[4].strip() == b"1":
            already += 1

    if changed:
        CSV.write_bytes(b"\n".join(lines))

    total = changed + already
    print(f"{CSV}: {total} background rows, {changed} switched on, {already} already on")
    if not total:
        sys.stderr.write(f"no rows matched {TARGET} - is this the right filelist?\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
