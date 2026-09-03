from __future__ import annotations

import json
import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path


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


class PublicArtifactSafetyTests(unittest.TestCase):
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

    def test_rejects_output_inside_repository(self) -> None:
        args = collector.parse_args([
            "--kind", "build",
            "--output", str(collector.ROOT / "forbidden-report-output"),
        ])
        with self.assertRaisesRegex(ValueError, "outside the repository"):
            collector.build_bundle(args)


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
