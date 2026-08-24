# Surface agent: next queue

The Windows bring-up is done and the stan ordering find was the hard part of it. This is the
next block of work, in order. Everything here is owned by the Surface so it does not collide
with the Mac's lane; see `WORK_SPLIT.md`.

**Before starting anything: merge `mac-work`.** It carries the input layer (mouse look, the
trigger fix), the frame-timing decoupling, and your own stan fix propagated to Linux, which
had the same bug and nobody had noticed.

---

## 1. Controller 1/2/3/4 bindings

The plumbing exists and is half-finished. `port_os.c` has a per-action binding table
(`GETV_BIND_FIRE` and friends, defaulting to `GE_SRC_RT` for fire), four player slots, and
`GETV_PADS`. What is missing is that the bindings are **global**, so player 2 cannot be bound
differently from player 1.

Wanted: per-player binding, `GETV_P2_BIND_FIRE` style, falling back to the global when unset.
Small, and split-screen is unpleasant without it.

Verify with `GETV_PADS=2` and two real pads if you have them, or one pad plus the keyboard.

## 2. CRT presentation mod

The best first plugin, because it needs nothing from the game: no texture identity, no asset
override, no game state. It is one fullscreen fragment shader over a target that already
exists.

`gfx_opengl.c` creates `ss_fbo` with a depth attachment for the supersample path. The scene
is already rendering into a texture there; the CRT pass reads it and writes the backbuffer.

Wanted, in one shader, each independently switchable:

- scanlines, with adjustable strength and spacing
- barrel distortion, subtle by default
- a vignette
- optional slight bloom on bright pixels

Gate it behind `GETV_CRT=1` with `GETV_CRT_*` for the parameters, off by default, and expose
it in the launcher's video tab. It is a presentation mod and belongs under GoldenEye+, not in
faithful mode.

⚠️ **Do not fold this into `gfx_opengl.c` as a special case.** Build it as the first consumer
of a general post-process pass, because FXAA is the second and HD textures will want the same
plumbing. Designing the plugin path against a real consumer is the whole reason this is first.

## 3. FXAA

One more pass on the plumbing item 2 builds. No data dependencies. `GETV_FXAA=1`.

## 4. Depth attachment as a texture

`ss_depth` is a renderbuffer, so nothing can sample it. Converting it to a texture unlocks
SSAO, depth of field, and anything else depth-aware. No visible result on its own, which is
exactly why it keeps getting skipped, and it blocks the entire Tier 2 list behind it.

## 5. Windows packaging

The build produces `goldeneye.exe` plus four DLLs and a font directory. Someone who wants to
play should get one folder that works.

- a `-Target dist` that stages exe, DLLs, `assets/fonts/` including `OFL.txt`, a default
  `goldeneye.cfg` and a short README into `build-windows/dist/`
- verify it runs from a copy on a machine with no toolchain installed, which is the only test
  that means anything here

## What NOT to touch

`getv/port/src/port_input.c`, `getv/port/src/port_os.c` beyond the binding table,
`vendor/ge-decomp/**`, and `docs/` other than your own Windows documents. Those are the Mac's
lane and are being actively edited.

`getv/build_windows.ps1` is shared. It is small and the conflicts read clearly, but pull
before editing it.

## Standing rules

- Never commit generated output as a fix. The stan `extern` change was correct and was
  applied to all 29 generated files, which is lost the moment assets are regenerated and
  invisible to every other machine because `vendor/` is gitignored. It now lives in
  `tools/uniquify_asset_symbols.py`. Anything that must survive belongs in a generator.
- `docs/research/` must stay untracked. It was committed here once because the exclusion
  lived in `.git/info/exclude`, which does not travel. It is in `.gitignore` now.
- Never push to GitHub. The user does that themselves.
- Measure before and after, and use `GETV_EXIT_FRAME` so two runs are comparable.
- `GETV_CULLSTAT` is the metric that catches world-state bugs. Triangle counts are not:
  Linux looked healthy at 2658 triangles while the player stood in an empty room.
