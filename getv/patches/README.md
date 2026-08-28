# GoldenEye decomp changes - the only copy that survives a re-clone

`vendor/` is gitignored (see `.gitignore`), so **every change the port makes to
`vendor/ge-decomp` is untracked**. Re-cloning or resetting the decomp discards all of it.
That is the same arrangement the Perfect Dark port uses, and the same warning applies here:

> **Re-cloning `vendor/` loses the work unless the patches are re-applied.**

Split by *when* they can be applied rather than by subject.

| Patch | Size | Covers | Applied |
|---|---|---|---|
| `0001-source.patch` | 1.4 MB, 140 files | `src/` (131), `include/` (8), `tools/` (1) | immediately after cloning the decomp |
| `0002-assets.patch` | 212 KB, 8 files | generated asset sources | at the end of the asset pipeline |
| `0006-fov-live-setter.patch` | 1 file | `src/fr.c` | after `0001` |
| `0007-load-trace.patch` | 1 file | asset load tracing | after `0001` |
| `0008-crosshair-color.patch` | 1 file | `src/game/gunfire.c` | after `0001` |
| `0009-freerun-divider.patch` | 1 file | `src/game/frametiming.c` | after `0001` |
| `0010-state-dump-player-position.patch` | 1 file | `src/game/objective_status.c` | after `0001` |

## The gap at 0003, 0004 and 0005 is deliberate

They were folded into `0001` the last time it was refreshed, and nobody retired them
afterwards. Applying them on top of `0001` then tried to add lines that were already there and
failed with `patch does not apply`. That is issue #4: it stopped `tools/install.sh` dead on a
fresh clone, and it had been broken for everyone who was not working from a tree that had
already been set up by hand.

Verified before removing them rather than assumed. On a fresh clone with `0001` applied,
`include/platform_info.h` and `src/game/stan.h` are already byte-identical to a working tree,
and `src/game/objective_status.c` differs only by what `0010` adds. Nothing in the three was
still reachable.

So the numbering is a record of what has existed rather than a promise that it stays
contiguous. A gap is not automatically a mistake, and a missing file is not automatically a
lost patch. Check against the scripts below before assuming either way.

## What 0001 carries beyond the decomp's own source

Most of it is the port layer's own changes to `src/` and `include/`. The part worth calling out
is the accessor surface, because it is what the port links against and it used to live in its
own patch:

- Player state behind `GePlayerState`: position (from `prop->pos`, not `player->pos`), angle,
  room from the stan tile, health, weapon, score.
- Control style helpers: `gePortPlayerControlStyle`, `gePortPlayerUsesTwoPads`,
  `gePortPlayerMovePad`.
- Navigation probes: `gePortProbeStandable`, `gePortProbeWalkable`, `gePortCanStandAt`,
  `gePortPathClear`, `gePortObstacleEdge`.
- The engine's own navigation graph: `gePortNavCount`, `gePortNavAt`, `gePortNavNeighbours`,
  `gePortNavNearest`, `gePortNavNearestPad`, `gePortNavRoute`.
- The guard door path, `gePortOpenDoorAhead`, which honours the door's key flags.
- The live enemy readout, `gePortEnemyCount` / `gePortEnemyAt` / `gePortEnemyFacing`.
- Objective state and prop extents.

See `docs/PLAYER_API.md` and `docs/ENEMY_API.md` for what each one returns.

**If you add a symbol the port layer calls, it belongs in a patch the same day.** `vendor/` is
gitignored, so decomp changes do not travel in a bundle. A build once failed to link for a full
day on `gePortPlayerMovePad`, `gePortPlayerAngle`, `gePortPlayerRoom`, `gePortPlayerHealth`,
`gePortPlayerWeapon`, `gePortPlayerScore` and `gePortProbeStandable`, every one of them written,
tested and committed on the other machine. Everything compiled; only the link knew. A commit
that builds on one machine and cannot link on the other is indistinguishable from a broken
commit, and the other side has no way to tell which.

**One patch per file.** The split that became `0003-enemy-shim` and `0004-player-accessors` was
generated twice against the same base, so only the first would apply, and a fresh clone got the
enemy shim without the player accessors. Two patches touching one file is the trap described in
the refresh notes below.

## Registration: two mechanisms, and only one of them is automatic

