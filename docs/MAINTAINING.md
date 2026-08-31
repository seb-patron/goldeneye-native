# Community maintenance and upstream replay

This repository is a community continuation of `SegfaultEvan/goldeneye-native`. The original
repository disappeared on August 29, 2026. Its last publicly available `main` commit is
`47fe1a1d215240adc1a3054c4ffefe780f526fe6`; that commit remains the historical upstream anchor.

Development does not need to stop while the original repository is unavailable. The goals are:

- keep this repository's `main` stable and playable;
- review, test and merge one logical bug fix at a time;
- keep every fix identifiable as a small, replayable commit; and
- make it straightforward to offer those fixes individually if the original upstream returns.

This is a maintenance workflow, not a claim of ownership or an official handoff.

## Branches and refs

| Ref | Purpose | Use as a future upstream PR? |
|---|---|---|
| `main` | Stable community build containing reviewed fixes | No; it accumulates community history |
| `fix/<short-name>` | One bug, its focused test and no unrelated cleanup | Source of the replayable fix commit |
| `play` | Optional local integration of unfinished work | Never |
| `submit/<short-name>` | A future branch created from restored upstream `main` | Yes |
| `47fe1a1` | Last known original-upstream `main` | Historical anchor only |

`main` is allowed to move forward. Do not hold a useful, reviewed fix out of the playable branch
merely because the original upstream may return later. The separation happens at the commit and
submission-branch level, not by freezing `main`.

## One-fix workflow

Start each fix from the latest community `main`:

```bash
git switch main
git pull --ff-only origin main
git switch -c fix/short-description
```

Then:

1. Reproduce one bug on the unchanged base.
2. Add the smallest source change and a focused regression test where practical.
3. Keep unrelated cleanup, documentation policy and other bugs out of the fix commit.
4. Run the relevant focused checks and the complete self-test workflow.
5. Open one pull request to this repository's `main` and link the bug report.
6. Record the replayable code commit and any dependency in [`PATCH_QUEUE.md`](../PATCH_QUEUE.md).
7. Merge only after the diff and evidence have been reviewed.

The code change and its test should form one replayable commit when practical. If the pull request
also updates the community-only patch queue, keep that bookkeeping in a separate commit. A future
upstream submission cherry-picks the code commit, not the queue update or a community merge commit.

Do not stack fixes by default. When fix B genuinely needs fix A, say so in both pull requests and
in the patch queue. Merge or submit A first. Once A lands, rebase or retarget B so its final diff
shows only B.

## Playing with work that is not ready to merge

Use a local `play` branch or a separate worktree to combine experimental fixes. The branch may be
rebuilt freely and should never be the head of a contribution pull request. Once an experiment is
understood, rebuild its final change on a dedicated `fix/*` branch.

Reviewed fixes belong on `main`, which is the normal branch for playing the maintained version.

## If the original upstream returns

First verify that the repository is controlled by the original maintainer and determine its real
current `main`. Do not assume that a newly created repository with a familiar name has the old
history. Add or repair the remote without replacing this repository's `origin`:

```bash
git remote add upstream https://github.com/SegfaultEvan/goldeneye-native.git
git fetch upstream
git log --oneline --decorate -10 upstream/main
```

Never open a pull request from the accumulated community `main`. For each entry marked
`upstream: pending` in the patch queue:

```bash
git switch --create submit/short-description upstream/main
git cherry-pick <replayable-fix-commit>
```

Resolve only incompatibilities caused by the restored upstream's newer code, rerun the complete
validation required by its current `CONTRIBUTING.md`, and inspect the full comparison:

```bash
git diff --check
git log --oneline upstream/main..HEAD
git diff --stat upstream/main...HEAD
git diff upstream/main...HEAD
```

Push `submit/short-description` to a fork of the restored repository and open one upstream pull
request. Wait for it to merge or close before preparing a dependent fix. Fetch the new upstream
`main`, create a fresh `submit/*` branch, and repeat. Community-only maintenance documents should
not be included unless the original maintainer explicitly asks for them.

If a fix no longer applies, record why instead of broadening the submission. If several fixes have
become inseparable because upstream changed the same contract, explain the dependency before
combining anything.

## Bug reports and community patches

Use the repository's [issue forms](https://github.com/seb-patron/goldeneye-native/issues/new/choose)
for reproducible build, gameplay and rendering problems. Search existing issues first and file one
problem per report. Include:

- the exact commit tested (`git rev-parse --short HEAD`);
- operating system, architecture and renderer;
- exact steps and any relevant `GETV_*` variables or command-line options;
- what happened and what should have happened;
- build counts, the first meaningful error and a short log excerpt when relevant; and
- whether it reproduces on current `main` with a clean build.

Screenshots of the running game are useful for rendering bugs. Never upload a ROM, save file,
`base.zip`, extracted asset, generated asset source, texture dump or audio bank. See
[`CONTRIBUTING.md`](../CONTRIBUTING.md) and [`LICENSING.md`](LICENSING.md) before submitting code.

A community member who can fix a reported problem is welcome to open a small pull request linked
to the issue. The same one-fix, measured-evidence and provenance rules apply.

## Maintainer checklist

- Keep `main` buildable and reserve it for reviewed changes.
- Require one bug per issue and one logical fix per pull request.
- Confirm failures against current `main` before attributing them to a patch.
- Preserve the canonical fix commit and record dependencies in `PATCH_QUEUE.md`.
- Replay queued commits in a disposable clone before describing them as independent.
- Keep ROMs, saves and extracted game data out of Git, issues and pull requests.
- Do not rewrite published history or detach this repository from its fork network casually.
- If more maintainers join, use review and branch protection rather than sharing credentials.
