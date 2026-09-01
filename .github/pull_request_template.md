## Problem

<!-- Link one issue and describe the observed and expected behavior. -->

Fixes #

## Change

<!-- Explain the root cause and the smallest change that fixes it. -->

## Evidence

<!-- Give exact reproduction steps and before/after measurements or screenshots when useful. -->

<!-- For renderer changes, include OpenGL/reference, old-backend and fixed-backend screenshots.
Generate the quantitative rows with:
python3 tools/compare_render_fingerprints.py --format markdown --reference REF.bmp "Before=OLD.bmp" "Fixed=NEW.bmp"

| Comparison with reference | Worst channel delta | Mean channel delta | Channels over tolerance |
| --- | ---: | ---: | ---: |
| Before | | | |
| Fixed | | | |
-->

## Validation

<!-- List exact commands and results, including build counts and pre-existing failures. -->

- [ ] Focused regression test or check:
- [ ] Complete self-test workflow:
- [ ] Relevant platform build:
- [ ] `git diff --check`:
- [ ] `python3 tools/check_no_game_data.py --changed origin/main`:

## Agent assistance

<!-- State material agent assistance briefly and say what the human reviewed and ran. Do not paste
private reasoning or a full transcript. Write "None" when no agent assisted. -->

- Assistance:
- Human review and verification:

## Upstream replay

<!-- Identify the code-and-test commit only. Keep PATCH_QUEUE bookkeeping separate. -->

- Replayable fix commit:
- Depends on:
- Community-only changes in this PR:

## Submission checks

- [ ] This pull request addresses one logical bug.
- [ ] I checked current `main` and existing issues/pull requests for duplicate work.
- [ ] I reviewed the complete diff and removed unrelated cleanup and generated files.
- [ ] No ROM is included in any form. I did not add, copy, stage, commit, upload, attach, paste,
      encode, archive or link to one.
- [ ] No save, `base.zip`, extracted asset, generated asset source, texture dump or audio bank is
      included or linked.
- [ ] The implementation follows the provenance rules in `CONTRIBUTING.md` and
      `docs/LICENSING.md`.
