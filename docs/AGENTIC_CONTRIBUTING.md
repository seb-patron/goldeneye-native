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

An agent editing anything under `.github/workflows/`, `tools/`, or `getv/patches/` should also read
[`docs/CI_TRUST.md`](CI_TRUST.md), which inventories what CI actually fetches, compiles and
executes, and records the current (unactivated) merge-protection proposal.

The three optional repository skills are:

- `$investigate-goldeneye-bug`: reproduce and narrow a gameplay, crash, rendering,
  configuration or build failure on Windows, macOS or Linux, leaving a bounded local handoff;
- `$report-goldeneye-bug`: reproduce one problem, collect sanitized evidence and prepare a GitHub
  issue draft;
- `$prepare-goldeneye-pr`: implement one fix, capture regression evidence, validate it and prepare
  a pull request draft.

The skills call repository tools for fragile operations instead of asking an agent to recreate
sanitization or comparison code from memory.

### Skill format and Claude Code

The workflows use the [Agent Skills format](https://agentskills.io/specification): `SKILL.md`
contains `name` and `description` in YAML frontmatter followed by Markdown instructions.
The optional `agents/openai.yaml` supplies OpenAI interface metadata; it is not a second workflow.

Claude Code discovers the standard `SKILL.md` entrypoints under `.claude/skills/`. Each links to
its shared instructions under `.agents/skills/`, using ordinary relative Markdown references.
This works on Windows without symlink privileges. Edit the shared workflow in `.agents/skills/`;
if its name or description changes, update the small Claude entrypoint as well. No generator,
`targets/` overlays or vendor-specific YAML format is required. See the official
[Claude skill documentation](https://code.claude.com/docs/en/skills) and
[OpenAI skill documentation](https://learn.chatgpt.com/docs/build-skills).

`CLAUDE.md` imports `AGENTS.md` and documents CLI usage. `.claude/settings.json` uses Claude's
[native permission rules](https://code.claude.com/docs/en/permissions) for pushes and GitHub
operations. Broad command-group rules can also prompt for reads; there are no custom approval
grants or shell-parsing hooks. Human review and the existing game-data checker remain required.
The `--staged` check reads index blobs, including when a working copy has changed or been removed.

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
   umask 077
   python3 tools/collect_bug_report.py \
     --output "$HOME/Documents/Goldeneye-Native-Reports/rendering-$(date +%Y%m%d-%H%M%S)" \
     --kind rendering \
     --renderer Metal \
     --stage "Complex (GETV_STAGE=31)" \
     --log /private/tmp/ge-report/runtime.log \
     --screenshot /private/tmp/ge-report/metal.bmp
   ```

   Choose a new, durable private `--output` directory outside every checkout and temporary
   staging area. The example uses restrictive permissions for newly created files; verify the
   destination is private and not shared or automatically published. Without `--output`, the
   collector uses system temporary storage, which is not a retained deliverable. It redacts
   personal paths and common credentials, rejects prohibited inputs and converts a native
   24-bit BMP to a metadata-free PNG. It never publishes anything.
8. Inspect `report.md`, `manifest.json` and every staged artifact. Add the actual/expected behavior
   and exact reproduction to the issue draft. Reopen the final report, manifest and every
   sanitized attachment from the durable output directory, verify they are readable and
   complete, and record that directory in the handoff before removing temporary staging.
   Keep the private bundle through review and submission; cleanup must not remove the only
   retained copy.
9. Show the complete draft and attachment list to the human contributor.
10. Create the issue only after explicit approval for that exact submission. Upload reviewed
    screenshots using a capable GitHub tool or an available authenticated browser upload flow.
    A GitHub tool without image support does not prohibit using another available upload method
    within the authorized submission. Verify that the images render in the published body.
    If no available method can upload them, retain the ready-to-paste body and sanitized PNGs,
    explain the specific blocker and hand off only the remaining upload work to the human.

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
6. Visual bug fixes must include before/after screenshots embedded in the PR body. For renderer
   changes, also include the reference. Fingerprint tables supplement the images, not replace them.
   Capture the reference, old behavior and fixed behavior from clean builds
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
    Use the screenshot upload workflow above and verify that the images render in the PR body.
    If upload is blocked, report the missing images and retain them for handoff; do not describe
    the visual evidence as complete.

Never commit screenshots, logs or diagnostic bundles. Attach only the reviewed, sanitized copies
to the issue or pull request.

### Presenting visual comparisons

Follow the original Metal fixes: [three-point filtering PR #1](https://github.com/seb-patron/goldeneye-native/pull/1)
uses reference/before/after columns with full frames and matching road crops;
[blob-shadow PR #3](https://github.com/seb-patron/goldeneye-native/pull/3) uses separately labeled
reference/before/after images with captions explaining the visible difference. Either layout is
appropriate. Use descriptive alt text and say where to look. Include matching crops when the
defect is hard to see at full-frame size, and record the shared scene, input, frame, resolution and
quality settings. Embed the uploaded attachments so reviewers can see the comparison in the PR
body without downloading files. Local filesystem paths are not published image URLs.

## Human responsibility and disclosure

The contributor must review the complete diff and artifacts, verify provenance and confirm that
reported commands actually ran. Agent output is not evidence by itself.

Do not publish chain-of-thought, private reasoning or full chat transcripts. A short disclosure is
both honest and sufficient, for example:

> Agent assistance: Codex helped investigate and implement this change. I reviewed the complete
> diff and ran the validation listed below.

Name a tool or model only when it materially helps reproduce the workflow.
