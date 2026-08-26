# GoldenEye decomp changes - the only copy that survives a re-clone

`vendor/` is gitignored (see `.gitignore`), so **every change the port makes to
`vendor/ge-decomp` is untracked**. Re-cloning or resetting the decomp discards all of it.
That is the same arrangement the Perfect Dark port uses, and the same warning applies here:

> **Re-cloning `vendor/` loses the work unless the patches are re-applied.**

There are three, split by *when* they can be applied rather than by subject.

| Patch | Size | Covers | Applied |
|---|---|---|---|
| `0001-source.patch` | 1.4 MB, 140 files | `src/` (131), `include/` (8), `tools/` (1) | immediately after cloning the decomp |
| `0002-assets.patch` | 212 KB, 8 files | generated asset sources | at the end of the asset pipeline |
| `0003-port-accessors.patch` | 44 KB, 1 file | `src/game/objective_status.c` | after `0001`, any time before building |

`0003` carries every accessor the port layer calls into the decomp: player position and angle,
the enemy readout, prop extents, standability and path clearance, the engine's navigation graph,
and the guard door path. See `docs/PLAYER_API.md` and `docs/ENEMY_API.md`.

It replaces the earlier `0003-enemy-shim.patch` and `0004-player-accessors.patch`, **which could
not both be applied**. Both were generated against the same base, so the second failed on a fresh
tree with `patch does not apply` and a clone got the enemy shim without the player accessors. Two
patches to one file is the trap described in the refresh notes below; one patch per file avoids
it entirely.

It is a **separate file rather than folded into `0001`** for a reason that is worth reading before
you regenerate anything.

The split exists because the eight files in `0002` do not exist in a fresh decomp - they are
produced from the ROM by the pipeline in `docs/SETUP.md` section 3.5. Applying it early fails,
and applying it before `uniquify_asset_symbols.py` gets the font symbols double-prefixed.

`0001` also creates five files that have no upstream counterpart, one of which,
`tools/gen_propdef_layout.py`, the pipeline itself calls.

## Restore

```bash
cd vendor/ge-decomp
git apply ../../getv/patches/0001-source.patch
git apply ../../getv/patches/0003-port-accessors.patch
# ... run the asset pipeline (docs/SETUP.md 3.5) ...
git apply ../../getv/patches/0002-assets.patch
```

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
> Without a repo, add changes as a **new numbered patch** instead (that is what `0003` is), or
> generate one against a saved copy of the file:
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

## What is deliberately not in here

Generated data - the audio segment, the obseg blobs, the animation blobs, the images segment,
the per-model `Model.c` files. They are large, derived from the ROM, and reproducible from
`tools/` and `scripts/`; see `docs/SETUP.md` section 3.5. **Never commit ROM-derived data.**

## 0003-port-accessors.patch: the symbols the port layer links against

`vendor/` is gitignored, so **decomp changes do not travel in a bundle**. That is not a detail: a
build once failed to link for a full day on `gePortPlayerMovePad`, `gePortPlayerAngle`,
`gePortPlayerRoom`, `gePortPlayerHealth`, `gePortPlayerWeapon`, `gePortPlayerScore` and
`gePortProbeStandable`, every one of them written, tested and committed on the other machine.
Everything compiled; only the link knew.

**If you add a symbol the port layer calls, it belongs in a patch the same day.** A commit that
builds on one machine and cannot link on the other is indistinguishable from a broken commit, and
the other side has no way to tell which.

Contains, in `src/game/objective_status.c`:

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

Apply with `git -C vendor/ge-decomp apply getv/patches/0003-port-accessors.patch`, after `0001`.

**One patch per file.** The previous split into `0003-enemy-shim` and `0004-player-accessors` was
generated twice against the same base, so only the first would apply and a fresh clone silently
got the enemy shim without the player accessors. Verify a regenerated patch by applying it to a
pristine copy and diffing the result against the working tree before trusting it.

