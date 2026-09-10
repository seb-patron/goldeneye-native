from __future__ import annotations

import json
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path
from unittest.mock import patch


TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import check_no_game_data as safety
import collect_bug_report as collector
import compare_render_fingerprints as comparison


def write_bmp(path: Path, rgb: tuple[int, int, int], width: int = 32, height: int = 24) -> None:
    stride = ((width * 3 + 3) // 4) * 4
    red, green, blue = rgb
    row = bytes((blue, green, red)) * width + b"\x00" * (stride - width * 3)
    pixels = row * height
    offset = 14 + 40
    header = (
        b"BM"
        + struct.pack("<IHHI", offset + len(pixels), 0, 0, offset)
        + struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    )
    path.write_bytes(header + pixels)


def hex_rows(count: int, per_line: int = 8, prefix: str = "") -> str:
    """Build synthetic hexadecimal data lines. The values are invented, not game data."""
    values = [f"0x{index % 251:02X}" for index in range(count)]
    return "".join(
        prefix + "    " + ", ".join(values[start:start + per_line]) + ",\n"
        for start in range(0, count, per_line)
    )


class PublicArtifactSafetyTests(unittest.TestCase):
    def test_staged_check_reads_index_instead_of_working_copy(self) -> None:
        # Synthetic content only. Keep temporary Git writes isolated even when run from a hook.
        environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, environment, clear=True):
            root = Path(directory)
            path = root / "probe.txt"
            path.write_bytes(b"invented test payload\x00")
            for arguments in (["init", "-q"], ["add", "probe.txt"]):
                subprocess.run(["git", *arguments], cwd=root, check=True, capture_output=True)
            path.write_text("safe working copy\n", encoding="utf-8")
            with patch.object(safety, "ROOT", root):
                self.assertEqual(safety.inspect_path(path), [])
                for removed in (False, True):
                    if removed:
                        path.unlink()
                    with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as errors:
                        self.assertEqual(safety.main(["--staged"]), 1)
                    self.assertIn("unexpected binary content", errors.getvalue())

    def test_detects_renamed_rom_and_archive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            renamed = root / "notes.dat"
            renamed.write_bytes(b"prefix" + b"\x80\x37\x12\x40" + b"payload")
            archive = root / "evidence.dat"
            archive.write_bytes(b"PK\x03\x04" + b"payload")
            self.assertTrue(any("N64 ROM header" in item for item in safety.inspect_path(renamed)))
            self.assertTrue(any("ZIP archive" in item for item in safety.inspect_path(archive)))

    def test_detects_encoded_payload(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "encoded.txt"
            path.write_bytes(b"A" * 5000)
            self.assertTrue(any("encoded binary payload" in item for item in safety.inspect_path(path)))

    def test_detects_dense_hex_arrays_in_source_and_patches(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            values = [f"0x{index % 251:02X}" for index in range(160)]
            source = root / "generated.c"
            source.write_text(
                "unsigned char invented[] = {\n    " + ", ".join(values) + "\n};\n",
                encoding="utf-8",
            )
            patch = root / "generated.patch"
            patch.write_text(
                "diff --git a/invented.c b/invented.c\n"
                "--- a/invented.c\n"
                "+++ b/invented.c\n"
                "@@ -1,3 +1,3 @@\n"
                "+unsigned char invented[] = {\n"
                "+    " + ", ".join(values) + "\n"
                "+};\n",
                encoding="utf-8",
            )
            for path in (source, patch):
                self.assertTrue(
                    any("high-density hexadecimal array" in item for item in safety.inspect_path(path))
                )

    def test_allows_ordinary_small_hex_constants(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "constants.c"
            path.write_text(
                "static const unsigned invented[] = {0x10, 0x20, 0x30, 0x40};\n",
                encoding="utf-8",
            )
            self.assertFalse(
                any("high-density hexadecimal array" in item for item in safety.inspect_path(path))
            )

    def test_detects_dense_hex_run_in_a_hunk_without_braces(self) -> None:
        """A patch hunk that starts partway through an initializer carries no braces."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "0002-invented.patch"
            path.write_text(
                "diff --git a/assets/invented.c b/assets/invented.c\n"
                "--- a/assets/invented.c\n"
                "+++ b/assets/invented.c\n"
                "@@ -100,20 +100,20 @@\n" + hex_rows(160, prefix="+"),
                encoding="utf-8",
            )
            self.assertTrue(
                any("high-density hexadecimal array" in item for item in safety.inspect_path(path))
            )

    def test_detects_dense_hex_run_in_nested_tables(self) -> None:
        """Each innermost brace is small, so only the run across lines sees the table."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "generated.c"
            rows = "\n".join(
                "    {" + ", ".join(f"0x{(row * 8 + column) % 251:02X}" for column in range(8)) + "},"
                for row in range(20)
            )
            path.write_text("unsigned char invented[20][8] = {\n" + rows + "\n};\n", encoding="utf-8")
            self.assertTrue(
                any("high-density hexadecimal array" in item for item in safety.inspect_path(path))
            )

    def test_dense_hex_run_threshold_boundary(self) -> None:
        """A short reviewed table stays clean; the same table past the threshold does not."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            below = root / "lookup.c"
            below.write_text(hex_rows(safety.HEX_ARRAY_MIN_LITERALS - 8), encoding="utf-8")
            at_threshold = root / "dumped.c"
            at_threshold.write_text(hex_rows(safety.HEX_ARRAY_MIN_LITERALS), encoding="utf-8")
            self.assertFalse(
                any("high-density hexadecimal array" in item for item in safety.inspect_path(below))
            )
            self.assertTrue(
                any(
                    "high-density hexadecimal array" in item
                    for item in safety.inspect_path(at_threshold)
                )
            )

    def test_detects_dense_hex_run_split_by_generated_row_comments(self) -> None:
        """Generated source annotates every row; the annotations must not hide the data."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "annotated.c"
            rows = "".join(
                f"    /* index = {row} */\n"
                + "    " + ", ".join(f"0x{(row * 8 + column) % 251:02X}" for column in range(8))
                + ",\n"
                for row in range(20)
            )
            path.write_text(rows, encoding="utf-8")
            self.assertTrue(
                any("high-density hexadecimal array" in item for item in safety.inspect_path(path))
            )

    def test_allows_scattered_constants_checksums_and_pins(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code = root / "flags.c"
            code.write_text(
                "".join(
                    f"#define INVENTED_FLAG_{index} 0x{1 << (index % 16):04X}  /* note {index} */\n"
                    for index in range(400)
                ),
                encoding="utf-8",
            )
            pins = root / "pins.sh"
            pins.write_text(
                "".join(
                    f"invented_{index}_sha256=\"{index:064x}\"\n" for index in range(400)
                ),
                encoding="utf-8",
            )
            for path in (code, pins):
                self.assertFalse(
                    any(
                        "high-density hexadecimal array" in item
                        for item in safety.inspect_path(path)
                    )
                )

    def test_dense_hex_run_does_not_span_two_files_in_one_patch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "0003-invented.patch"
            half = safety.HEX_ARRAY_MIN_LITERALS // 2
            section = (
                "diff --git a/getv/port/invented{0}.c b/getv/port/invented{0}.c\n"
                "--- a/getv/port/invented{0}.c\n"
                "+++ b/getv/port/invented{0}.c\n"
                "@@ -1,8 +1,8 @@\n"
            )
            path.write_text(
                section.format(1) + hex_rows(half, prefix="+") + "\n"
                + section.format(2) + hex_rows(half, prefix="+") + "\n",
                encoding="utf-8",
            )
            self.assertFalse(
                any("high-density hexadecimal array" in item for item in safety.inspect_path(path))
            )

    def test_dense_hex_allowance_is_exact_path_only(self) -> None:
        allowed = sorted(safety.ALLOWED_DENSE_HEX_ARRAY_PATHS)
        self.assertTrue(allowed, "the reviewed dense-array allowance must not be empty")
        for path in allowed:
            self.assertTrue(path.exists(), f"{path} is allowlisted but missing")
            self.assertFalse(
                any("high-density hexadecimal array" in item for item in safety.inspect_path(path))
            )
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory) / allowed[0].name
            copied.write_bytes(allowed[0].read_bytes())
            self.assertTrue(
                any("high-density hexadecimal array" in item for item in safety.inspect_path(copied))
            )


class BugReportCollectorTests(unittest.TestCase):
    def test_sanitizes_logs_and_normalizes_native_screenshot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "runtime.log"
            log.write_text(
                "user=/Users/alice project\n"
                "rom=/Users/alice/roms/goldeneye.z64\n"
                "token=ghp_abcdefghijklmnopqrstuvwxyz123456\n",
                encoding="utf-8",
            )
            screenshot = root / "capture.bmp"
            write_bmp(screenshot, (20, 40, 60))
            output = root / "bundle"
            args = collector.parse_args([
                "--kind", "rendering",
                "--renderer", "Metal",
                "--stage", "Complex (31)",
                "--log", str(log),
                "--screenshot", str(screenshot),
                "--output", str(output),
            ])
            output = output.resolve()
            self.assertEqual(collector.build_bundle(args), output)
            sanitized = (output / "runtime-1.log").read_text(encoding="utf-8")
            self.assertNotIn("/Users/alice", sanitized)
            self.assertNotIn("goldeneye.z64", sanitized)
            self.assertNotIn("ghp_", sanitized)
            self.assertTrue((output / "screenshot-1.png").read_bytes().startswith(b"\x89PNG"))
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertTrue(manifest["safety"]["prohibited_artifact_checks_passed"])
            self.assertTrue(manifest["safety"]["manual_review_required"])
            self.assertEqual(len(manifest["artifacts"]), 2)

    @unittest.expectedFailure  # Issue #85: flat colours made of base64 characters look like payloads.
    def test_accepts_flat_colour_native_screenshots(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            screenshot = Path(directory) / "grey.bmp"
            write_bmp(screenshot, (100, 100, 100), width=320, height=240)
            collector.native_bmp_to_png(screenshot, Path(directory) / "grey.png")
            comparison.comparison_rows(screenshot, [("same", screenshot)])

    def test_rejects_output_inside_repository(self) -> None:
        args = collector.parse_args([
            "--kind", "build",
            "--output", str(collector.ROOT / "forbidden-report-output"),
        ])
        with self.assertRaisesRegex(ValueError, "outside the repository"):
            collector.build_bundle(args)


class IsolatedInterpreterTests(unittest.TestCase):
    def test_tools_import_siblings_without_script_directory_on_sys_path(self) -> None:
        # -I omits the script directory exactly as the Windows setup's embeddable Python does.
        for name in ("collect_bug_report.py", "compare_render_fingerprints.py"):
            with self.subTest(tool=name):
                process = subprocess.run([sys.executable, "-I", str(TOOLS / name), "--help"],
                                         capture_output=True, text=True)
                self.assertEqual(process.returncode, 0, process.stderr)


class FingerprintComparisonTests(unittest.TestCase):
    def test_markdown_and_json_data(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root / "reference.bmp"
            same = root / "same.bmp"
            changed = root / "changed.bmp"
            write_bmp(reference, (10, 20, 30))
            write_bmp(same, (10, 20, 30))
            write_bmp(changed, (40, 20, 30))
            channels, rows = comparison.comparison_rows(reference, [
                ("Same", same), ("Changed", changed)
            ])
            self.assertEqual(channels, 144)
            self.assertEqual(rows[0]["worst_channel_delta"], 0)
            self.assertEqual(rows[1]["worst_channel_delta"], 30)
            markdown = comparison.render_markdown(rows)
            self.assertIn("| Same | 0 | 0.0000 | 0/144 |", markdown)
            self.assertIn("| Changed | 30 |", markdown)
            output = io.StringIO()
            with redirect_stdout(output):
                result = comparison.main([
                    "--format", "markdown",
                    "--reference", str(reference),
                    f"Same={same}",
                    f"Changed={changed}",
                ])
            self.assertEqual(result, 0)
            self.assertEqual(output.getvalue().strip(), markdown)


if __name__ == "__main__":
    unittest.main()
