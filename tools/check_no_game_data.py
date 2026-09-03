#!/usr/bin/env python3
"""Reject ROMs, saves and other non-public game artifacts before publication."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SCAN_BYTES = 8 * 1024 * 1024

ROM_MAGICS = {
    b"\x80\x37\x12\x40": "big-endian N64 ROM header",
    b"\x37\x80\x40\x12": "byte-swapped N64 ROM header",
    b"\x40\x12\x37\x80": "little-endian N64 ROM header",
}
ARCHIVE_MAGICS = {
    b"PK\x03\x04": "ZIP archive",
    b"7z\xbc\xaf\x27\x1c": "7-Zip archive",
    b"Rar!\x1a\x07": "RAR archive",
    b"\x1f\x8b": "gzip archive",
}
FORBIDDEN_SUFFIXES = {
    ".z64", ".n64", ".v64", ".rom", ".sav", ".save", ".srm", ".eep",
    ".bin", ".iso", ".img", ".elf",
}
FORBIDDEN_NAMES = {
    "base.zip", "baserom", "baserom.z64", "eeprom.bin", "save.dat",
}
ALLOWED_BINARY_SUFFIXES = {
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".icns", ".ttf", ".otf",
    ".woff", ".woff2", ".pdf", ".ogg", ".wav",
}
BASE64_PAYLOAD = re.compile(rb"(?:[A-Za-z0-9+/]{4096,}={0,2})")
HEX_LITERAL = re.compile(r"\b0[xX][0-9A-Fa-f]{2,16}(?:[uUlL]*)\b")
BRACED_TEXT = re.compile(r"\{([^{}]*)\}", re.DOTALL)
HEX_ARRAY_MIN_LITERALS = 128
HEX_ARRAY_MIN_DENSITY = 0.65
# This port-owned launcher icon is already reviewed and intentionally stored as a C header.
# The exception is exact-path only; renamed or copied dense arrays are still rejected.
ALLOWED_DENSE_HEX_ARRAY_PATHS = {
    ROOT / "getv" / "port" / "src" / "ge_icon.h",
}


def _display(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT))
    except (OSError, ValueError):
        return path.name


def _has_dense_hex_array(data: bytes) -> bool:
    """Identify large literal arrays without retaining or reporting their values."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return False
    for match in BRACED_TEXT.finditer(text):
        body = match.group(1)
        literals = list(HEX_LITERAL.finditer(body))
        if len(literals) < HEX_ARRAY_MIN_LITERALS:
            continue
        non_whitespace = sum(not char.isspace() for char in body)
        if non_whitespace == 0:
            continue
        literal_characters = sum(len(item.group(0)) for item in literals)
        if literal_characters / non_whitespace >= HEX_ARRAY_MIN_DENSITY:
            return True
    return False


def inspect_path(path: Path, *, allow_native_bmp: bool = False) -> list[str]:
    """Return publication-safety failures without following symlinks."""
    path = path if path.is_absolute() else ROOT / path
    failures: list[str] = []
    display = _display(path)

    if path.is_symlink():
        target = os.readlink(path)
        lower_target = target.lower()
        if any(lower_target.endswith(suffix) for suffix in FORBIDDEN_SUFFIXES):
            failures.append(f"{display}: symlink targets a forbidden game-data file")
        return failures
    if not path.exists():
        return [f"{display}: file does not exist"]
    if not path.is_file():
        return [f"{display}: not a regular file"]

    lower_name = path.name.lower()
    lower_parts = {part.lower() for part in path.parts}
    suffix = path.suffix.lower()
    if lower_name in FORBIDDEN_NAMES:
        failures.append(f"{display}: forbidden game-data filename")
    if suffix in FORBIDDEN_SUFFIXES:
        failures.append(f"{display}: forbidden game-data or compiled-output extension")
    if "roms" in lower_parts:
        failures.append(f"{display}: files from a ROM directory may never be published")
    if suffix == ".bmp" and not allow_native_bmp:
        failures.append(f"{display}: rendered BMP captures must not be committed")

    with path.open("rb") as fh:
        data = fh.read(SCAN_BYTES)

    for magic, description in ROM_MAGICS.items():
        if magic in data:
            failures.append(f"{display}: contains a {description}, even if renamed")
    for magic, description in ARCHIVE_MAGICS.items():
        at_archive_header = data.startswith(magic)
        prefixed_archive = len(magic) >= 4 and magic in data[:4096]
        if at_archive_header or prefixed_archive:
            failures.append(f"{display}: {description} files are not accepted as public artifacts")

    binary_allowed = suffix in ALLOWED_BINARY_SUFFIXES or (allow_native_bmp and suffix == ".bmp")
    if b"\x00" in data and not binary_allowed:
        failures.append(f"{display}: unexpected binary content")
    if BASE64_PAYLOAD.search(data):
        failures.append(f"{display}: contains a suspicious encoded binary payload")
    try:
        allowed_dense_array = path.resolve() in {
            allowed.resolve() for allowed in ALLOWED_DENSE_HEX_ARRAY_PATHS
        }
    except OSError:
        allowed_dense_array = False
    if not allowed_dense_array and _has_dense_hex_array(data):
        failures.append(f"{display}: contains a suspicious high-density hexadecimal array")
    return failures


def _git_paths(args: list[str]) -> set[Path]:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, check=True, capture_output=True
    )
    return {
        ROOT / os.fsdecode(value)
        for value in result.stdout.split(b"\0")
        if value
    }


def changed_paths(base: str) -> set[Path]:
    paths = _git_paths(["diff", "--name-only", "--diff-filter=ACMR", "-z", f"{base}...HEAD"])
    paths |= _git_paths(["diff", "--name-only", "--diff-filter=ACMR", "-z"])
    paths |= _git_paths(["diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"])
    paths |= _git_paths(["ls-files", "--others", "--exclude-standard", "-z"])
    return paths


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--changed", metavar="BASE", help="check files changed from BASE plus local changes")
    mode.add_argument("--staged", action="store_true", help="check files staged for commit")
    mode.add_argument("--tracked", action="store_true", help="check every tracked file")
    parser.add_argument("paths", nargs="*", type=Path, help="specific files to check")
    args = parser.parse_args(argv)
    if not (args.changed or args.staged or args.tracked or args.paths):
        parser.error("choose --changed, --staged, --tracked, or provide paths")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    paths = {path if path.is_absolute() else ROOT / path for path in args.paths}
    try:
        if args.changed:
            paths |= changed_paths(args.changed)
        if args.staged:
            paths |= _git_paths(["diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"])
        if args.tracked:
            paths |= _git_paths(["ls-files", "-z"])
    except subprocess.CalledProcessError as exc:
        print(exc.stderr.decode(errors="replace"), file=sys.stderr)
        return 2

    failures: list[str] = []
    checked = 0
    for path in sorted(paths, key=str):
        if not path.exists() and not path.is_symlink():
            continue
        checked += 1
        failures.extend(inspect_path(path))

    if failures:
        print("PUBLIC ARTIFACT CHECK FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("Never include or upload a ROM. Remove every prohibited artifact before continuing.", file=sys.stderr)
        return 1

    print(f"public artifact check: {checked} file(s) passed; no prohibited game data detected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
