# Who is doing what

Two agents, one tree. This is the division and the reasoning behind it. Read
`COLLABORATION.md` for how work moves between machines.

The rule that matters: **divide by file, not by feature.** Two people editing
`port_input.c` for different reasons is the expensive case and it has already happened once.

## Surface agent

Owns Windows and the launcher interface. Both are areas the Mac cannot test properly:
Windows needs the machine, and the launcher needs someone looking at it.

**Files owned:** `getv/build_windows.ps1`, `getv/build_windows.sh`,
`tools/fetch_deps_windows.ps1`, `getv/port/src/ge_launcher.cpp`,
`getv/port/src/ge_imgui.cpp`, `getv/port/assets/**`

### Queue, in order

1. ~~**Confirm the world renders.**~~ **DONE 2026-08-24.** Diffed against the macOS
   reference: `curroom=29`, `rooms(front=0 straddle=1 BEHIND=0)`, `vtx total=326`,
   `tris submitted=908` -- all match. Verified across **all 26 loadable stages**, not just
   Bunker 1: the 20 campaign levels boot with real stan data (495-2755 tiles) and a valid
   room, and the six multiplayer arenas do the same under `GETV_MP=2`. Solo they still exit
   with the deliberate "no setup data" refusal, which is correct and unchanged.
   Three downstream symptoms went with it, untouched: `[getv][nostan]` unplaced objects
   4 → 0, the weapon/hand now renders (`hinv=1/0` was a misreading -- that is the *left*
   hand hidden, correct for a one-handed PP7), and the ~8200-frame `ITEM ENTRY CORRUPT`
   crash is gone (9000 frames clean). Record: `docs/WINDOWS_STAN_ORDERING.md`.

   The fix also had to move out of the assets: `vendor/` is gitignored, so an edit to the
   29 generated stan files neither travels nor survives regeneration. The tentative-definition
   repair now lives in `tools/uniquify_asset_symbols.py` (new `--forward-decls-only` mode,
   runs without clang) and `getv/build_linux.sh` has the two GCC flags, since it has the
   identical latent bug.

2. **Controller 1/2/3/4 bindings.** The plumbing exists: four player slots, `GETV_PADS`,
   and a per-action binding table in `port_os.c` (`GETV_BIND_FIRE` and friends). What is
   missing is per-player naming, so pad 2 can be bound independently of pad 1. Small, and it
   is what split-screen needs to be pleasant.

   ⚠️ **Ownership conflict -- not started for that reason.** `port_os.c` is listed under
   *Mac*'s owned files, and Mac's own queue item 4 (co-op movement) is described as
   "multiplayer-specific gating between reading the pad and applying movement", which is the
   same region of the same file. This is exactly the case `COLLABORATION.md` calls the
   expensive one. Surface agent needs one of: the file reassigned, the binding table moved
   somewhere Surface owns, or Mac to land co-op movement first.
3. ~~**CRT presentation mod.**~~ **DONE 2026-08-24.**
4. ~~**FXAA.**~~ **DONE 2026-08-24.**
5. ~~**Depth attachment as a texture.**~~ **DONE 2026-08-24.**

   ⚠️ **The premise of item 3 as written was wrong, and it hid a live bug.** `ss_fbo` is
   inside `#ifdef TVOS_SUPERSAMPLE`, which is defined on tvOS ONLY -- `build_windows.ps1`,
   `build_linux.sh` and `build_mac.sh` all leave it undefined. There was no offscreen target
   on the desktop at all, so there was nothing for a fullscreen shader to sample.

   Worse: `gfx_supersample` is only read from the environment inside that same ifdef, so
   **`GETV_SUPERSAMPLE` did nothing on any desktop platform** -- including behind the
   launcher's Supersampling control, which looked like it worked. The scaling of
   `gfx_current_dimensions` in `gfx_pc.c:5381` is not platform-gated, so setting the factor
   was the only missing piece.

   All three items are therefore one piece of plumbing, now in `gfx_opengl.c` under
   `GE_POSTFX` (defined when `TVOS_SUPERSAMPLE` is not): an offscreen colour texture plus a
   **depth texture** (item 5, so it can be sampled), and a resolve pass that does the
   supersample downsample, FXAA and the CRT terms in one fullscreen triangle. Entirely off
   unless gated -- with no `GETV_` set, no GL object is created and the frame path is
   byte-identical to before (verified: `tris submitted=908 drawn=329`, unchanged).

   Gates: `GETV_CRT=1` (preset, each term individually overridable via
   `GETV_CRT_SCANLINE` / `_MASK` / `_CURVE` / `_VIGNETTE` / `_LINES`), `GETV_FXAA=1`,
   `GETV_SUPERSAMPLE=1..4`. All exposed on the launcher's Video page.

   Two things worth carrying forward. Scanlines are keyed to a **virtual line count** (240 by
   default), not the output pixel grid: `sin(gl_FragCoord.y * PI)` is evaluated at fragment
   centres `y + 0.5` where `|sin|` is 1 for *every* pixel, so the obvious formulation darkens
   the whole image uniformly and reads as "too dark" rather than as a bug. And the brightness
   compensation is the exact inverse of the mean attenuation, `1/((1-scan/2)(1-2*mask/3))`,
   so CRT mode changes the texture of the image without changing its exposure.

   Measured on the HD 4400, Bunker 1, 620 frames wall-clock (60 fps cap, baseline does not
   reach it): off 13s, FXAA 14s, supersample 2x 16s, everything on 16s.

