#!/usr/bin/env python3
"""Pin the RGBA16 byte-order defaults against big-endian assets and decoder output, ROM-free.

The renderer's RGBA16 read (thirdparty 0001-getv-port-layer.patch) and the game decoder's 16-bit
swap (0001-source.patch) must agree. This compiles both committed definitions, unchanged, into a
small program with two invented two-texel images:

- a big-endian byte stream, as static assets such as the Rareware logo arrays are declared; and
- a native u16 store on a little-endian host, as the game's texture decoder writes it.

With no GETV_* override set, both must decode to the same texels in the same order. Run it with
--patch-root pointing at an unchanged base checkout as a negative control.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATCH = Path("getv/patches/0001-source.patch")
RENDERER_PATCH = Path("getv/patches/thirdparty/0001-getv-port-layer.patch")
EXPECTED_CHECKS = 4

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef int32_t s32;
enum { TEXFORMAT_RGBA32, TEXFORMAT_RGB24, TEXFORMAT_RGBA16, TEXFORMAT_RGB15, TEXFORMAT_IA16 };

/* Production definitions from %(source_patch)s. */
%(decoder)s

/* Production definitions from %(renderer_patch)s. */
%(renderer)s

static struct { struct { const u8 *addr; } loaded_texture[1]; } rdp;

static uint16_t read_texel(const u8 *addr, uint32_t i)
{
    const int tile = 0;
    const int ge_r16 = ge_rgba16be_mode();
    rdp.loaded_texture[tile].addr = addr;
%(read)s
    return col16;
}

static int check(const char *source, const u8 *bytes, const uint16_t *expected, unsigned texels)
{
    int failures = 0;
    for (unsigned i = 0; i < texels; i++) {
        uint16_t got = read_texel(bytes, i);
        if (got != expected[i]) {
            printf("FAIL %%s texel %%u: expected 0x%%04x, got 0x%%04x\n", source, i, expected[i], got);
            failures++;
        } else {
            printf("PASS %%s texel %%u = 0x%%04x\n", source, i, got);
        }
    }
    return failures;
}

int main(void)
{
    /* Invented RGBA5551 texels: opaque red, then opaque green. */
    static const u8 big_endian_asset[] = {0xF8, 0x01, 0x07, 0xC1};
    const uint16_t expected[] = {0xF801, 0x07C1};
    const uint16_t native_store[] = {0xF801, 0x07C1};
    const uint16_t probe = 1;
    u8 decoded[sizeof(native_store)];
    int failures = 0;

    if (*(const u8 *) &probe != 1) {
        printf("FAIL host is not little-endian; the native-store case models the port's hosts\n");
        return 1;
    }
    if (getenv("GETV_RGBA16BE") != NULL || getenv("GETV_TEX16BE") != NULL) {
        printf("FAIL a byte-order override is set; this test checks the defaults\n");
        return 1;
    }
    failures += check("big-endian asset", big_endian_asset, expected, 2);
    memcpy(decoded, native_store, sizeof(decoded));
    geTexFixRgba16ByteOrder(decoded, (s32) sizeof(decoded), TEXFORMAT_RGBA16);
    failures += check("decoder native store", decoded, expected, 2);
    return failures != 0;
}
"""


def added_lines(path):
    """The lines a patch adds, without their leading '+'."""
    return [line[1:] for line in path.read_text(encoding="utf-8").splitlines()
            if line.startswith("+") and not line.startswith("+++")]


def function_through_brace(lines, first, name):
    """From a production line through the next column-zero closing brace after `name`."""
    try:
        start = lines.index(first)
        opener = next(i for i in range(start, len(lines)) if lines[i].startswith(name))
        end = lines.index("}", opener)
    except (ValueError, StopIteration):
        raise SystemExit(f"production definition not found: {name}")
    return lines[start:end + 1]


def renderer_read(lines):
    """The texel index mapping and big-endian read inside import_texture_rgba16."""
    first = "        uint32_t b0 = 2 * i, b1 = 2 * i + 1;"
    try:
        start = lines.index(first)
    except ValueError:
        raise SystemExit("import_texture_rgba16 texel read not found")
    block = lines[start:start + 4]
    if not block[-1].startswith("        uint16_t col16 = "):
        raise SystemExit("import_texture_rgba16 texel read changed shape; update this test")
    return block


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"),
                        help="C compiler executable (MinGW gcc.exe on Windows)")
    parser.add_argument("--patch-root", type=Path, default=ROOT,
                        help="repository whose patches are tested (an unchanged base for a negative control)")
    args = parser.parse_args()

    root = args.patch_root.resolve()
    source = added_lines(root / SOURCE_PATCH)
    renderer = added_lines(root / RENDERER_PATCH)
    program = HARNESS % {
        "source_patch": SOURCE_PATCH.as_posix(),
        "renderer_patch": RENDERER_PATCH.as_posix(),
        "decoder": "\n".join(function_through_brace(
            source, "int geTex16beFlag = -1;", "void geTexFixRgba16ByteOrder(")),
        "renderer": "\n".join(function_through_brace(
            renderer, "static int ge_rgba16be_mode(void) {", "static int ge_rgba16be_mode(void) {")),
        "read": "\n".join(renderer_read(renderer)),
    }

    environment = {k: v for k, v in os.environ.items() if k not in {"GETV_RGBA16BE", "GETV_TEX16BE"}}
    if os.name == "nt":
        # MinGW's compiler subprocesses and runtime DLLs must be discoverable.
        environment["PATH"] = str(Path(args.cc).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="ge-rgba16-") as temporary:
        scratch = Path(temporary)
        (scratch / "rgba16.c").write_text(program, encoding="utf-8")
        executable = scratch / ("rgba16.exe" if os.name == "nt" else "rgba16")
        subprocess.run([args.cc, "-std=gnu17", "-O1", "-Wall", "-Werror", "rgba16.c", "-o", executable.name],
                       cwd=scratch, env=environment, check=True)
        result = subprocess.run([str(executable)], cwd=scratch, env=environment,
                                capture_output=True, text=True)
    print(result.stdout, end="")
    checks = sum(line.startswith(("PASS ", "FAIL ")) for line in result.stdout.splitlines())
    if checks != EXPECTED_CHECKS:
        raise SystemExit(f"expected {EXPECTED_CHECKS} texel checks, ran {checks}")
    return result.returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
