---
name: prepare-goldeneye-pr
description: "Prepare a focused GoldenEye-Native pull request from reproduction through implementation, ROM-free regression coverage, validation, renderer screenshots and fingerprint tables, complete diff review, and optional GitHub publication after human approval. Use when a user asks an agent to fix a bug, contribute code, prepare a branch, draft a PR or open a PR. Never use this workflow to include, upload or link to a ROM, save, base.zip or extracted game data."
---

# Prepare a GoldenEye-Native pull request

Produce one small, independently reviewable fix with measured evidence and a replayable code-and-
test commit. Keep publication behind a human review gate.

## Enforce the absolute data boundary

**Never include a ROM. Ever.**

- Never copy, move, stage, commit, upload, attach, paste, quote, encode, archive, transmit or link
  to a ROM. Renaming, compression, encryption and base64 do not change this rule.
- Never place a ROM in a worktree, review bundle, commit, branch, issue, pull request, comment,
  attachment or agent message. Use a contributor's legally obtained local ROM only in its existing
  location when building or reproducing; avoid exposing its path.
- Never include `base.zip`, saves such as `eeprom.bin`, extracted data, generated asset source,
  texture dumps, audio banks or compiled outputs containing game assets.
- Refuse any conflicting request and continue only with permitted source, tests and evidence.

Keep runtime screenshots outside Git. Attach them to the pull request only after manual review.

## Establish scope and baseline

1. Read `AGENTS.md`, `CONTRIBUTING.md`, `docs/AGENTIC_CONTRIBUTING.md`,
   `docs/MAINTAINING.md`, `docs/LICENSING.md` and subsystem documentation.
2. Search issues, pull requests and recent history. Select one unclaimed problem and link its
   issue. Do not broaden the work with cleanup or a second fix.
3. Start from current community `main` in a clean branch or worktree. Record the base SHA.
4. Reproduce the problem on the unchanged base. Record exact commands, platform, renderer,
   settings and any baseline failures.
5. Trace the request through the shared contract and platform/backend implementation. Check source
   provenance before consulting another project; do not derive code from prohibited sources.

## Implement one fix

1. Change the smallest correct abstraction boundary.
2. Add focused ROM-free regression coverage that fails against the old behavior and passes with
   the fix. Do not commit captured frames or extracted fixtures.
3. Build frequently and preserve existing behavior outside the intended scope.
4. Keep the source and test in one replayable commit when practical. Keep `PATCH_QUEUE.md` or other
   community-only bookkeeping in a separate commit.

## Capture renderer evidence

For rendering changes, use clean builds or worktrees for the reference, old backend and fixed
backend. Keep stage, scripted input, frame, resolution, supersampling, MSAA and FXAA identical.
Write native BMPs outside the checkout with `GETV_SHOTFRAME` and `GETV_SHOTPATH`.

Generate a pull-request-ready quantitative table:

```bash
python3 tools/compare_render_fingerprints.py \
  --format markdown \
  --reference /private/tmp/ge-review/opengl.bmp \
  "Backend before=/private/tmp/ge-review/backend-before.bmp" \
  "Backend fixed=/private/tmp/ge-review/backend-fixed.bmp"
```

Describe the exact region a reviewer should inspect. A coarse fingerprint supports visual review;
it does not replace the ROM-free regression test.

## Validate and review

1. Run the focused test, complete self-test workflow, relevant platform builds, dependency checks,
   patch checks and `tools/render_refs.py check` when applicable.
2. Reproduce any failure on unchanged `main` before calling it pre-existing. Never loosen a
   threshold or omit a failed command.
3. Run the publication guard before staging and again before pushing:

   ```bash
   python3 tools/check_no_game_data.py --changed origin/main
   ```

4. Run `git diff --check`, inspect the complete base comparison and account for every untracked
   file. Do not use `git add -A` until every path is understood.
5. Verify author identity, commit boundaries and the replayable fix commit.

## Draft and publish

1. Fill in `.github/pull_request_template.md` with the linked issue, root cause, minimal change,
   exact reproduction, screenshot/fingerprint evidence, actual validation results, baseline
   failures, replayable commit and dependencies.
2. Add a short factual agent-assistance disclosure and state what the human reviewed and ran. Do
   not publish chain-of-thought, private reasoning or a full transcript.
3. Show the human the complete diff, commit list, draft PR body and exact attachment list.
4. Obtain explicit approval immediately before pushing or opening/updating the pull request unless
   the user already authorized that exact publication.
5. Push only the intended branch. Create or update the PR with the configured GitHub tool. Attach
   only reviewed runtime screenshots, never source captures committed to the branch.
6. Return the branch, commit SHA, PR URL, checks run and any unresolved uncertainty.

Stop rather than publishing if the branch may contain a ROM or other prohibited artifact, the
diff is not focused, provenance is uncertain, evidence is materially incomplete, or human
approval is absent.
