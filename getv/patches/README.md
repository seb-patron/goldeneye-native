# GoldenEye decomp changes — the ONLY copy that survives a re-clone

`vendor/` is gitignored (see `.gitignore`), so **every change the port makes to
`vendor/ge-decomp` is untracked**. Re-cloning or resetting the decomp discards all of it.
That is the same arrangement the Perfect Dark port uses (`tvos/patches/`), and the same
warning applies here:

> **Re-cloning `vendor/` loses the work unless the patch is re-applied.**

`0001-getv-port.patch` is a plain `git diff` of `vendor/ge-decomp` — 98 tracked files.

## Restore

```bash
cd vendor/ge-decomp && git apply ../../getv/patches/0001-getv-port.patch
```

## Refresh (do this before any commit that touches the decomp)

```bash
git -C vendor/ge-decomp diff > getv/patches/0001-getv-port.patch
```

## What is deliberately NOT in here

Generated data — the audio segment, the obseg blobs, the animation blobs, the images
segment, the per-model `Model.c` files. They are large, derived from the ROM, and
reproducible from `tools/` and `scripts/`; see the regeneration commands in
`docs/ROADMAP.md`. **Never commit ROM-derived data.**
