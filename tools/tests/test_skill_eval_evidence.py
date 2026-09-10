"""Exercise the evidence gate against real temporary Git histories; no model calls."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_skill_eval_evidence as gate


class EvidenceGateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.skill = ".agents/skills/demo/SKILL.md"
        self.write(self.skill, "old\n")
        self.write("tools/skill_eval.py", "# evaluator\n")
        self.write("tools/skill_eval_cases.json", "{}\n")
        self.command("init", "-q")
        self.commit()
        self.base = self.command("rev-parse", "HEAD").decode().strip()
        self.replay = Mock()

    def command(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root, stderr=subprocess.STDOUT)

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")

    def commit(self):
        paths = [str(p.relative_to(self.root)).replace("\\", "/") for p in self.root.rglob("*")
                 if p.is_file() and ".git" not in p.relative_to(self.root).parts]
        self.command("add", "--", *paths)
        self.command("-c", "user.name=Eval Fixture", "-c", "user.email=eval@example.invalid",
                     "commit", "-qm", "fixture")

    def proof(self):
        self.write(self.skill, "new\n")
        self.record = {"repeats": 2, "finished_utc": "2026-09-09T00:00:00Z", "case_ids": ["scenario"],
                       "revisions": {"before": {"sha": self.base, "policy_sha256": {self.skill: gate.fingerprint(b"old\n")}},
                                     "after": {"sha": self.base, "policy_sha256": {self.skill: gate.fingerprint(b"new\n")}}}}
        self.save_record()
        self.write("docs/evals/run.md", "Before/after comparison; ties and failures included.\n")
        self.write("docs/evals/run.evidence.json", json.dumps({"version": 1,
                   "report": "docs/evals/run.md", "record": "docs/evals/run.json"}))

    def save_record(self):
        self.write("docs/evals/run.json", json.dumps(self.record))

    def check(self, cases=None):
        return gate.check(self.base, root=self.root, replay=self.replay,
                          cases=[{"id": "scenario", "skill": "demo"}] if cases is None else cases)

    def test_non_skill_change_needs_no_evidence(self):
        self.write("README.md", "documentation\n")
        self.commit()
        self.assertEqual(self.check(), [])
        self.replay.assert_not_called()

    def test_skill_change_without_evidence_fails(self):
        self.write(self.skill, "new\n")
        self.commit()
        self.assertIn("missing fresh", " ".join(self.check()))

    def test_uncommitted_evidence_cannot_satisfy_gate(self):
        self.write(self.skill, "new\n")
        self.commit()
        self.proof()
        self.assertIn("missing fresh", " ".join(self.check()))

    def test_matching_committed_evidence_passes_and_is_replayed(self):
        self.proof()
        self.commit()
        self.assertEqual(self.check(), [])
        self.replay.assert_called_once_with(self.root / "docs/evals/run.json", None)

    def test_stale_after_or_wrong_before_fingerprint_fails(self):
        for revision in ["before", "after"]:
            with self.subTest(revision=revision):
                self.proof()
                self.record["revisions"][revision]["policy_sha256"][self.skill] = "stale"
                self.save_record()
                self.commit()
                self.assertIn("missing fresh", " ".join(self.check()))

    def test_missing_relevant_scenarios_fails(self):
        self.proof()
        self.record["case_ids"] = ["unrelated"]
        self.save_record()
        self.commit()
        self.assertIn("missing fresh", " ".join(self.check()))

    def test_unsupported_new_skill_cannot_reuse_other_scenarios(self):
        self.proof()
        self.commit()
        self.assertIn("missing fresh", " ".join(self.check(cases=[])))

    def test_claude_entrypoint_requires_its_own_snapshot(self):
        self.proof()
        self.write(".claude/skills/demo/SKILL.md", "entrypoint\n")
        self.commit()
        self.assertIn(".claude/skills/demo/SKILL.md", " ".join(self.check()))

    def test_insufficient_repeats_fails(self):
        self.proof()
        self.record["repeats"] = 1
        self.save_record()
        self.commit()
        self.assertIn("at least two repetitions", " ".join(self.check()))

    def test_replay_failure_fails_gate(self):
        self.proof()
        self.commit()
        self.replay.side_effect = ValueError("tampered results")
        self.assertIn("tampered results", " ".join(self.check()))

    def test_modified_working_copy_evidence_is_rejected(self):
        self.proof()
        self.commit()
        self.write("docs/evals/run.json", "{}\n")
        self.assertIn("differs from commit", " ".join(self.check()))

    def test_historical_report_cannot_be_rewritten_for_new_change(self):
        self.proof()
        self.commit()
        self.base = self.command("rev-parse", "HEAD").decode().strip()
        self.write(self.skill, "third\n")
        self.record["revisions"]["before"]["policy_sha256"][self.skill] = gate.fingerprint(b"new\n")
        self.record["revisions"]["after"]["policy_sha256"][self.skill] = gate.fingerprint(b"third\n")
        self.save_record()
        self.write("docs/evals/another.evidence.json", json.dumps({"version": 1,
                   "report": "docs/evals/run.md", "record": "docs/evals/run.json"}))
        self.commit()
        self.assertIn("must be new", " ".join(self.check()))

    def test_gate_imports_evaluator_without_script_directory_on_sys_path(self):
        # -I omits the script directory exactly as the Windows setup's embeddable Python does.
        process = subprocess.run([sys.executable, "-I", gate.__file__, "--help"],
                                 capture_output=True, text=True)
        self.assertEqual(process.returncode, 0, process.stderr)

    def test_path_traversal_rejected(self):
        with self.assertRaises(ValueError):
            gate.evidence_path("docs/evals/../../secret.json", ".json")

    def test_missing_snapshot_can_fetch_only_exact_sha_from_origin(self):
        self.proof()
        missing = "a" * 40
        self.record["revisions"]["after"]["sha"] = missing
        with patch.object(gate, "git") as fetch:
            gate.ensure_snapshots(self.root, self.record, True)
            fetch.assert_called_once_with(self.root, "fetch", "--no-tags", "origin", missing)

    def test_missing_snapshot_without_fetch_fails(self):
        self.proof()
        self.record["revisions"]["after"]["sha"] = "a" * 40
        with self.assertRaisesRegex(ValueError, "missing evaluated commit"):
            gate.ensure_snapshots(self.root, self.record, False)

    def test_invalid_snapshot_cannot_trigger_fetch(self):
        self.proof()
        self.record["revisions"]["before"]["sha"] = "--upload-pack=unexpected"
        with patch.object(gate, "git") as fetch:
            with self.assertRaises(ValueError):
                gate.ensure_snapshots(self.root, self.record, True)
            fetch.assert_not_called()


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(EvidenceGateTests)
    if not suite.countTestCases():
        raise SystemExit("No evidence-gate tests discovered")
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() and not result.skipped else 1)
