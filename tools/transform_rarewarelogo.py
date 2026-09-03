#!/usr/bin/env python3
"""Convert the locally extracted Rareware logo arrays to byte streams."""

from __future__ import annotations

import os
import re
import stat
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ASSET_DIRECTORY = ROOT / "vendor" / "ge-decomp" / "assets"
TARGET_NAME = "rarewarelogo.c"
EXPECTED_ARRAYS = (
    "imgRAre_0x0020",
    "img_raRE_0x0AE0",
    "imgWAre_0x15A0",
    "imgwaRE_0x2060",
    "D_02004FE8",
    "D_02005FF0",
)
DECLARATION = re.compile(
    r"(?m)^(?P<indent>[ \t]*)(?P<type>u8|u32)[ \t]+"
    r"(?P<name>" + "|".join(map(re.escape, EXPECTED_ARRAYS)) + r")"
    r"[ \t]*\[[ \t]*\][ \t]*=[ \t]*\{"
)
ANY_EXPECTED_NAME = re.compile(
    r"\b(" + "|".join(map(re.escape, EXPECTED_ARRAYS)) + r")\b"
)


class TransformError(ValueError):
    """The local generated file is absent, unsafe, or structurally unexpected."""


def _validated_target(target: Path, asset_directory: Path) -> Path:
    asset_directory = asset_directory.absolute()
    target = target.absolute()
    if target != asset_directory / TARGET_NAME:
        raise TransformError(f"target must be the expected assets/{TARGET_NAME} path")
    if target.is_symlink():
        raise TransformError("refusing to replace a symlinked generated asset")
    try:
        resolved_directory = asset_directory.resolve(strict=True)
        resolved_target = target.resolve(strict=True)
    except FileNotFoundError as exc:
        raise TransformError(
            "the locally extracted assets are missing; run extraction first"
        ) from exc
    repository_asset_directory = ROOT.resolve() / "vendor" / "ge-decomp" / "assets"
    if asset_directory == ASSET_DIRECTORY.absolute() and resolved_directory != repository_asset_directory:
        raise TransformError("repository asset directory must not traverse a symlink")
    if resolved_target.parent != resolved_directory:
        raise TransformError("generated asset resolves outside the expected asset directory")
    target_stat = target.stat()
    if not stat.S_ISREG(target_stat.st_mode):
        raise TransformError("generated asset is not a regular file")
    if target_stat.st_nlink != 1:
        raise TransformError("refusing to replace a multiply linked generated asset")
    return target


def _initializer_values(body: str, digits: int, name: str) -> list[str]:
    token = rf"0[xX][0-9A-Fa-f]{{{digits}}}"
    if not re.fullmatch(rf"\s*{token}(?:\s*,\s*{token})*\s*,?\s*", body):
        raise TransformError(f"{name} has an unexpected or ambiguous initializer")
    return re.findall(token, body)


def _locate_arrays(text: str) -> list[tuple[re.Match[str], int]]:
    matches = list(DECLARATION.finditer(text))
    name_counts = {name: 0 for name in EXPECTED_ARRAYS}
    located: list[tuple[re.Match[str], int]] = []
    for match in matches:
        name = match.group("name")
        name_counts[name] += 1
        close = text.find("};", match.end())
        if close < 0:
            raise TransformError(f"{name} has no unambiguous initializer terminator")
        located.append((match, close))

    missing = [name for name, count in name_counts.items() if count != 1]
    if missing or len(matches) != len(EXPECTED_ARRAYS):
        raise TransformError("expected each Rareware logo array exactly once")
    return located


def transform_text(text: str) -> tuple[str, bool]:
    """Return validated transformed source and whether it changed."""
    if not ANY_EXPECTED_NAME.search(text):
        raise TransformError("expected Rareware logo arrays were not found")
    located = _locate_arrays(text)
    types = {match.group("type") for match, _ in located}
    if types == {"u8"}:
        for match, close in located:
            _initializer_values(text[match.end():close], 2, match.group("name"))
        return text, False
    if types != {"u32"}:
        raise TransformError("refusing a partially transformed generated asset")

    newline = "\r\n" if "\r\n" in text else "\n"
    replacements: list[tuple[int, int, str]] = []
    for match, close in located:
        words = _initializer_values(text[match.end():close], 8, match.group("name"))
        byte_values: list[int] = []
        for word in words:
            value = int(word, 16)
            byte_values.extend((value >> shift) & 0xFF for shift in (24, 16, 8, 0))

        indent = match.group("indent")
        value_indent = indent + "    "
        lines = []
        for offset in range(0, len(byte_values), 16):
            chunk = byte_values[offset:offset + 16]
            suffix = "," if offset + 16 < len(byte_values) else ""
            lines.append(
                value_indent + ", ".join(f"0x{value:02X}" for value in chunk) + suffix
            )
        replacement = (
            f"{indent}u8 {match.group('name')}[] = {{{newline}"
            + newline.join(lines)
            + f"{newline}{indent}}}"
        )
        replacements.append((match.start(), close + 1, replacement))

    transformed = text
    for start, end, replacement in reversed(replacements):
        transformed = transformed[:start] + replacement + transformed[end:]
    return transformed, True


def _atomic_replace(target: Path, content: bytes, mode: int) -> None:
    temporary: Path | None = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{TARGET_NAME}.", suffix=".tmp", dir=target.parent
        )
        temporary = Path(temporary_name)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, stat.S_IMODE(mode))
        os.replace(temporary, target)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def transform_file(target: Path, asset_directory: Path) -> bool:
    """Validate and atomically transform the one expected generated asset file."""
    target = _validated_target(target, asset_directory)
    original = target.read_bytes()
    try:
        text = original.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise TransformError("generated asset is not UTF-8 text") from exc
    transformed, changed = transform_text(text)
    if not changed:
        return False
    mode = target.stat().st_mode
    _atomic_replace(target, transformed.encode("utf-8"), mode)
    return True


def main() -> int:
    target = ASSET_DIRECTORY / TARGET_NAME
    try:
        changed = transform_file(target, ASSET_DIRECTORY)
    except (OSError, TransformError) as exc:
        print(f"Rareware logo transform failed: {exc}", file=os.sys.stderr)
        return 1
    state = "converted" if changed else "already converted"
    print(f"Rareware logo arrays: {state} locally")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
