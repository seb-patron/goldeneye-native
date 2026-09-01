#!/usr/bin/env python3
"""Create a local, sanitized GoldenEye-Native bug-report bundle without publishing it."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from datetime import datetime, timezone
from pathlib import Path

from check_no_game_data import ROOT, inspect_path


MAX_TEXT_BYTES = 8 * 1024 * 1024
TOKEN_PATTERNS = [
    re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,})\b"),
    re.compile(r"\bsk-[A-Za-z0-9_-]{20,}\b"),
]
SECRET_ASSIGNMENT = re.compile(
    r"(?i)\b(password|passwd|token|secret|api[_-]?key|authorization)\s*([:=])\s*([^\s]+)"
)
ROM_PATH = re.compile(
    r"(?i)(?:[A-Za-z]:[\\/]|/|~/)[^\r\n\"']*?\.(?:z64|n64|v64|rom)(?=\s|$|[\"'])"
)
PRIVATE_FILE_PATH = re.compile(
    r"(?i)(?:[A-Za-z]:[\\/]|/|~/)[^\r\n\"']*?(?:base\.zip|eeprom\.bin)(?=\s|$|[\"'])"
)


def sanitize_text(text: str) -> str:
    for pattern in TOKEN_PATTERNS:
        text = pattern.sub("<REDACTED_TOKEN>", text)
    text = SECRET_ASSIGNMENT.sub(lambda m: f"{m.group(1)}{m.group(2)}<REDACTED>", text)
    text = ROM_PATH.sub("<ROM_PATH_REDACTED>", text)
    text = PRIVATE_FILE_PATH.sub("<PRIVATE_GAME_DATA_PATH_REDACTED>", text)

    home = str(Path.home())
    if home and home != "/":
        text = text.replace(home, "~")
    text = re.sub(r"/Users/[^/\s]+", "<HOME>", text)
    text = re.sub(r"/home/[^/\s]+", "<HOME>", text)
    text = re.sub(r"[A-Za-z]:\\Users\\[^\\\s]+", "<HOME>", text)
    return text


def _run(command: list[str]) -> str:
    try:
        result = subprocess.run(
            command, cwd=ROOT, capture_output=True, text=True, timeout=15, check=False
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"
    if result.returncode != 0:
        return "unavailable"
    return sanitize_text(result.stdout.strip())


def repository_metadata() -> dict[str, object]:
    status = _run(["git", "status", "--short"])
    changes = [] if not status or status == "unavailable" else status.splitlines()
    working_tree = (
        "unavailable" if status == "unavailable"
        else "clean" if not changes
        else f"dirty ({len(changes)} path(s))"
    )
    return {
        "commit": _run(["git", "rev-parse", "HEAD"]),
        "short_commit": _run(["git", "rev-parse", "--short", "HEAD"]),
        "branch": _run(["git", "branch", "--show-current"]),
        "working_tree": working_tree,
        "working_tree_changes": changes,
    }


def system_metadata() -> dict[str, str]:
    values = {
        "platform": sanitize_text(platform.platform()),
        "architecture": sanitize_text(platform.machine()),
        "python": sanitize_text(platform.python_version()),
        "compiler": (_run(["clang", "--version"]) or "unavailable").splitlines()[0],
    }
    if platform.system() == "Darwin":
        values["macOS"] = _run(["sw_vers", "-productVersion"])
        values["model"] = _run(["sysctl", "-n", "hw.model"])
        values["SDK"] = _run(["xcrun", "--sdk", "macosx", "--show-sdk-version"])
    return values


def runtime_environment() -> dict[str, str]:
    result: dict[str, str] = {}
    for key, value in sorted(os.environ.items()):
        if not key.startswith("GETV_"):
            continue
        if any(word in key.upper() for word in ("PASSWORD", "TOKEN", "SECRET", "AUTH", "KEY")):
            result[key] = "<REDACTED>"
        else:
            result[key] = sanitize_text(value)
    return result


def read_sanitized_text(path: Path) -> str:
    failures = inspect_path(path)
    if failures:
        raise ValueError("; ".join(failures))
    if path.stat().st_size > MAX_TEXT_BYTES:
        raise ValueError(f"{path.name}: text artifact exceeds {MAX_TEXT_BYTES // (1024 * 1024)} MiB")
    data = path.read_bytes()
    if b"\x00" in data:
        raise ValueError(f"{path.name}: binary data cannot be included as a log")
    text = data.decode("utf-8", errors="replace")
    if any(len(line) > 16384 for line in text.splitlines()):
        raise ValueError(f"{path.name}: suspiciously long encoded or binary-looking line")
    return sanitize_text(text)


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", binascii.crc32(kind + payload) & 0xFFFFFFFF)
    )


def native_bmp_to_png(source: Path, destination: Path) -> None:
    failures = inspect_path(source, allow_native_bmp=True)
    if failures:
        raise ValueError("; ".join(failures))
    if source.is_symlink():
        raise ValueError(f"{source.name}: screenshot symlinks are not accepted")
    if source.stat().st_size > 64 * 1024 * 1024:
        raise ValueError(f"{source.name}: screenshot is unexpectedly large")

    data = source.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError(f"{source.name}: only native BMP captures are accepted")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    planes, bpp = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if dib_size < 40 or width <= 0 or signed_height == 0:
        raise ValueError(f"{source.name}: unsupported BMP header")
    if planes != 1 or bpp != 24 or compression != 0:
        raise ValueError(f"{source.name}: expected an uncompressed 24-bit native BMP")

    height = abs(signed_height)
    if width * height > 50_000_000:
        raise ValueError(f"{source.name}: screenshot dimensions are unexpectedly large")
    stride = ((width * 3 + 3) // 4) * 4
    if pixel_offset + stride * height > len(data):
        raise ValueError(f"{source.name}: truncated BMP pixel data")

    rows = bytearray()
    for output_y in range(height):
        source_y = height - 1 - output_y if signed_height > 0 else output_y
        row = data[pixel_offset + source_y * stride : pixel_offset + source_y * stride + width * 3]
        rows.append(0)
        for offset in range(0, len(row), 3):
            blue, green, red = row[offset : offset + 3]
            rows.extend((red, green, blue))

    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png.extend(_png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
    png.extend(_png_chunk(b"IDAT", zlib.compress(bytes(rows), level=9)))
    png.extend(_png_chunk(b"IEND", b""))
    destination.write_bytes(png)


def _outside_repository(path: Path) -> bool:
    try:
        path.resolve().relative_to(ROOT.resolve())
        return False
    except ValueError:
        return True


def _artifact_record(path: Path, kind: str) -> dict[str, object]:
    data = path.read_bytes()
    return {
        "file": path.name,
        "kind": kind,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("gameplay", "configuration", "rendering", "build", "crash"), required=True)
    parser.add_argument("--renderer", default="Not renderer-specific or unknown")
    parser.add_argument("--stage", default="Not provided")
    parser.add_argument("--actual", default="Not provided — complete before filing.")
    parser.add_argument("--expected", default="Not provided — complete before filing.")
    parser.add_argument("--reproduction", default="Not provided — complete before filing.")
    parser.add_argument("--frequency", default="Not provided")
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--log", action="append", type=Path, default=[])
    parser.add_argument("--crash-report", action="append", type=Path, default=[])
    parser.add_argument("--screenshot", action="append", type=Path, default=[])
    parser.add_argument("--output", type=Path, help="new output directory outside the repository")
    return parser.parse_args(argv)


def build_bundle(args: argparse.Namespace) -> Path:
    if args.output:
        output = args.output.expanduser().resolve()
        if not _outside_repository(output):
            raise ValueError("the report bundle must be outside the repository")
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="goldeneye-native-report-"))

    artifacts: list[dict[str, object]] = []
    try:
        for index, source in enumerate(args.log, 1):
            destination = output / f"runtime-{index}.log"
            destination.write_text(read_sanitized_text(source.expanduser().resolve()), encoding="utf-8")
            artifacts.append(_artifact_record(destination, "sanitized runtime/build log"))
        for index, source in enumerate(args.crash_report, 1):
            destination = output / f"crash-{index}.ips"
            destination.write_text(read_sanitized_text(source.expanduser().resolve()), encoding="utf-8")
            artifacts.append(_artifact_record(destination, "sanitized crash report"))
        for index, source in enumerate(args.screenshot, 1):
            destination = output / f"screenshot-{index}.png"
            native_bmp_to_png(source.expanduser().resolve(), destination)
            artifacts.append(_artifact_record(destination, "metadata-free runtime screenshot"))

        repo = repository_metadata()
        system = system_metadata()
        environment = runtime_environment()
        manifest = {
            "created_utc": datetime.now(timezone.utc).isoformat(),
            "publication_status": "local draft; not uploaded",
            "safety": {
                "prohibited_artifact_checks_passed": True,
                "manual_review_required": True,
            },
            "repository": repo,
            "system": system,
            "getv_environment": environment,
            "artifacts": artifacts,
        }
        (output / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

        env_lines = [f"{key}={value}" for key, value in environment.items()] or ["No GETV_* values inherited by collector."]
        command_lines = [sanitize_text(value) for value in args.command] or ["Not provided — complete before filing."]
        artifact_lines = [f"- `{item['file']}` — {item['kind']}" for item in artifacts] or ["- No files staged."]
        report = f"""# GoldenEye-Native {args.kind} bug report draft

