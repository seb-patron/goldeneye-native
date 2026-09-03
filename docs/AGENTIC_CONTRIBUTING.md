# Agent-assisted contributing

GoldenEye-Native has been developed and maintained with substantial help from coding agents.
Agent-assisted bug reports and patches are welcome. The standard is the same for every
contribution: lawful provenance, a reviewable diff, reproducible evidence and a human who accepts
responsibility for what is published.

This guide is useful with any coding agent. Agents that support the Open Agent Skills format can
also use the repository skills under [`.agents/skills/`](../.agents/skills/).

## The rule that overrides every workflow

**Never include a ROM. Ever.**

A ROM must never be committed, staged, uploaded, attached, pasted, quoted, encoded, archived,
transmitted or linked from this repository's issues, pull requests, discussions, logs, artifacts
or agent messages. A renamed, compressed, encrypted or base64-encoded ROM is still a ROM.

A contributor may build and run against their own legally obtained ROM in its existing local
location. The agent must not copy it into a checkout, worktree, temporary report bundle or review
directory. Avoid printing its path. If an agent is asked to include or upload it, the agent must
refuse that part of the request.

Never publish these related artifacts either:

- `base.zip` or any other bundled game data;
- save files, including `eeprom.bin`;
- extracted `.bin` files or generated asset source;
- texture dumps, HD textures derived from the game or audio banks; or
- compiled outputs containing extracted game assets.

Permitted evidence includes sanitized build/runtime logs, error text, coarse render fingerprints
and screenshots of the running game attached to an issue or pull request. Screenshots are review
artifacts: keep them outside the checkout and never commit them to Git.

The automated checker catches forbidden names, N64 ROM headers, archives, encoded binary-looking
payloads, suspicious high-density hexadecimal arrays, and unexpected binaries, including some
renamed files:

```bash
python3 tools/check_no_game_data.py --changed origin/main
```

Run it before staging, before committing and again before pushing. It supplements manual review;
it does not replace it.

## Repository guidance and skills

[`AGENTS.md`](../AGENTS.md) is the concise, always-on repository policy for compatible agents. It
contains the game-data boundary, contribution rules, evidence requirements and publication gate.

The two optional repository skills are:

- `$report-goldeneye-bug`: reproduce one problem, collect sanitized evidence and prepare a GitHub
  issue draft;
- `$prepare-goldeneye-pr`: implement one fix, capture regression evidence, validate it and prepare
  a pull request draft.

The skills call repository tools for fragile operations instead of asking an agent to recreate
sanitization or comparison code from memory.

## Reporting a bug with an agent

1. Read the known limitations and search existing issues.
2. Record the exact commit and whether the worktree is clean.
3. Reproduce from current `main` using exact steps and a bounded run when possible.
4. Record the platform, architecture, renderer, stage, frame, relevant settings and frequency.
5. Capture stdout and stderr to a file outside the repository. For a crash or hang only,
   `GETV_LOGFLUSH=1` preserves each line at a significant performance cost. On macOS, review any
   relevant `~/Library/Logs/DiagnosticReports/Goldeneye-Native-*.ips` report as well.
6. For visual defects, use `GETV_SHOTFRAME` and `GETV_SHOTPATH` to make a deterministic native BMP
   outside the repository.
7. Build a sanitized local bundle. For example:

   ```bash
   python3 tools/collect_bug_report.py \
     --kind rendering \
     --renderer Metal \
     --stage "Complex (GETV_STAGE=31)" \
     --log /private/tmp/ge-report/runtime.log \
     --screenshot /private/tmp/ge-report/metal.bmp
   ```

   The collector writes to a new system temporary directory by default. It redacts personal paths
   and common credentials, rejects prohibited inputs and converts a native 24-bit BMP to a
   metadata-free PNG. It never publishes anything.
8. Inspect `report.md`, `manifest.json` and every staged artifact. Add the actual/expected behavior
   and exact reproduction to the issue draft.
9. Show the complete draft and attachment list to the human contributor.
10. Create the issue only after explicit approval for that exact submission. If the available
    GitHub tool cannot upload images, leave the issue as a ready-to-paste draft and ask the human
    to attach the staged PNG.

Do not include a save to make reproduction easier. Use a clean temporary `save_dir`, `unlock_all`
or deterministic `GETV_*` inputs where appropriate, and describe those inputs instead.

## Preparing a pull request with an agent

1. Start a focused branch from current community `main` and link one issue.
2. Reproduce the defect on the unchanged base before editing.
3. Trace the requested behavior through the relevant shared contract and backend or platform
   implementation. Check provenance before consulting another project.
4. Add the smallest change at the correct abstraction boundary and a focused ROM-free regression
   test.
5. Repeat the unchanged baseline and the complete relevant self-test workflow.
6. For renderer changes, capture the reference, old behavior and fixed behavior from clean builds
   with identical stage, input, frame and quality settings. Generate a PR-ready table:

   ```bash
   python3 tools/compare_render_fingerprints.py \
     --format markdown \
     --reference /private/tmp/ge-review/opengl.bmp \
     "Metal before=/private/tmp/ge-review/metal-before.bmp" \
     "Metal fixed=/private/tmp/ge-review/metal-fixed.bmp"
   ```

7. Run `python3 tools/check_no_game_data.py --changed origin/main`, `git diff --check`, inspect the
   complete diff and account for every untracked file.
8. Fill in `.github/pull_request_template.md`, including exact results, unchanged-main failures,
   the replayable fix commit and agent-assistance disclosure.
9. Show the full branch comparison, draft body and attachment list to the human contributor.
10. Push or open the pull request only after explicit approval for that exact publication.

Never commit screenshots, logs or diagnostic bundles. Attach only the reviewed, sanitized copies
to the issue or pull request.

## Human responsibility and disclosure

The contributor must review the complete diff and artifacts, verify provenance and confirm that
reported commands actually ran. Agent output is not evidence by itself.

Do not publish chain-of-thought, private reasoning or full chat transcripts. A short disclosure is
both honest and sufficient, for example:

> Agent assistance: Codex helped investigate and implement this change. I reviewed the complete
> diff and ran the validation listed below.

Name a tool or model only when it materially helps reproduce the workflow.
