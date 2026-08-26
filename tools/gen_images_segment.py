#!/usr/bin/env python3
"""
Emit the combined images ROM segment as a C array.

Why this exists
---------------
`ge007.ld` places `assets/images/combined/combined.o` into the images segment. Upstream
builds that object straight from `combined.bin` with `ld -r -b binary`, which is a GNU
extension; Mach-O has no equivalent, and neither does link.exe. Emitting the same bytes as
a C array gives the port a real symbol to hand `image.c`, and works on every toolchain.

`combined.bin` itself is produced by the two steps named in the generated file's header,
both of which run earlier in the asset pipeline:

    python3 scripts/make/sync_imagelist_with_def.py build/imagelist.csv
    bash  scripts/make/combine_images_named.sh build/imagelist.csv assets/images/combined

The output is ROM-derived and must never be committed. It is roughly 10.8 MB of C for a
3 MB segment, which is also why it is generated rather than carried in a patch.
"""

import sys
from pathlib import Path

SRC = Path("assets/images/combined/combined.bin")
DST = Path("assets/images/ge_images_segment.c")
PER_LINE = 32

HEADER = '''/* GENERATED - the "images" ROM segment as C data.
 *
 * ge007.ld places assets/images/combined/combined.o into the images segment, built
 * from combined.bin via `ld -r -b binary` (GNU-only, and Mach-O has no equivalent).
 * Emitted as a C array instead, giving the port a real symbol to hand image.c.
 *
 * Rebuild:
 *   python3 scripts/make/sync_imagelist_with_def.py build/imagelist.csv
 *   bash scripts/make/combine_images_named.sh build/imagelist.csv assets/images/combined
 */
'''


def main() -> int:
    if not SRC.is_file():
        sys.stderr.write(
            f"{SRC} not found. Run sync_imagelist_with_def.py and "
            f"combine_images_named.sh first; see docs/SETUP.md section 3.5.\n")
        return 1

    data = SRC.read_bytes()
    n = len(data)

    out = [HEADER, f"unsigned char ge_images_segment[{n}] = {{\n"]
    for i in range(0, n, PER_LINE):
        out.append(",".join(str(b) for b in data[i:i + PER_LINE]))
        out.append(",\n")
    out.append("};\n\nconst unsigned int ge_images_segment_size = %d;\n\n" % n)

    DST.write_text("".join(out))
    print(f"{DST}: {n} bytes from {SRC}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
