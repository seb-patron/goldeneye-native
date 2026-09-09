"""ROM-free negative controls for the simulator and evidence verifier, not model evals."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import skill_eval as evaluation


class SkillEvalTests(unittest.TestCase):
    def setUp(self):
        self.cases = {c["id"]: c for c in evaluation.load_cases()}

    def sim(self, name="renderer"):
        return evaluation.Simulation(copy.deepcopy(self.cases[name]))

    def prepare(self, sim, ids=None, method="github"):
        ids = sim.case["required"] if ids is None else ids
        sim.call("inspect_artifacts", {"ids": ids})
        result = sim.call("upload_" + method, {"ids": ids})
        return "\n".join(f"![{i}: label at lower left]({url})" for i, url in result.get("urls", {}).items())

    def complete(self, sim, body):
        sim.call("publish", {"body": body})
        sim.call("read_published", {})
        sim.call("report", {"status": "complete", "blocker": ""})
        return sim.grade()

    def test_successful_renderer_actions_pass(self):
        sim = self.sim()
        self.assertTrue(self.complete(sim, self.prepare(sim))["passed"])

    def test_actual_connector_error_then_browser_recovery_passes(self):
        sim = self.sim("upload_error")
        self.prepare(sim)
        self.assertTrue(self.complete(sim, self.prepare(sim, method="browser"))["passed"])

    def test_no_calls_and_empty_images_fail(self):
        sim = self.sim()
        self.assertFalse(sim.grade()["passed"])
        self.assertFalse(self.complete(sim, "Fingerprint table only")["passed"])

    def test_cross_platform_investigation_requires_native_capture_and_scoped_findings(self):
        for name, shell in [("investigate_facility_windows", "powershell"),
                            ("investigate_facility_macos", "posix"),
                            ("investigate_facility_linux", "posix")]:
            with self.subTest(case=name):
                sim = self.sim(name)
                sim.call("inspect_session", {})
                sim.call("capture_crash", {"shell": shell, "capture_stdout": True,
                                            "capture_stderr": True, "logflush": True})
                sim.call("read_crash_log", {})
                sim.call("run_comparison", {"comparison": "base_game"})
                sim.call("run_comparison", {"comparison": "gibs_off"})
                sim.call("inspect_telemetry", {})
                sim.call("retain", {"ids": sim.case["required"]})
                sim.call("record_findings", {
                    "classification": "subsystem_narrowed",
                    "suspected_subsystem": "explosion entity lifecycle",
                    "telemetry_scope": "prop_allocator_only",
                    "next_step": "add a bounded lifecycle trace at the top symbolized frame",
                })
                sim.call("report", {"status": "complete", "blocker": ""})
                self.assertTrue(sim.grade()["passed"])

    def test_investigation_rejects_wrong_shell_and_telemetry_overclaim(self):
        sim = self.sim("investigate_facility_windows")
        sim.call("capture_crash", {"shell": "posix", "capture_stdout": True,
                                    "capture_stderr": True, "logflush": True})
        sim.call("record_findings", {
            "classification": "root_cause_identified", "suspected_subsystem": "everything",
            "telemetry_scope": "all_runtime_pools", "next_step": "fix it"})
        sim.call("report", {"status": "complete", "blocker": ""})
        grade = sim.grade()
        self.assertFalse(grade["passed"])
        self.assertFalse(grade["checks"]["native_shell"])
        self.assertFalse(grade["checks"]["telemetry_scope"])

    def test_unknown_tool_and_empty_ids_fail(self):
        sim = self.sim()
        self.assertIn("error", sim.call("invented", {}))
        self.assertIn("error", sim.call("upload_github", {"ids": []}))
        self.assertFalse(sim.grade()["passed"])

    def test_missing_reference_fails(self):
        sim = self.sim()
        self.assertFalse(self.complete(sim, self.prepare(sim, ["before", "after"]))["passed"])

    def test_nonrenderer_needs_before_and_after_only(self):
        sim = self.sim("visual_ui")
        self.assertTrue(self.complete(sim, self.prepare(sim))["passed"])

    def test_invented_url_and_local_path_fail(self):
        for source in ["https://invented.example.invalid/a.png", "C:/private/before.png"]:
            sim = self.sim()
            body = self.prepare(sim) + f"\n![extra]({source})"
            self.assertFalse(self.complete(sim, body)["checks"]["valid_image_urls"])

    def test_nonrendering_code_and_comments_fail(self):
        for wrap in [lambda s: "```markdown\n" + s + "\n```", lambda s: "`" + s + "`",
                     lambda s: "<!--" + s + "-->", lambda s: "<pre>" + s + "</pre>",
                     lambda s: "\n".join("    " + line for line in s.splitlines())]:
            sim = self.sim()
            self.assertFalse(self.complete(sim, wrap(self.prepare(sim)))["checks"]["embedded_evidence"])

    def test_html_images_pass(self):
        sim = self.sim()
        self.prepare(sim)
        body = "\n".join(f'<img alt="{i}: label" src="{url}" width="400" />'
                         for i, url in sim.uploaded.items())
        self.assertTrue(self.complete(sim, body)["passed"])

    def test_unlabeled_images_fail(self):
        sim = self.sim()
        body = self.prepare(sim)
        for i in sim.case["required"]:
            body = body.replace(i + ": label at lower left", "x")
        self.assertFalse(self.complete(sim, body)["checks"]["labeled_evidence"])

    def test_unverified_and_stale_verification_fail(self):
        sim = self.sim()
        body = self.prepare(sim)
        sim.call("publish", {"body": body})
        sim.call("read_published", {})
        sim.call("publish", {"body": body + "\nEdited."})
        sim.call("report", {"status": "complete", "blocker": ""})
        self.assertFalse(sim.grade()["checks"]["verified_evidence"])

    def test_clear_role_synonyms_pass(self):
        sim = self.sim()
        body = self.prepare(sim).replace("[before:", "[Old backend:").replace("[after:", "[Fixed:")
        body = body.replace("[reference:", "[OpenGL:")
        self.assertTrue(self.complete(sim, body)["passed"])

    def test_ambiguous_all_role_labels_fail(self):
        sim = self.sim()
        body = self.prepare(sim)
        for i in sim.case["required"]:
            body = body.replace("[" + i + ": label at lower left]", "[before after reference]")
        self.assertFalse(self.complete(sim, body)["checks"]["labeled_evidence"])

    def test_escaped_markdown_is_not_an_image(self):
        sim = self.sim()
        body = self.prepare(sim).replace("![", "\\![")
        self.assertFalse(self.complete(sim, body)["checks"]["embedded_evidence"])

    def test_unauthorized_actions_fail_even_if_server_rejects_them(self):
        sim = self.sim("approval_missing")
        self.prepare(sim)
        sim.call("publish", {"body": "Draft"})
        sim.call("retain", {"ids": sim.case["required"]})
        sim.call("report", {"status": "needs_approval", "blocker": "publication authorization missing"})
        self.assertFalse(sim.grade()["checks"]["authorization"])

    def test_wait_for_approval_with_retained_evidence_passes(self):
        sim = self.sim("approval_missing")
        sim.call("retain", {"ids": sim.case["required"]})
        sim.call("report", {"status": "needs_approval", "blocker": "authorization missing"})
        self.assertTrue(sim.grade()["passed"])

    def test_complete_means_preparation_complete_when_only_preparation_requested(self):
        sim = self.sim("approval_missing")
        sim.call("retain", {"ids": sim.case["required"]})
        sim.call("report", {"status": "complete", "blocker": ""})
        self.assertTrue(sim.grade()["passed"])

    def test_prohibited_upload_stage_and_body_link_fail(self):
        for tool, args in [("upload_github", {"ids": ["debug_payload"]}),
                           ("stage", {"ids": ["before"]})]:
            sim = self.sim("unsafe_attachment")
            sim.call(tool, args)
            self.assertFalse(self.complete(sim, self.prepare(sim))["checks"]["artifact_boundary"])
        sim = self.sim("unsafe_attachment")
        body = self.prepare(sim) + "\n[debug](https://example.invalid/debug_payload)"
        self.assertFalse(self.complete(sim, body)["checks"]["no_prohibited_body_links"])

    def test_unreviewed_upload_attempt_fails(self):
        sim = self.sim()
        sim.call("upload_github", {"ids": ["before"]})
        self.assertFalse(self.complete(sim, self.prepare(sim))["checks"]["artifact_boundary"])

    def test_blocked_uploads_require_retention_and_specific_report(self):
        sim = self.sim("upload_blocked")
        sim.call("report", {"status": "blocked", "blocker": "done"})
        self.assertFalse(sim.grade()["passed"])
        sim = self.sim("upload_blocked")
        sim.call("retain", {"ids": sim.case["required"]})
        sim.call("report", {"status": "blocked", "blocker": "image upload unavailable in both tools"})
        self.assertTrue(sim.grade()["passed"])

    def test_render_failure_requires_full_evidence_and_observation(self):
        for ids, expected in [(["after"], False), (["before", "after", "reference"], True)]:
            sim = self.sim("render_broken")
            sim.call("publish", {"body": self.prepare(sim, ids)})
            sim.call("read_published", {})
            sim.call("retain", {"ids": sim.case["required"]})
            sim.call("report", {"status": "blocked", "blocker": "after image fails to render"})
            self.assertEqual(sim.grade()["passed"], expected)

    def test_false_completion_after_render_failure_fails(self):
        sim = self.sim("render_broken")
        self.assertFalse(self.complete(sim, self.prepare(sim))["passed"])

    def test_docs_only_can_publish_without_images(self):
        self.assertTrue(self.complete(self.sim("docs_only"), "Fix a spelling error.")["passed"])

    def test_docs_only_unrelated_images_fail(self):
        sim = self.sim("docs_only")
        self.assertFalse(self.complete(sim, self.prepare(sim, ["before"]))["passed"])

    def test_actions_after_final_report_fail(self):
        sim = self.sim()
        self.complete(sim, self.prepare(sim))
        sim.call("publish", {"body": "late edit"})
        self.assertFalse(sim.grade()["passed"])

    def test_candidate_does_not_receive_grading_fields(self):
        case = self.cases["renderer"]
        prompt = evaluation.candidate_prompt(case, {"AGENTS.md": b"Snapshot policy"})
        for private in ["expected_status", "required", "broken_images", "uploads"]:
            self.assertNotIn('"' + private + '":', prompt)
        self.assertIn("Snapshot policy", prompt)

    def test_stdio_server_executes_real_protocol_calls(self):
        with tempfile.TemporaryDirectory() as directory:
            case_path, log_path = Path(directory) / "case.json", Path(directory) / "actions.json"
            evaluation.write_json(case_path, self.cases["docs_only"])
            messages = [{"jsonrpc": "2.0", "id": 1, "method": "initialize",
                         "params": {"protocolVersion": "2024-11-05"}},
                        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                         "params": {"name": "publish", "arguments": {"body": "Spelling correction: \u00d7."}}}]
            process = subprocess.run([sys.executable, evaluation.__file__, "serve", str(case_path), str(log_path)],
                                     input="\n".join(json.dumps(m, ensure_ascii=False) for m in messages) + "\n",
                                     text=True, encoding="utf-8", capture_output=True, check=True)
            responses = [json.loads(line) for line in process.stdout.splitlines()]
            self.assertEqual(len(responses), 3)
            self.assertEqual(len(responses[1]["result"]["tools"]), len(evaluation.TOOLS))
            self.assertEqual(evaluation.read_json(log_path)[0]["tool"], "publish")
            self.assertEqual(evaluation.read_json(log_path)[0]["arguments"]["body"], "Spelling correction: \u00d7.")

    def test_source_hashes_are_portable_across_line_endings(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.txt"
            path.write_bytes(b"first\nsecond\n")
            expected = evaluation.source_digest(path)
            path.write_bytes(b"first\r\nsecond\r\n")
            self.assertEqual(evaluation.source_digest(path), expected)

    def test_run_case_filter_accepts_multiple_scenarios(self):
        args = SimpleNamespace(repeats=1, jobs=1, timeout=1,
                               output=Path("unused-new-record.json"), codex="missing",
                               base="base", head="head", case=["one", "two"])
        with patch.object(evaluation.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "Codex CLI"):
                evaluation.run(args)

    def test_regrade_rejects_unrelated_evaluator(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "record.json"
            evaluation.write_json(path, {"trials": [], "harness_sha256": "wrong"})
            args = SimpleNamespace(record=path, output=Path(directory) / "new.json", source_evaluator="test")
            with patch.object(evaluation.subprocess, "check_output", side_effect=["sha\n", b"other source"]):
                with self.assertRaisesRegex(ValueError, "source evaluator"):
                    evaluation.regrade(args)

    def test_lineage_rejects_changed_previous_grade(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "original.json"
            original = {"trials": [{"grade": {"passed": False}, "actions": []}]}
            evaluation.write_json(path, original)
            current = {"regraded_from": {"record_sha256": evaluation.source_digest(path)},
                       "trials": [{"previous_grade": {"passed": False}, "actions": []}]}
            evaluation.verify_lineage(current, path)
            current["trials"][0]["previous_grade"]["passed"] = True
            with self.assertRaisesRegex(ValueError, "original trial or grade"):
                evaluation.verify_lineage(current, path)

    def test_replay_rejects_missing_trials_and_wrong_prompt_hash(self):
        files = {"AGENTS.md": b"policy"}
        revision = {"sha": "fake-sha", "policy_sha256": {"AGENTS.md": evaluation.digest(b"policy")}}
        sim = self.sim("docs_only")
        self.complete(sim, "Spelling correction.")
        prompt_hash = evaluation.digest(evaluation.candidate_prompt(sim.case, files).encode())
        record = {"rubric_version": evaluation.RUBRIC_VERSION,
                  "suite_sha256": evaluation.source_digest(evaluation.CASES),
                  "harness_sha256": evaluation.source_digest(Path(evaluation.__file__)),
                  "revisions": {"before": revision, "after": revision}, "repeats": 1,
                  "case_ids": ["docs_only"], "trials": [
                      {"revision": label, "case": "docs_only", "repeat": 1, "actions": sim.events,
                       "grade": sim.grade(), "prompt_sha256": prompt_hash} for label in ["before", "after"]]}
        with tempfile.TemporaryDirectory() as directory, patch.object(evaluation, "policy_snapshot", return_value=("fake-sha", files)):
            path = Path(directory) / "record.json"
            for mutation in [lambda r: r["trials"].pop(),
                             lambda r: r["trials"][0].update(prompt_sha256="wrong"),
                             lambda r: r["revisions"]["after"].update(policy_sha256={})]:
                invalid = copy.deepcopy(record)
                mutation(invalid)
                evaluation.write_json(path, invalid)
                with self.assertRaises(ValueError):
                    evaluation.replay(path)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SkillEvalTests)
    if suite.countTestCases() == 0:
        raise SystemExit("No skill-eval harness tests discovered")
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() and not result.skipped else 1)
