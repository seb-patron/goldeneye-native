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

1. **Confirm the world renders.** The `.data` ordering fix is in. Diff the culling stats
   against the macOS reference in `WINDOWS_HANDOFF.md` and confirm they match rather than
   just confirming it looks right.
2. **Controller 1/2/3/4 bindings.** The plumbing exists: four player slots, `GETV_PADS`,
   and a per-action binding table in `port_os.c` (`GETV_BIND_FIRE` and friends). What is
   missing is per-player naming, so pad 2 can be bound independently of pad 1. Small, and it
   is what split-screen needs to be pleasant.
3. **CRT presentation mod.** The best first plugin, because it needs nothing from the game:
   `gfx_opengl.c` already creates `ss_fbo` with a depth attachment for supersampling, so
   scanlines, barrel distortion and a vignette are one fullscreen fragment shader over an
   existing target. Doing it first means the mod plugin system gets designed against a real
   consumer instead of in the abstract.
4. **FXAA.** One more pass on the same plumbing while it is open.
5. **Depth attachment as a texture.** Currently a renderbuffer, so it cannot be sampled.
   Converting it unlocks SSAO and anything else depth-aware. No visible result on its own,
   which is exactly why it keeps getting skipped.

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
