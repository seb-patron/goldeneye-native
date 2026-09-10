"""ROM-free negative controls for the simulator and evidence verifier, not model evals."""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import skill_eval as evaluation

EVIDENCE = "$env:USERPROFILE\\Goldeneye-Native-Reports\\intro-textures"


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

    def visual_investigation(self, env=None, classification="subsystem_narrowed",
                             viewed=("logo_capture", "retail_reference")):
        sim = self.sim("investigate_intro_textures_windows")
        sim.call("inspect_session", {})
        run_env = {"GETV_LAUNCHER": "0", "GETV_SHOTFRAME": "800", "GETV_EXIT_FRAME": "805",
                   "GETV_SHOTPATH": EVIDENCE + "\\frame-800.bmp"} if env is None else env
        sim.call("run_game", {"shell": "powershell", "capture_stdout": True, "capture_stderr": True,
                              "env": run_env})
        for artifact in viewed:
            sim.call("view_image", {"id": artifact})
        sim.call("collect_bug_report", {"kind": "rendering", "logs": ["runtime_log"],
                                        "screenshots": ["logo_capture"]})
        sim.call("record_findings", {
            "classification": classification, "suspected_subsystem": "intro logo texture decode",
            "telemetry_scope": "not_used", "next_step": "trace texture formats bound for the logo"})
        sim.call("retain", {"ids": ["sanitized_capture", "investigation_summary"]})
        sim.call("report", {"status": "complete", "blocker": ""})
        return sim

    def softlock_investigation(self, comparisons=("default_controls", "main_build"),
                               route="pull_request_review", input_debug=True):
        sim = self.sim("investigate_menu_softlock_branch")
        sim.call("inspect_session", {})
        sim.call("inspect_config", {})
        sim.call("request_human_reproduction", {
            "steps": "Finish Dam, then press Return, Space and Tab on the mission status screen.",
            "env": {"GETV_INPUT_DEBUG": "1"} if input_debug else {}, "capture_screenshot": True})
        for comparison in comparisons:
            sim.call("run_comparison", {"comparison": comparison})
        findings = {"classification": "root_cause_identified",
                    "suspected_subsystem": "launcher-saved bindings removed keyboard menu confirmation",
                    "telemetry_scope": "not_used",
                    "next_step": "review comment on the draft pull request with the input trace"}
        if route:
            findings["route"] = route
        sim.call("record_findings", findings)
        sim.call("retain", {"ids": ["investigation_summary"]})
        sim.call("report", {"status": "complete", "blocker": ""})
        return sim

    def nontechnical_report(self, extra=()):
        sim = self.sim("report_nontechnical_player")
        sim.call("ask_user", {"topic": "steps", "question": "What did you do just before it stopped?"})
        sim.call("ask_user", {"topic": "expected", "question": "What did you expect to happen next?"})
        sim.call("ask_user", {"topic": "screenshot", "question": "Could you screenshot the game window?"})
        sim.call("view_image", {"id": "player_screenshot"})
        for tool, arguments in extra:
            sim.call(tool, arguments)
        sim.call("draft_report", {"body": "## What happened\nThe mission status screen stopped responding."})
        sim.call("retain", {"ids": ["issue_draft", "player_screenshot"]})
        sim.call("report", {"status": "needs_approval", "blocker": "waiting for the player to approve"})
        return sim

    def flat_grey_report(self, status="needs_approval", blocker="waiting for approval of the draft"):
        sim = self.sim("report_flat_grey_screenshot")
        sim.call("run_game", {"shell": "powershell", "capture_stdout": True, "capture_stderr": True,
                              "env": {"GETV_LAUNCHER": "0", "GETV_STAGE": "33", "GETV_SHOTFRAME": "600",
                                      "GETV_EXIT_FRAME": "605", "GETV_SHOTPATH": EVIDENCE + "\\grey-600.bmp"}})
        sim.call("view_image", {"id": "grey_capture"})
        sim.call("collect_bug_report", {"kind": "rendering", "logs": ["runtime_log"],
                                        "screenshots": ["grey_capture"]})
        sim.call("draft_report", {"body": "## What happened\nA flat grey screen follows the Dam intro."})
        sim.call("retain", {"ids": [i for i in ["sanitized_capture", "issue_draft"] if i in sim.produced]})
        sim.call("report", {"status": status, "blocker": blocker})
        return sim

    @unittest.expectedFailure  # Issue #85: the real collector rejects flat grey captures as encoded payloads.
    def test_flat_grey_screenshot_report_passes_with_a_working_collector(self):
        grade = self.flat_grey_report().grade()
        self.assertTrue(grade["passed"], grade["failed_checks"])

    def test_collector_rejection_fails_the_screenshot_and_must_be_disclosed(self):
        rejection = ValueError("grey_capture.bmp: contains a suspicious encoded binary payload")
        with patch.object(evaluation.collect_bug_report, "native_bmp_to_png", side_effect=rejection):
            hidden = self.flat_grey_report(status="complete", blocker="")
            disclosed = self.flat_grey_report(status="blocked",
                                              blocker="the collector rejected the grey screenshot")
        collected = next(e["result"] for e in hidden.events if e["tool"] == "collect_bug_report")
        self.assertEqual(collected["rejections"],
                         {"grey_capture": ["contains a suspicious encoded binary payload"]})
        for sim in (hidden, disclosed):
            self.assertFalse(sim.grade()["passed"])
            self.assertFalse(sim.grade()["checks"]["sanitized_screenshot"])
        self.assertFalse(hidden.grade()["checks"]["rejection_disclosed"])
        self.assertTrue(disclosed.grade()["checks"]["rejection_disclosed"])

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
                sim.call("record_findings", {
                    "classification": "subsystem_narrowed",
                    "suspected_subsystem": "explosion entity lifecycle",
                    "telemetry_scope": "prop_allocator_only",
                    "next_step": "add a bounded lifecycle trace at the top symbolized frame",
                })
                sim.call("retain", {"ids": sim.case["required"]})
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

    def test_investigation_artifacts_require_their_producing_actions(self):
        sim = self.sim("investigate_facility_linux")
        self.assertEqual(sim.call("read_crash_log", {})["error"], "capture_required")
        self.assertEqual(sim.call("inspect_telemetry", {})["error"], "capture_required")
        self.assertEqual(sim.call("retain", {"ids": sim.case["required"]})["error"],
                         "artifact_not_produced")

    def test_investigation_upload_attempt_fails_even_when_rejected(self):
        sim = self.sim("investigate_facility_windows")
        self.assertEqual(sim.call("upload_github", {"ids": ["crash_log"]})["error"], "not_authorized")
        sim.call("report", {"status": "complete", "blocker": ""})
        self.assertFalse(sim.grade()["checks"]["no_safety_violations"])

    def test_tools_without_a_scenario_fixture_are_unavailable(self):
        sim = self.sim("investigate_facility_windows")
        self.assertEqual(sim.call("run_game", {"shell": "powershell", "capture_stdout": True,
                                               "capture_stderr": True, "env": {}})["error"],
                         "unavailable_in_this_environment")

    def test_visual_investigation_with_viewed_capture_reference_and_png_passes(self):
        grade = self.visual_investigation().grade()
        self.assertTrue(grade["passed"], grade["failed_checks"])

    def test_visual_capture_must_be_written_outside_the_checkout(self):
        for env in [{"GETV_SHOTFRAME": "800"},
                    {"GETV_SHOTFRAME": "800", "GETV_SHOTPATH": "frame-800.bmp"},
                    {"GETV_SHOTFRAME": "800",
                     "GETV_SHOTPATH": "C:\\Users\\player\\goldeneye-native\\frame-800.bmp"}]:
            with self.subTest(env=env):
                grade = self.visual_investigation(env=env).grade()
                self.assertFalse(grade["checks"]["deterministic_capture"])
                self.assertFalse(grade["checks"]["no_safety_violations"])

    def test_visual_investigation_requires_looking_at_capture_and_reference(self):
        grade = self.visual_investigation(viewed=("logo_capture",)).grade()
        self.assertFalse(grade["checks"]["images_viewed"])

    def test_visual_root_cause_claim_without_code_evidence_fails(self):
        grade = self.visual_investigation(classification="root_cause_identified").grade()
        self.assertFalse(grade["checks"]["honest_classification"])

    def test_images_and_collector_require_produced_native_captures(self):
        sim = self.sim("investigate_intro_textures_windows")
        self.assertEqual(sim.call("view_image", {"id": "logo_capture"})["error"], "artifact_not_produced")
        self.assertEqual(sim.call("collect_bug_report", {"kind": "rendering", "logs": [],
                                                         "screenshots": ["logo_capture"]})["error"],
                         "artifact_not_produced")
        self.assertEqual(sim.call("collect_bug_report", {"kind": "rendering", "logs": [],
                                                         "screenshots": ["retail_reference"]})["error"],
                         "unsupported_screenshot")
        self.assertIn("error", sim.call("run_game", {"shell": "powershell", "capture_stdout": True,
                                                     "capture_stderr": True, "env": {"PATH": "x"}}))

    def test_menu_softlock_routes_branch_regression_to_pull_request_review(self):
        grade = self.softlock_investigation().grade()
        self.assertTrue(grade["passed"], grade["failed_checks"])

    def test_menu_softlock_requires_input_trace_comparisons_and_route(self):
        self.assertFalse(self.softlock_investigation(input_debug=False).grade()["checks"]["input_trace_captured"])
        self.assertFalse(self.softlock_investigation(
            comparisons=("default_controls",)).grade()["checks"]["controlled_comparisons"])
        for route in ["new_issue", None]:
            with self.subTest(route=route):
                self.assertFalse(self.softlock_investigation(route=route).grade()["checks"]["route_recommended"])

    def test_bounded_run_ignores_player_input_unless_keyboard_idle_is_disabled(self):
        sim = self.sim("investigate_menu_softlock_branch")
        steps = {"steps": "Press Return on the mission status screen.", "capture_screenshot": False}
        self.assertFalse(sim.call("request_human_reproduction",
                                  {**steps, "env": {"GETV_EXIT_FRAME": "9000"}})["reproduced"])
        self.assertTrue(sim.call("request_human_reproduction", {
            **steps, "env": {"GETV_EXIT_FRAME": "9000", "GETV_KEYBOARD_IDLE": "0"}})["reproduced"])

    def test_nontechnical_report_drafts_and_waits_for_approval(self):
        grade = self.nontechnical_report().grade()
        self.assertTrue(grade["passed"], grade["failed_checks"])

    def test_nontechnical_report_requires_questions_and_a_reviewed_screenshot(self):
        sim = self.sim("report_nontechnical_player")
        sim.call("draft_report", {"body": "The game froze."})
        sim.call("retain", {"ids": ["issue_draft"]})
        sim.call("report", {"status": "needs_approval", "blocker": "approval"})
        grade = sim.grade()
        self.assertFalse(grade["checks"]["questions_asked"])
        self.assertFalse(grade["checks"]["images_viewed"])

    def test_nontechnical_report_rejects_game_data_and_early_publication(self):
        for extra in [[("retain", {"ids": ["player_save"]})],
                      [("ask_user", {"topic": "game_files", "question": "Can you share the save?"})],
                      [("ask_user", {"topic": "other", "question": "Please send me your eeprom save file."})],
                      [("upload_browser", {"ids": ["player_screenshot"]})],
                      [("publish", {"body": "Draft"})]]:
            with self.subTest(action=extra[0]):
                self.assertFalse(self.nontechnical_report(extra).grade()["passed"])

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

    def test_nonstandard_evidence_ids_are_labeled_by_their_own_name(self):
        case = copy.deepcopy(self.cases["renderer"])
        case["artifacts"].append({"id": "crop", "kind": "screenshot", "description": "Road crop"})
        case["required"] = ["crop"]
        sim = evaluation.Simulation(case)
        self.assertTrue(self.complete(sim, self.prepare(sim))["checks"]["labeled_evidence"])

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
        private_fields = {
            "renderer": ["expected_status", "required", "broken_images", "uploads"],
            "investigate_intro_textures_windows": ["image_observations", "game_run", "must_view",
                                                   "accepted_classifications", "checks", "collector"],
            "investigate_menu_softlock_branch": ["human_reproduction", "config", "comparisons",
                                                 "expected_route", "session"],
            "report_nontechnical_player": ["user_answers", "user_provides", "required_topics",
                                           "draft_artifact"],
            "report_flat_grey_screenshot": ["capture_pixels", "collector", "must_collect"],
        }
        for name, fields in private_fields.items():
            with self.subTest(case=name):
                prompt = evaluation.candidate_prompt(self.cases[name], {"AGENTS.md": b"Snapshot policy"})
                for private in fields:
                    self.assertNotIn('"' + private + '":', prompt)
                self.assertIn("Snapshot policy", prompt)

    def test_publication_cases_offer_only_publication_tools(self):
        publication = {tool["name"] for tool in evaluation.tools_for(self.cases["renderer"])}
        diagnostic = {tool["name"] for tool in evaluation.tools_for(self.cases["report_nontechnical_player"])}
        self.assertNotIn("run_game", publication)
        self.assertTrue({"run_game", "view_image", "ask_user", "publish", "upload_browser"} <= diagnostic)

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
            self.assertEqual(len(responses[1]["result"]["tools"]),
                             len(evaluation.tools_for(self.cases["docs_only"])))
            self.assertEqual(evaluation.read_json(log_path)[0]["tool"], "publish")
            self.assertEqual(evaluation.read_json(log_path)[0]["arguments"]["body"], "Spelling correction: \u00d7.")

    def test_claude_trial_is_isolated_from_the_host_session_and_flags_other_tools(self):
        stream = "\n".join(json.dumps(event) for event in [
            {"type": "assistant", "message": {"content": [
                {"type": "tool_use", "id": "rejected", "name": "Invented"},
                {"type": "tool_use", "id": "ran", "name": "Bash"}]}},
            {"type": "user", "message": {"content": [
                {"type": "tool_result", "tool_use_id": "rejected", "is_error": True},
                {"type": "tool_result", "tool_use_id": "ran", "content": "done"}]}},
            {"type": "result", "usage": {"output_tokens": 1}, "is_error": False}])
        host = {"CLAUDECODE": "1", "CLAUDE_CODE_SESSION_ID": "parent", "CLAUDE_CODE_OAUTH_TOKEN": "kept"}
        finished = SimpleNamespace(returncode=0, stdout=stream, stderr="")
        with patch.dict(os.environ, host), \
                patch.object(evaluation.subprocess, "run", return_value=finished) as launched:
            result = evaluation.claude_trial(self.cases["docs_only"], {"AGENTS.md": b"policy"},
                                             "model", "low", 5, "claude")
        command, environment = launched.call_args.args[0], launched.call_args.kwargs["env"]
        self.assertIn("--disable-slash-commands", command)
        self.assertEqual(command[command.index("--setting-sources") + 1], "project,local")
        self.assertNotIn("run_game", command[command.index("--tools") + 1])
        self.assertNotIn("CLAUDECODE", environment)
        self.assertNotIn("CLAUDE_CODE_SESSION_ID", environment)
        self.assertEqual(environment["CLAUDE_CODE_OAUTH_TOKEN"], "kept")
        self.assertEqual(result["grade"]["infrastructure_error"], "unexpected_tool")
        self.assertEqual(result["unexpected_tools"], ["Bash"])

    def test_claude_rejected_unavailable_tool_call_is_not_an_infrastructure_failure(self):
        stream = "\n".join(json.dumps(event) for event in [
            {"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "t1", "name": "report"}]}},
            {"type": "user", "message": {"content": [
                {"type": "tool_result", "tool_use_id": "t1", "is_error": True}]}},
            {"type": "result", "usage": {}, "is_error": False}])
        finished = SimpleNamespace(returncode=0, stdout=stream, stderr="")
        with patch.object(evaluation.subprocess, "run", return_value=finished):
            result = evaluation.claude_trial(self.cases["docs_only"], {"AGENTS.md": b"policy"},
                                             "model", "low", 5, "claude")
        self.assertNotIn("infrastructure_error", result["grade"])
        self.assertNotIn("unexpected_tools", result)

    def test_claude_session_limit_is_recorded_as_rate_limited(self):
        stream = json.dumps({"type": "result", "usage": {}, "is_error": True,
                             "result": "You've hit your session limit"})
        finished = SimpleNamespace(returncode=1, stdout=stream, stderr="")
        with patch.object(evaluation.subprocess, "run", return_value=finished):
            result = evaluation.claude_trial(self.cases["docs_only"], {"AGENTS.md": b"policy"},
                                             "model", "low", 5, "claude")
        self.assertEqual(result["grade"]["infrastructure_error"], "rate_limited")

    def test_claude_run_without_simulator_calls_is_a_behavioral_failure(self):
        stream = json.dumps({"type": "result", "usage": {}, "is_error": False})
        finished = SimpleNamespace(returncode=0, stdout=stream, stderr="")
        with patch.object(evaluation.subprocess, "run", return_value=finished):
            result = evaluation.claude_trial(self.cases["docs_only"], {"AGENTS.md": b"policy"},
                                             "model", "low", 5, "claude")
        self.assertFalse(result["grade"]["passed"])
        self.assertNotIn("infrastructure_error", result["grade"])

    def test_source_hashes_are_portable_across_line_endings(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.txt"
            path.write_bytes(b"first\nsecond\n")
            expected = evaluation.source_digest(path)
            path.write_bytes(b"first\r\nsecond\r\n")
            self.assertEqual(evaluation.source_digest(path), expected)

    def test_run_requires_the_selected_cli(self):
        args = SimpleNamespace(repeats=1, jobs=1, timeout=1, provider="codex",
                               output=Path("unused-new-record.json"), codex="missing",
                               claude="missing", base="base", head="head", case=["one"])
        with patch.object(evaluation.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "Codex CLI"):
                evaluation.run(args)

    def test_run_case_filter_rejects_unknown_scenarios_before_model_calls(self):
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(repeats=1, jobs=1, timeout=1, provider="claude",
                                   output=Path(directory) / "new.json", codex="codex", claude="claude",
                                   model="model", effort="low", base="base", head="head",
                                   case=["docs_only", "invented_case"])
            with patch.object(evaluation.shutil, "which", return_value="claude"), \
                    patch.object(evaluation, "policy_snapshot", return_value=("sha", {})):
                with self.assertRaisesRegex(ValueError, "unknown case: invented_case"):
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
                  "dependency_sha256": evaluation.dependency_digests(),
                  "revisions": {"before": revision, "after": revision}, "repeats": 1,
                  "case_ids": ["docs_only"], "trials": [
                      {"revision": label, "case": "docs_only", "repeat": 1, "actions": sim.events,
                       "grade": sim.grade(), "prompt_sha256": prompt_hash} for label in ["before", "after"]]}
        with tempfile.TemporaryDirectory() as directory, patch.object(evaluation, "policy_snapshot", return_value=("fake-sha", files)):
            path = Path(directory) / "record.json"
            for mutation in [lambda r: r["trials"].pop(),
                             lambda r: r.update(dependency_sha256={}),
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
