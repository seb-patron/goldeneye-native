# GoldenEye decomp changes — the only copy that survives a re-clone

`vendor/` is gitignored (see `.gitignore`), so **every change the port makes to
`vendor/ge-decomp` is untracked**. Re-cloning or resetting the decomp discards all of it.
That is the same arrangement the Perfect Dark port uses, and the same warning applies here:

> **Re-cloning `vendor/` loses the work unless the patches are re-applied.**

There are two, split by *when* they can be applied rather than by subject.

| Patch | Size | Covers | Applied |
|---|---|---|---|
| `0001-source.patch` | 1.4 MB, 140 files | `src/` (131), `include/` (8), `tools/` (1) | immediately after cloning the decomp |
| `0002-assets.patch` | 212 KB, 8 files | generated asset sources | at the end of the asset pipeline |

The split exists because the eight files in `0002` do not exist in a fresh decomp — they are
produced from the ROM by the pipeline in `docs/SETUP.md` section 3.5. Applying it early fails,
and applying it before `uniquify_asset_symbols.py` gets the font symbols double-prefixed.

`0001` also creates five files that have no upstream counterpart, one of which,
`tools/gen_propdef_layout.py`, the pipeline itself calls.

## Restore

```bash
cd vendor/ge-decomp
git apply ../../getv/patches/0001-source.patch
# ... run the asset pipeline (docs/SETUP.md 3.5) ...
git apply ../../getv/patches/0002-assets.patch
```

## Refresh (before any commit that touches the decomp)

Each patch must be regenerated over its own paths. A bare `git diff` would sweep the entire
extracted asset tree into `0001` — hundreds of megabytes of ROM-derived data.

```bash
cd vendor/ge-decomp
git diff -- src include tools > ../../getv/patches/0001-source.patch
git diff -- assets/animationtable_data.h assets/font_dl.c assets/rarewarelogo.c \
            assets/font/fontBankGothic.c assets/font/fontZurichBold.c \
            assets/obseg/setup/e/UsetuplenZ.c assets/obseg/setup/j/UsetuplenZ.c \
            assets/obseg/setup/u/UsetuplenZ.c > ../../getv/patches/0002-assets.patch
```

## What is deliberately not in here

Generated data — the audio segment, the obseg blobs, the animation blobs, the images segment,
the per-model `Model.c` files. They are large, derived from the ROM, and reproducible from
`tools/` and `scripts/`; see `docs/SETUP.md` section 3.5. **Never commit ROM-derived data.**
