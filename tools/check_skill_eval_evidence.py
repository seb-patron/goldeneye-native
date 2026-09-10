#!/usr/bin/env python3
"""Require fresh, committed, replayable evidence for repository skill changes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

# The Windows setup's embeddable Python never adds the script directory to sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import skill_eval

ROOT = Path(__file__).resolve().parents[1]
SKILL_ROOTS = (".agents/skills/", ".claude/skills/")


def git(root, *args):
    return subprocess.check_output(["git", *args], cwd=root)


def blob(root, ref, path):
    process = subprocess.run(["git", "show", ref + ":" + path], cwd=root, capture_output=True)
    if process.returncode:
        return None
    return process.stdout


def fingerprint(data):
    return hashlib.sha256(data).hexdigest() if data is not None else None


def evidence_path(value, suffix):
    if not isinstance(value, str):
        raise ValueError("evidence path must be a string")
    path = PurePosixPath(value)
    if (not value.startswith("docs/evals/") or ".." in path.parts or "\\" in value
            or not value.endswith(suffix)):
        raise ValueError("evidence must use a repository-relative docs/evals/ path")
    return value


def ensure_snapshots(root, record, fetch_missing):
    for revision in ("before", "after"):
        sha = record["revisions"][revision]["sha"]
        if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{40}", sha):
            raise ValueError("evaluated revisions must be full Git commit IDs")
        exists = subprocess.run(["git", "cat-file", "-e", sha + "^{commit}"], cwd=root,
                                capture_output=True).returncode == 0
        if not exists:
            if not fetch_missing:
                raise ValueError(f"missing evaluated commit {sha}; fetch it or use --fetch-missing")
            # The remote is fixed, never supplied by an evidence record. This is read-only.
            git(root, "fetch", "--no-tags", "origin", sha)


def check(base, head="HEAD", root=ROOT, replay=None, cases=None, fetch_missing=False):
    base = git(root, "merge-base", base, head).decode().strip()
    head = git(root, "rev-parse", "--verify", head + "^{commit}").decode().strip()
    changed = set(git(root, "diff", "--name-only", "--no-renames", base, head).decode().splitlines())
    skills = {p for p in changed if p.startswith(SKILL_ROOTS)}
    if not skills:
        return []
    cases = skill_eval.load_cases() if cases is None else cases
    replay = skill_eval.replay if replay is None else replay
    manifests = sorted(p for p in changed if p.startswith("docs/evals/") and p.endswith(".evidence.json"))
    covered = set()
    errors = []
    for manifest in manifests:
        try:
            data = blob(root, head, manifest)
            if data is None:
                continue  # A deleted historical manifest cannot satisfy the gate.
            entry = json.loads(data)
            if entry.get("version") != 1:
                raise ValueError("unsupported evidence manifest version")
            if blob(root, base, manifest) is not None:
                raise ValueError("add a new evidence manifest; preserve historical results")
            report = evidence_path(entry["report"], ".md")
            record_path = evidence_path(entry["record"], ".json")
            source = evidence_path(entry["source_record"], ".json") if entry.get("source_record") else None
            for path in [report, record_path, *([source] if source else [])]:
                committed = blob(root, head, path)
                if committed is None:
                    raise ValueError(f"missing committed evidence: {path}")
                # Replay must see exactly the committed data, not an untracked replacement.
                disk = (root / path).read_bytes().replace(b"\r\n", b"\n")
                if disk != committed.replace(b"\r\n", b"\n"):
                    raise ValueError(f"working-copy evidence differs from commit: {path}")
            if (report not in changed or record_path not in changed
                    or blob(root, base, report) is not None or blob(root, base, record_path) is not None):
                raise ValueError("comparison report and result record must be new in this PR")
            if not blob(root, head, report).strip():
                raise ValueError("comparison report is empty")
            record = json.loads(blob(root, head, record_path))
            if record.get("repeats", 0) < 2 or not record.get("finished_utc"):
                raise ValueError("require a completed comparison with at least two repetitions")
            for path in ("tools/skill_eval.py", "tools/skill_eval_cases.json"):
                disk = (root / path).read_bytes().replace(b"\r\n", b"\n")
                if disk != blob(root, head, path).replace(b"\r\n", b"\n"):
                    raise ValueError("evaluator working copy differs from committed evaluator")
            ensure_snapshots(root, record, fetch_missing)
            replay(root / record_path, root / source if source else None)
            before = record["revisions"]["before"]["policy_sha256"]
            after = record["revisions"]["after"]["policy_sha256"]
            for path in skills:
                skill_name = path.split("/")[2]
                relevant = {c["id"] for c in cases if c["skill"] == skill_name}
                if not relevant or not relevant <= set(record["case_ids"]):
                    continue
                if (path in before and path in after
                        and before[path] == fingerprint(blob(root, base, path))
                        and after[path] == fingerprint(blob(root, head, path))):
                    covered.add(path)
        except (ValueError, KeyError, TypeError, OSError, subprocess.CalledProcessError) as error:
            errors.append(f"{manifest}: {error}")
    if skills - covered:
        errors.append("missing fresh before/after eval evidence for: " + ", ".join(sorted(skills - covered)))
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--fetch-missing", action="store_true",
                        help="fetch exact missing evaluation commits from origin (for squash merges)")
    args = parser.parse_args()
    errors = check(args.base, args.head, fetch_missing=args.fetch_missing)
    for error in errors:
        print(error, file=sys.stderr)
    if not errors:
        print("Skill evidence gate passed: every changed skill is covered, or no skills changed.")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
