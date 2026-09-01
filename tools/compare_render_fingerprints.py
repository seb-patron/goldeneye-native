#!/usr/bin/env python3
"""Compare native renderer BMPs with GoldenEye-Native's coarse render fingerprint."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from check_no_game_data import inspect_path
from render_refs import TOLERANCE, fingerprint


def candidate(value: str) -> tuple[str, Path]:
    if "=" in value:
        label, filename = value.split("=", 1)
    else:
        filename = value
        label = Path(filename).stem
    if not label or not filename:
        raise argparse.ArgumentTypeError("use LABEL=/absolute/path/to/capture.bmp")
    return label, Path(filename)


def comparison_rows(reference_path: Path, candidates: list[tuple[str, Path]]) -> tuple[int, list[dict[str, object]]]:
    failures = inspect_path(reference_path, allow_native_bmp=True)
    if failures:
        raise ValueError("; ".join(failures))
    reference = fingerprint(reference_path)
    rows: list[dict[str, object]] = []
    for label, path in candidates:
        failures = inspect_path(path, allow_native_bmp=True)
        if failures:
            raise ValueError("; ".join(failures))
        other = fingerprint(path)
        if len(other) != len(reference):
            raise ValueError(f"fingerprint length differs for {path.name}: {len(other)} != {len(reference)}")
        delta = [abs(a - b) for a, b in zip(reference, other)]
        rows.append({
            "comparison": label,
            "worst_channel_delta": max(delta),
            "mean_channel_delta": sum(delta) / len(delta),
            "channels_over_tolerance": sum(value > TOLERANCE for value in delta),
            "total_channels": len(delta),
        })
    return len(reference), rows


def render_text(reference: Path, channels: int, rows: list[dict[str, object]]) -> str:
    width = max(len("capture"), *(len(str(row["comparison"])) for row in rows))
    lines = [
        f"reference: {reference.name}",
        f"fingerprint: {channels} channels; tolerance: {TOLERANCE}",
        "",
        f"{'capture':<{width}}  {'worst':>5}  {'mean':>9}  {'over_tolerance':>14}",
    ]
    for row in rows:
        over = f"{row['channels_over_tolerance']}/{row['total_channels']}"
        lines.append(
            f"{str(row['comparison']):<{width}}  {int(row['worst_channel_delta']):>5}  "
            f"{float(row['mean_channel_delta']):>9.4f}  {over:>14}"
        )
    return "\n".join(lines)


def render_markdown(rows: list[dict[str, object]]) -> str:
    lines = [
        f"| Comparison with reference | Worst channel delta | Mean channel delta | Channels over tolerance {TOLERANCE} |",
        "| --- | ---: | ---: | ---: |",
    ]
    for row in rows:
        label = str(row["comparison"]).replace("|", "\\|")
        lines.append(
            f"| {label} | {row['worst_channel_delta']} | {float(row['mean_channel_delta']):.4f} | "
            f"{row['channels_over_tolerance']}/{row['total_channels']} |"
        )
    return "\n".join(lines)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True, help="reference native BMP")
    parser.add_argument("--format", choices=("text", "markdown", "json"), default="text")
    parser.add_argument("candidates", type=candidate, nargs="+", metavar="LABEL=CAPTURE.bmp")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        channels, rows = comparison_rows(args.reference.expanduser().resolve(), [
            (label, path.expanduser().resolve()) for label, path in args.candidates
        ])
    except (OSError, ValueError, IndexError, ZeroDivisionError) as exc:
        print(f"compare_render_fingerprints: {exc}", file=sys.stderr)
        return 1
    if args.format == "markdown":
        print(render_markdown(rows))
    elif args.format == "json":
        print(json.dumps({
            "reference": args.reference.name,
            "fingerprint_channels": channels,
            "tolerance": TOLERANCE,
            "comparisons": rows,
        }, indent=2))
    else:
        print(render_text(args.reference, channels, rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
