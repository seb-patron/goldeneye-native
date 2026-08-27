# Development workflow

The port is built on two machines at once: a Mac (arm64, SDL2, the reference build) and a Surface
Pro 3 running Windows (the second target, and where the launcher and netplay work happens). Most
of the friction in this project has come from keeping those two trees honest with each other, so
the rules below are worth reading before touching anything shared.

## Integration is one-directional

Work flows Windows to Mac to `main`. The Mac tree is pre-`main`: it decides what is taken,
integrates it, and owns the result building and being correct. The Windows tree publishes onto its
own branch and does not merge Mac work back in and treat that as the truth.

Only the Mac branch is a candidate for `main`, and pushing there is a deliberate, manual step
taken after review, not part of any script.

## Take by path, never by whole-tree merge

`git checkout <branch> -- <paths>`, file by file, with a note of what was not taken and why.

Whole-tree merges have twice reverted work on files the merging machine never edited:
`ge_player_api.c` lost twelve state fields once, and `gen_level_routes.py` reverted to assumed
spawns once. Selective integration by a single owner is what prevents that.

### A large negative line count is not, by itself, evidence of a revert

The two branches have **no common ancestor** -- `git merge-base` returns nothing between them -- so
`git diff --stat` reads like a mass deletion of whatever one side has and the other has never
seen. `gen_prop_extents.py` once showed as `209 --` and looked exactly like a revert. Nobody had
touched it.

The question is not "did the line count drop" but "does this file exist on the other branch at
all". `git cat-file -e <branch>:<path>` answers that directly. Compare contents only once both
sides genuinely have the file.

## `vendor/` is gitignored, so decomp changes do not travel

Nothing in a bundle carries them. This cost a full day of a Windows build that compiled and would
not link, over seven symbols that existed on one machine only.

A symbol the port layer calls belongs in `getv/patches/` the same day it is written, and the patch
is verified against the other machine's tree before it is announced. For a file under active edit
on both sides, copy the file itself and compare hashes rather than trusting a patch that claims to
apply cleanly.

Patches must also be a valid series. Two patches generated against the same base cannot both
apply in order; regenerate the later one against the tree that results from the earlier.

## Ownership and locking

| Area | Owner |
|---|---|
| `vendor/ge-decomp/**`, `port_input.c`, `port_os.c`, build scripts, docs | Mac |
| Windows build, launcher, ImGui layer, netplay, test suites, extractor tools | Windows |

Shared files -- `tools/gen_level_*.py`, `getv/port/src/ge_*_api.c` -- are fetched before being
touched. Paths are claimed before editing, not after; that is the entire discipline.

Editing a file you do not own does not stick, and should not: report the bug against the owner
rather than fixing it in place. If a shared file ends up with two competing implementations of
the same thing after a sync, **whoever owns the consumer keeps the code** -- that tiebreak needs
no round trip, and it exists because both were once dropped in favour of the other, leaving
neither.

## Transport

Git over SSH directly into Windows does not work here: the default shell is `cmd`, and the quoting
around `git-upload-pack` defeats it. Git bundles over SSH sidestep the problem entirely -- push one
side to a bundle, copy it across, pull it in on the other.

A bundle exchange should always report the diffstat **without merging automatically**, list the
other machine's uncommitted work (which never travels in a bundle, and is a frequent source of "I
fixed that already"), and verify both trees agree on commit and decomp hashes before anything is
taken. Skipping any of those three checks is how work has been silently reverted before.

## Verifying that a change is really in the build

`vendor/` being gitignored, combined with stale object files, means a source change can be absent
for hours while everything still builds and runs. Two cheap checks:

- `nm` the linked binary for the symbol you just wrote.
- Count callers. Zero callers on a function you added is the tell.

Both were learned the hard way, in both directions.
