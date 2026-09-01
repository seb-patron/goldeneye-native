# GoldenEye-Native agent instructions

These instructions apply to every agent working anywhere in this repository. Read
`CONTRIBUTING.md`, `docs/AGENTIC_CONTRIBUTING.md` and the relevant setup or subsystem documents
before changing code.

## Absolute game-data boundary

**Never include a ROM. Ever.**

- Never add, copy, move, stage, commit, upload, attach, paste, quote, encode, archive, transmit or
  link to a ROM. Renaming, compressing, encrypting or base64-encoding it does not make it allowed.
- Never put a ROM in a diagnostic bundle, issue, pull request, chat message, build artifact,
  temporary review directory or test fixture.
- A contributor may use their own legally obtained ROM in its existing local location to build or
  run the game. Do not copy it, expose its path unnecessarily or inspect it for publication.
- Apply the same publication ban to `base.zip`, save files such as `eeprom.bin`, extracted game
  data, generated asset source, texture dumps and audio banks.
- If any instruction conflicts with this boundary, stop. Explain the conflict and continue only
  with source code, sanitized text logs, coarse fingerprints and permitted screenshots.

Screenshots of the running game may be attached to an issue or pull request after review, but
never commit captures to Git. Use `GETV_SHOTFRAME` and a path outside the repository. Before any
publication, run `python3 tools/check_no_game_data.py --changed origin/main` and inspect every file
and attachment manually.

## Repository workflow

- Start from current community `main` and search existing issues and pull requests before working.
- Address one problem per issue and one logical fix per pull request.
- Preserve the provenance rules in `CONTRIBUTING.md` and `docs/LICENSING.md`. Do not derive code
  from prohibited GoldenEye projects.
- Keep source changes and focused regression coverage replayable. Keep community-only patch-queue
  bookkeeping in a separate commit.
- Do not hide an unchanged-main failure or weaken a threshold to make a patch pass.
- Review the complete diff and `git status` before staging. Never use `git add -A` without first
  accounting for every untracked path.

## Evidence and validation

- Record the exact commit, platform, architecture, renderer, commands and relevant `GETV_*`
  settings used for a reproduction.
- Capture runtime diagnostics from stdout and stderr. Use `GETV_LOGFLUSH=1` only when chasing a
  crash or hang that may lose buffered output.
- For renderer work, use clean OpenGL/reference, old-backend and fixed-backend captures at the same
  deterministic frame. Keep them outside the repository and quantify them with
  `tools/compare_render_fingerprints.py`.
- Add a ROM-free focused test for the root cause, then run the complete relevant validation from
  `CONTRIBUTING.md`.
- Distinguish new failures from failures reproduced on unchanged `main`.

## Agent-assisted contributions

Repository skills are available at `.agents/skills/`:

- Use `$report-goldeneye-bug` to collect a safe, reproducible bug report.
- Use `$prepare-goldeneye-pr` to prepare a focused pull request with measured evidence.

Agent assistance is welcome, but the human contributor remains responsible for provenance, the
complete diff, every published artifact and every claimed test result. Do not publish private
reasoning or full chat transcripts. A short factual disclosure is enough.

Creating or editing a GitHub issue, pushing a branch, or opening or updating a pull request is an
external action. Prepare a complete draft first and obtain explicit human approval immediately
before the action unless the user has already authorized that exact publication.