`tools/install.sh` and `tools/install.ps1` glob `getv/patches/0*.patch` in numeric order and
skip `0002` until the asset pipeline has run, so a patch added later is picked up on its own and
cannot be forgotten.

`tools/setup.sh` and `tools/setup-mac.sh` name each patch explicitly, so **a patch added there
must be registered by hand, in the same commit**. A patch that is committed but never applied by
a setup script is invisible: the tree builds, nothing complains, and the change simply is not
there. That has happened twice, to `0003` and then to `0007`, which is twice more than it should.

## Restore

Every patch in numeric order, with `0002` last because it needs the generated assets to exist.
Do not abbreviate this list: it used to stop after `0003`, and a tree missing `0006` links with
`gePortSetFovScale` undefined, which is issue #5.

```bash
cd vendor/ge-decomp
git apply ../../getv/patches/0001-source.patch
git apply ../../getv/patches/0006-fov-live-setter.patch
git apply ../../getv/patches/0007-load-trace.patch
git apply ../../getv/patches/0008-crosshair-color.patch
git apply ../../getv/patches/0009-freerun-divider.patch
git apply ../../getv/patches/0010-state-dump-player-position.patch
# ... run the asset pipeline (docs/SETUP.md 3.5) and the namespacing pass (3.6) ...
git apply ../../getv/patches/0002-assets.patch
```

`tools/install.sh` and `tools/install.ps1` do all of this for you.

## Refresh (before any commit that touches the decomp)

> **CHECK THAT `vendor/ge-decomp` IS A GIT REPOSITORY FIRST.**
>
> `git diff` in a directory that is not a repo prints **nothing and exits 0**. Redirecting that
> into `0001-source.patch` replaces 1.4 MB across 140 files with an empty file, and since
> `vendor/` is gitignored there is no other copy. The refresh command below is the single most
> destructive line in this repository.
>
> On at least one machine here the decomp is a plain directory rather than a clone -- the sources
> are present and patched, but there is no `.git`. Verify before running anything:
>
> ```bash
> cd vendor/ge-decomp
> git rev-parse --is-inside-work-tree || echo "NOT A REPO -- do not regenerate"
> ```
>
> Without a repo, add changes as a **new numbered patch** instead, or generate one against a
> saved copy of the file:
>
> ```bash
> cp src/game/foo.c /tmp/foo.c.orig     # before editing
> diff -u /tmp/foo.c.orig src/game/foo.c > /tmp/raw.patch
> # then rewrite the two header lines to a/src/game/foo.c and b/src/game/foo.c
> ```
>
> Whichever route, verify it round-trips onto a pristine copy before trusting it:
>
> ```bash
> patch -p1 --dry-run < ../../getv/patches/000N-thing.patch
> ```

Each patch must be regenerated over its own paths. A bare `git diff` would sweep the entire
extracted asset tree into `0001` - hundreds of megabytes of ROM-derived data.

```bash
cd vendor/ge-decomp
git diff -- src include tools > ../../getv/patches/0001-source.patch
git diff -- assets/animationtable_data.h assets/font_dl.c assets/rarewarelogo.c \
            assets/font/fontBankGothic.c assets/font/fontZurichBold.c \
            assets/obseg/setup/e/UsetuplenZ.c assets/obseg/setup/j/UsetuplenZ.c \
            assets/obseg/setup/u/UsetuplenZ.c > ../../getv/patches/0002-assets.patch
```

Regenerating by hand is how `0006` was corrupted: one context line was truncated, `git apply`
rejected it, and the tree it was meant to fix would not link. That is issue #5. Round-trip every
regenerated patch onto a pristine copy before committing it.

## Why 0002 is separate

The eight files in `0002` do not exist in a fresh decomp. They are produced from the ROM by the
pipeline in `docs/SETUP.md` section 3.5. Applying it early fails, and applying it before
`uniquify_asset_symbols.py` runs gets the font symbols double-prefixed.

`0001` also creates five files that have no upstream counterpart, one of which,
`tools/gen_propdef_layout.py`, the pipeline itself calls.

## What is deliberately not in here

Generated data - the audio segment, the obseg blobs, the animation blobs, the images segment,
the per-model `Model.c` files. They are large, derived from the ROM, and reproducible from
`tools/` and `scripts/`; see `docs/SETUP.md` section 3.5. **Never commit ROM-derived data.**
