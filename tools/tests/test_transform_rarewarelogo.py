from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import transform_rarewarelogo as logo


INVENTED_WORDS = ("0x01020304", "0xA1B2C3D4", "0x10203040", "0x55667788")


def generated_source(*, malformed: bool = False, type_name: str = "u32") -> str:
    arrays = []
    for index, name in enumerate(logo.EXPECTED_ARRAYS):
        values = list(INVENTED_WORDS)
        if malformed and index == 2:
            values.insert(1, "not-a-word")
        arrays.append(f"{type_name} {name}[] = {{\n    {', '.join(values)}\n}};\n")
    return "#include <ultra64.h>\n\n" + "\n".join(arrays)


class RarewareLogoTransformTests(unittest.TestCase):
    def make_asset(self, root: Path, content: str) -> tuple[Path, Path]:
        assets = root / "assets"
        assets.mkdir()
        target = assets / logo.TARGET_NAME
        target.write_text(content, encoding="utf-8")
        return assets, target

    def test_converts_words_most_significant_byte_first(self) -> None:
        transformed, changed = logo.transform_text(generated_source())
        self.assertTrue(changed)
        self.assertIn(
            "u8 imgRAre_0x0020[] = {\n"
            "    0x01, 0x02, 0x03, 0x04, 0xA1, 0xB2, 0xC3, 0xD4, "
            "0x10, 0x20, 0x30, 0x40, 0x55, 0x66, 0x77, 0x88\n"
            "};",
            transformed,
        )

    def test_formatting_and_second_run_are_stable(self) -> None:
        first, changed = logo.transform_text(generated_source())
        self.assertTrue(changed)
        second, changed_again = logo.transform_text(first)
        self.assertFalse(changed_again)
        self.assertEqual(second, first)
        self.assertEqual(first.count("u8 "), len(logo.EXPECTED_ARRAYS))
        self.assertNotIn("u32 ", first)

    def test_rejects_malformed_or_partial_input(self) -> None:
        with self.assertRaisesRegex(logo.TransformError, "unexpected or ambiguous"):
            logo.transform_text(generated_source(malformed=True))
        partial = generated_source().replace("u32 imgRAre", "u8 imgRAre", 1)
        with self.assertRaisesRegex(logo.TransformError, "partially transformed"):
            logo.transform_text(partial)

    def test_rejects_unexpected_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            unexpected = root / logo.TARGET_NAME
            unexpected.write_text(generated_source(), encoding="utf-8")
            with self.assertRaisesRegex(logo.TransformError, "expected assets"):
                logo.transform_file(unexpected, assets)

    def test_validation_failure_does_not_modify_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            assets, target = self.make_asset(Path(directory), generated_source(malformed=True))
            before = target.read_bytes()
            with self.assertRaises(logo.TransformError):
                logo.transform_file(target, assets)
            self.assertEqual(target.read_bytes(), before)

    def test_replace_failure_leaves_original_and_no_temporary_copy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            assets, target = self.make_asset(Path(directory), generated_source())
            before = target.read_bytes()
            with mock.patch.object(logo.os, "replace", side_effect=OSError("invented failure")):
                with self.assertRaisesRegex(OSError, "invented failure"):
                    logo.transform_file(target, assets)
            self.assertEqual(target.read_bytes(), before)
            self.assertEqual(sorted(path.name for path in assets.iterdir()), [logo.TARGET_NAME])


if __name__ == "__main__":
    unittest.main()
