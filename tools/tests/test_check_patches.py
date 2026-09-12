"""ROM-free regression tests for tools/check_patches.sh clone handling.

A complete vendor/ge-decomp should use the fast local-clone path.

A partial/promisor vendor/ge-decomp may be missing promised blobs. In that
case the checker must clone the real upstream and detach at the exact vendor
HEAD rather than trying to clone the incomplete repository locally.

All repositories and source files in this test are synthetic.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools/check_patches.sh"
CANONICAL_UPSTREAM = "https://github.com/n64decomp/007"


def git(cwd: Path, *args: str, env=None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        cwd=cwd,
        env=env,
        check=True,
        capture_output=True,
        text=True,
    )


class CheckPatchesCloneTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)

    def make_fixture(self, partial_vendor: bool):
        root = self.base / "project"
        upstream = self.base / "upstream"
        home = self.base / "home"

        (root / "tools").mkdir(parents=True)
        (root / "getv/patches").mkdir(parents=True)
        (root / "vendor").mkdir(parents=True)
        home.mkdir()

        shutil.copy2(CHECKER, root / "tools/check_patches.sh")

        for name in ("setup.sh", "setup-mac.sh", "setup-windows.sh"):
            (root / "tools" / name).write_text(
                "# synthetic setup fixture\n"
                "# applies 0001-test.patch\n",
                encoding="utf-8",
            )

        (root / "getv/patches/0001-test.patch").write_text(
            "diff --git a/probe.txt b/probe.txt\n"
            "--- a/probe.txt\n"
            "+++ b/probe.txt\n"
            "@@ -1 +1 @@\n"
            "-base\n"
            "+patched\n",
            encoding="utf-8",
        )

        git(self.base, "init", "-q", "-b", "main", str(upstream))
        git(upstream, "config", "user.name", "Synthetic Test")
        git(upstream, "config", "user.email", "test@example.invalid")
        git(upstream, "config", "uploadpack.allowFilter", "true")

        (upstream / "probe.txt").write_text("base\n", encoding="utf-8")
        git(upstream, "add", "probe.txt")
        git(upstream, "commit", "-q", "-m", "base")
        base_commit = git(upstream, "rev-parse", "HEAD").stdout.strip()

        # Make upstream one commit newer. The test patch only applies to the
        # older commit. This proves that the fallback preserves vendor HEAD
        # rather than accidentally testing current upstream main.
        (upstream / "probe.txt").write_text("newer\n", encoding="utf-8")
        git(upstream, "commit", "-qam", "newer upstream")
        newer_commit = git(upstream, "rev-parse", "HEAD").stdout.strip()
        self.assertNotEqual(base_commit, newer_commit)

        vendor = root / "vendor/ge-decomp"
        upstream_url = upstream.resolve().as_uri()

        clone_env = os.environ.copy()
        clone_env["GIT_ALLOW_PROTOCOL"] = "file"

        if partial_vendor:
            subprocess.run(
                [
                    "git",
                    "clone",
                    "-q",
                    "--filter=blob:none",
                    "--no-checkout",
                    upstream_url,
                    str(vendor),
                ],
                env=clone_env,
                check=True,
                capture_output=True,
                text=True,
            )

            # Keep HEAD symbolic to main, but move main back to the baseline
            # without checking anything out. Missing blobs stay missing.
            git(vendor, "update-ref", "refs/heads/main", base_commit)

            self.assertEqual(
                git(
                    vendor,
                    "config",
                    "--get",
                    "remote.origin.promisor",
                ).stdout.strip(),
                "true",
            )

            missing = git(
                vendor,
                "rev-list",
                "--objects",
                "HEAD",
                "--missing=print",
            ).stdout.splitlines()

            self.assertTrue(
                any(line.startswith("?") for line in missing),
                "partial-clone fixture must contain a missing promised object",
            )
        else:
            subprocess.run(
                ["git", "clone", "-q", upstream_url, str(vendor)],
                env=clone_env,
                check=True,
                capture_output=True,
                text=True,
            )

            # Make the complete vendor repository represent the older
            # baseline while remaining a normal checked-out branch.
            git(vendor, "reset", "--hard", base_commit)

        # Redirect the checker's hard-coded GitHub upstream to our synthetic
        # complete repository. The regression therefore requires no network.
        subprocess.run(
            [
                "git",
                "config",
                "--file",
                str(home / ".gitconfig"),
                f"url.{upstream_url}.insteadOf",
                CANONICAL_UPSTREAM,
            ],
            check=True,
            capture_output=True,
            text=True,
        )

        run_env = os.environ.copy()
        run_env["HOME"] = str(home)
        run_env["GIT_CONFIG_NOSYSTEM"] = "1"
        run_env["GIT_ALLOW_PROTOCOL"] = "file"

        return root, base_commit, run_env

    def run_checker(self, root: Path, env):
        return subprocess.run(
            ["bash", "tools/check_patches.sh"],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
        )

    def test_complete_vendor_keeps_fast_local_clone(self) -> None:
        root, _, env = self.make_fixture(partial_vendor=False)

        result = self.run_checker(root, env)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            "cloning a pristine decomp from vendor/ge-decomp",
            result.stdout,
        )
        self.assertNotIn("partial/promisor", result.stdout)
        self.assertIn("  ok    0001-test.patch", result.stdout)
        self.assertIn("all 1 patches apply", result.stdout)

    def test_promisor_vendor_uses_exact_head_network_fallback(self) -> None:
        root, base_commit, env = self.make_fixture(partial_vendor=True)

        result = self.run_checker(root, env)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("local decomp is partial/promisor", result.stdout)
        self.assertIn(base_commit, result.stdout)
        self.assertIn("  ok    0001-test.patch", result.stdout)
        self.assertIn("all 1 patches apply", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