## Mac

Owns input, game systems and timing. All three are cross-platform and all three are
measurable headlessly, which is where the Mac is strongest.

**Files owned:** `getv/port/src/port_input.c`, `getv/port/src/port_os.c`,
`getv/port/src/ge_ruleset.c`, `getv/port/src/ge_lua.c`, `vendor/ge-decomp/**`, `docs/**`

### Queue, in order

1. **Frame timing.** The headline item. `GETV_SIMDIV` exists and renders every frame while
   ticking once per n; whether game time stays correct across a divider is the next
   measurement, not an assumption. See below.
2. **Start with any gun.** Belongs in the cheat system and is also how the tick gets tested:
   an automatic weapon (`ITEM_FNP90`) fires continuously, where the PP7 stops at seven
   rounds. Needs a small spawn-time hook, since `all_guns` sets a flag whose effect lives in
   the in-game turn-on switch.
3. **Measure what `aspect` does.** An afternoon and no code. If widescreen stretches a 4:3
   image rather than widening the field of view, every non-4:3 setting has been wrong since
   it shipped.
4. **Co-op movement.** Four explanations eliminated with measurements. What remains is
   multiplayer-specific gating between reading the pad and applying movement.

## Shared, so coordinate

`getv/build_windows.ps1` gets edited by both. It is small and the conflicts read clearly,
but check it first when merging.

`.gitignore` matters more than it looks. `docs/research/` was excluded through
`.git/info/exclude`, which is per-clone and does not travel, so the second machine committed
all twelve files. Anything that must stay unpublished belongs in `.gitignore`.

## The frame-timing plan

This is the one the project is judged on, so the plan is written down rather than carried
around in someone's head.

**Step 1, in progress.** `GETV_SIMDIV=n` gates the tick calls inside `lvlRender` while the
draw calls run every frame. Default 1, which is exactly today's behaviour.

**Step 1a, the next measurement.** `updateFrameCounters()` still runs every frame, so the
simulation sees one field of delta per tick while n fields of real time have passed. If
travel distance per second drops with the divider, that is the cause, and the fix is to
accumulate skipped fields into the tick frame's delta. Method: fixed real-time run, scripted
forward input, compare distance travelled at `SIMDIV=1` and `SIMDIV=2`. The harness can do
this now that it can also fire.

**Step 2, interpolation.** Without it a skipped tick redraws the previous state unchanged and
motion judders. The work is not the interpolation, it is deciding what may be interpolated:
GoldenEye stores state the AI reads as discrete fact, and blending any of it produces
erratic behaviour that is far harder to diagnose than judder.

**Step 3, the frame-quantised systems.** Fire rates, reload timing, turret delay and reaction
stepping converted from counting iterations to counting time. Open-ended, and each conversion
needs checking against retail behaviour.

**What is honest to say meanwhile:** `framerate = 30` gives a simulation at the cadence the
game was authored for, with a correct time base. That is a correct configuration, not a fix,
and the difference matters.