> Local draft only. Nothing in this directory has been uploaded. Inspect every file before publication.
>
> **Never include a ROM, ever.** Automated prohibited-artifact checks passed for the selected
> inputs; this does not replace manual review for a ROM, save, `base.zip` or extracted game data.

## Commit and system

- Commit: `{repo['commit']}`
- Branch: `{repo['branch']}`
- Working tree: `{repo['working_tree']}`
- Platform: `{system.get('platform', 'unavailable')}`
- Architecture: `{system.get('architecture', 'unavailable')}`
- Renderer: {sanitize_text(args.renderer)}
- Level or screen: {sanitize_text(args.stage)}
- Frequency: {sanitize_text(args.frequency)}

## What happened

{sanitize_text(args.actual)}

## What should happen instead

{sanitize_text(args.expected)}

## Exact reproduction

{sanitize_text(args.reproduction)}

## Commands

```text
{chr(10).join(command_lines)}
```

## Relevant GETV environment

```text
{chr(10).join(env_lines)}
```

## Reviewed attachments

{chr(10).join(artifact_lines)}

## Report preparation

Agent-assisted evidence collection. A human must review the complete draft and every attachment before filing.
"""
        (output / "report.md").write_text(report, encoding="utf-8")
    except Exception:
        shutil.rmtree(output, ignore_errors=True)
        raise
    return output


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        output = build_bundle(args)
    except (OSError, ValueError) as exc:
        print(f"collect_bug_report: {exc}", file=sys.stderr)
        return 1
    print(output)
    print("local draft created; inspect report.md, manifest.json and every artifact before upload")
    print("never include or upload a ROM")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
