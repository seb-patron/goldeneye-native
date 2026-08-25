# Vision

Where this could go, and what it would cost. `ROADMAP.md` is the short list of what is
being worked on now; this is the long arc behind it.

Everything here is scored against what the tree actually does today, because a plan that
does not distinguish shipped work from intention stops being useful about a week in. Three
labels are used, and nothing is promoted without a measurement:

- **DONE** - implemented and verified. The verification is named.
- **PARTIAL** - real code exists and does something, with a stated gap.
- **OPEN** - not started, or a config key exists that is parsed and inert.

> A config key is not a feature. `docs/CONFIGURATION.md` has a "Reserved, parsed but inert"
> section, and `ssao`, `shadows`, `per_pixel_lighting` and `muzzle_lights` are all in it.
> They are namespace, not code, and they are scored OPEN below.

## The three projects inside this one

The single most useful idea in the design notes is that this repository is really three
efforts that happen to share a source tree, and that they have different standards of
proof:

1. **Preserve.** Make the original game run correctly on modern hardware. Correctness is
   judged against the real N64, and the reference captures in `CLAUDE.md` are the ground
   truth. A change that makes the game look nicer but diverges from retail fails here.
2. **Modernise.** Add what the N64 could not do - resolution, framerate, controls, field
   of view. Judged on whether it feels right without breaking (1).
3. **Open up.** Expose the engine so other people can build things Rare never could.
   Judged on whether someone else can do it without your help.

These pull against each other, which is why they are worth naming. Mouse look is (2) and
actively wrong for (1). A Lua mod that rebalances weapons is (3) and has no bearing on
either. Keeping them separate is what lets "faithful" and "GoldenEye+" coexist instead of
arguing.

---

## Phase 1 - Native

| # | Item | Status |
|---|---|---|
| 1 | Native renderer | **DONE.** Fast3D over OpenGL via SDL2. `GfxRenderingAPI` (`getv/port/fast3d/gfx_rendering_api.h`) is a struct of ~20 function pointers and is already the platform abstraction. |
| 2 | Resolution / aspect | **PARTIAL.** `resolution`, `aspect`, `fullscreen`, `supersample` are implemented. What is not established is whether widescreen widens the *horizontal* field or stretches a 4:3 image - the notes are right that this is the distinction that matters, and it has not been measured. |
| 3 | FOV system | **DONE.** `fov` (50-160), applied at the `guPerspectiveF` call in `fr.c:711`. Two earlier attempts scaled the wrong thing, compiled, and moved triangle counts convincingly - see `ROADMAP.md`. |
| 4 | High-FPS architecture | **PARTIAL, and the most consequential open problem in the project.** See below. |

### The tick coupling, stated honestly

GoldenEye's gameplay logic is tied to its frame step. Animation, weapon fire rates, guard
reactions and turret behaviour advance per tick, so raising the frame rate does not merely
render more often, it runs the game faster. `framerate=30` therefore also sets
`GETV_TICKFIELDS=2`, because the two cannot be decoupled by asking nicely.

The correct fix is the one every mature port eventually makes: a **fixed simulation tick
with interpolated rendering**. The game ticks at its authored rate; the renderer draws as
often as the display allows and interpolates positions between ticks.

That is a substantial change and it is not free. It requires that every visually
interpolated quantity be identified, and GoldenEye stores plenty of state that must *not*
be interpolated (anything the AI reads as a discrete fact). Getting that list wrong
produces jitter that is much harder to diagnose than the original problem. The Perfect
Dark port's own experimental high-FPS support carries warnings above roughly 165 FPS,
which is a useful signal about how much of this is genuinely hard rather than tedious.

**Until that lands, "60 fps" is the honest ceiling and anything above it changes the
game.** This is the single most important thing to be straight about with players.

---

## Phase 2 - Modern controls

| # | Item | Status |
|---|---|---|
| 5 | Mouse look | **OPEN.** Needs yaw/pitch separated from the pad path, plus sensitivity, invert, raw input. |
| 6 | Modern controller | **PARTIAL.** `controls` presets exist (`xbox`, `playstation`, `switch`, numeric layouts), with `aim`, `deadzone`, `invert_look`. Dual-analog as a first-class scheme alongside the eight original styles is not done. |

The notes make one architectural point here that is worth more than the features: separate
**player orientation**, **camera orientation** and **weapon orientation**. They are the
same thing today. Splitting them is what makes third person, free camera and photo mode
possible later, and doing it early is much cheaper than retrofitting it.

---

## Phases 3-4 - Third person, free camera

| # | Item | Status |
|---|---|---|
| 7-9 | Third-person camera, animation, collision | **OPEN.** Blocked on the orientation split above. |
| 10 | Free camera / photo mode | **OPEN.** Cheapest of the four, and a good first proof that the split works. |

---

## Phase 5 - Modern visuals

| # | Item | Status |
|---|---|---|
| 11 | HD texture replacement | **OPEN.** Needs a texture database keyed by a stable identity, which the port does not yet have. |
| 12 | Filtering / mipmaps | **DONE.** `filtering` (point/bilinear/three-point, three-point being the real N64 behaviour), `mipmaps`, `anisotropic` clamped to `GL_MAX_TEXTURE_MAX_ANISOTROPY`, `msaa`, `depth_bits`. |
| 13-15 | Dynamic lighting, shadows, post-processing | **OPEN.** The config keys exist and are inert. |

**This is where a renderer abstraction decision actually gets forced**, and not before.
Shadow maps, SSAO and HDR need pipeline state and render passes that the current GL path
does not express well. That - not cross-platform reach - is the real argument for a
modern graphics library. See "On bgfx" below.

---

## Phases 6-8 - Gameplay, gadgets, AI

| # | Item | Status |
|---|---|---|
| 16-19 | ADS, manual reload, dual wield, secondary fire | **OPEN.** All are tick-coupled, so they are downstream of Phase 1 item 4. |
| 20-22 | Night vision, thermal, remote camera | **OPEN.** Night vision and thermal are post-processing over an existing frame and are the cheapest items in this range. |
| 23-25 | Enhanced guard AI, communication, personalities | **OPEN.** |

⚠️ **AI opcodes branch on render visibility** (`IFImOnScreen`, `IFMyRoomIsOnScreen`). Any
culling change presents as an AI bug - guards failing to activate, scripts stalling. This
is documented in `CLAUDE.md` and it means Phase 8 and the renderer are not independent.

---

## Phases 9-12 - Combat simulator, environment, modifiers, randomizer

| # | Item | Status |
|---|---|---|
| 26-28 | Combat simulator, bots, teams | **PARTIAL.** Multiplayer works: split screen, radar, all 64 characters, and co-op brings up 2-4 players. Bots do not exist. |
| 29-30 | Weather, environmental audio | **OPEN.** |
| 31 | Gameplay modifiers (rulesets) | **DONE.** `classic`/`hardcore`/`survival`/`chaos`/`horde` plus nine individual percentages, hooked into `lvlSetMultipliersForDifficulty()`. Measured on Agent: hardcore takes `aiHealth` 2.000 -> 1.000 and ammo 2.000 -> 1.000. |
| 32 | Randomizer | **OPEN.** |

**Rulesets were the highest return per unit of work in this document, and shipping them
confirmed why**: the game already had one function setting every value involved.
`lvlSetMultipliersForDifficulty()` (lv.c:917) assigns enemy health, damage, accuracy,
reaction, ammo, explosion and turret strength per difficulty, so a ruleset is that
function's output multiplied by a table. No level, asset or geometry is touched.
**Horde is a ruleset with a respawn rule**, not a separate mode: it attaches to dying and
spawns through the engine's own `chrSpawnAtCoord`.

Two knobs are inverted internally and the hooks compensate -- `enemy_health` divides
`g_AiHealthModifier`, which scales damage dealt *to* a guard. Getting that backwards would
have made hard mode quietly easier, which is the class of bug nobody reports.

The randomizer's own constraint is the interesting part, and the notes get it right:
randomisation has to stay *semantically valid*. "Photograph the computer" cannot be
assigned to a level with no computer. That means objectives need declared capabilities
before they can be shuffled, and that schema is the actual work.

---

## Phases 13-18 - Custom content

| # | Item | Status |
|---|---|---|
| 33 | Custom levels | **OPEN.** Hardest item here; needs the setup/stan/bg pipeline running in reverse. |
| 34-35 | Custom weapons, enemies | **OPEN.** Both are data-table driven and more tractable than levels. |
| 36-37 | Custom missions, scripting | **PARTIAL.** The scripting host exists (below); mission authoring does not. |

### Lua - DONE, as of this commit

`getv/port/src/ge_lua.c`, built optionally against Lua 5.4.7 (MIT, fetched by
`tools/fetch_lua.sh`, never vendored). Mods live in `mods/<name>/mod.lua` and may define:

```lua
function onFrame(frame)        end
function onPlayerSpawn(player) end
function onWeaponFire(weapon)  end
```

with a read-mostly `ge.*` API (`log`, `stage`, `player_count`, `player_pos`). Verified on
Bunker 1: the example mod loads, hooks fire, and `p0` reads `-632.7,189.6,1674.6` from
live game state.

Absent `liblua.a` the hooks compile to empty functions and nothing else in the tree
changes, so scripting is an addition and never a prerequisite. A mod that raises an error
is reported once and disabled, because a syntax error in somebody's mod must not look like
a crash in GoldenEye.

**What it cannot do yet** is change anything. The API is read-only by design for a first
cut. Write access - move a player, set health, spawn a prop - is the obvious next step and
is what turns this from telemetry into modding.

---

## Phases 19-20 - Co-op and online

| # | Item | Status |
|---|---|---|
| 38 | Co-op | **PARTIAL, with a known defect.** 2-4 players spawn and are separated correctly (`GETV_COOP_SPREAD`, default 60). They do not move: player 0 travels 72.5 units against 16,930 solo. Spawn collision and pad presence have both been ruled out. Cause unknown. |
| 39 | Online | **OPEN.** Needs authoritative state, client interpolation and a transport, and it is downstream of the fixed-tick work, because rollback or interpolation over a variable tick is not worth attempting. |

---

## The launcher, and "GoldenEye+" -- BUILT

`--launcher` opens it; the desktop shortcut uses it. Everything below was the plan and is
now the implementation, including the re-exec, which the measurement predicted would be
necessary.

A launcher is much cheaper than it looks, because **the entire mod
surface is already environment variables**: about 275 `GETV_*` gates plus `goldeneye.cfg`.
A launcher is a user interface over that surface, not new engine capability.

One measured constraint decides its architecture. **76 of those gates are read once into a
`static` on first use** (`static int x = -1; if (x == -1) x = getenv(...)`). A setting
changed after boot therefore does not take effect, and a launcher that toggles options in
the running process would silently do nothing for most of them.

So the launcher **sets the environment and then executes the game**, rather than poking a
live process. The cleanest form is one binary with a `--launcher` flag: it opens a window,
collects settings, then re-execs itself with the environment applied and the flag removed.
One binary to ship, one code path, and no possibility of the launcher and the game
disagreeing about what a setting means.

What it should expose, in rough order of value:

- **Profile: Faithful or GoldenEye+.** Two named presets over the existing config. This is
  what makes "we changed things" legible instead of contentious - a player who wants the
  original gets it, and everything modern lives behind one clearly labelled door.
- **Level select** - `GETV_STAGE`, which already exists and already works.
- **Cheats** - the named cheat system is implemented (`docs/CHEATS.md`); this is a
  checkbox list over it.
- **Mod packs** - `GETV_MODDIR`, pointing at a chosen mods directory.
- **Rulesets, including horde mode** - once Phase 11 exists. The launcher is where a
  ruleset gets chosen, which is another reason to do rulesets early.
- **Video and controls** - resolution, supersample, filtering, FOV, framerate, control
  preset.

**GoldenEye+ is a profile, not a fork.** Every item under it stays individually toggleable,
and "faithful" stays the default, because Phase 1's standard of proof is the real N64 and
that does not change just because Phase 2 exists.

---


## The GoldenEye+ renderer, ranked by what this codebase can actually reach

A list of modern effects is easy to write and the ordering is usually wrong for any
particular engine, because what is cheap depends entirely on what the renderer already
produces. Two measurements decide almost every row below, and both were taken against the
tree rather than assumed:

**There is already a framebuffer object with a depth attachment.** `gfx_opengl.c` creates
`ss_fbo` with a `GL_DEPTH24_STENCIL8` renderbuffer plus `ss_screen_fbo` for the supersample
path. A post-processing chain therefore has somewhere to hook, and that is most of the
plumbing for the whole of Tier 2's screen-space work.

**There are no per-pixel normals, and none are sent to the GPU.** The only normals in the
renderer are in `calculate_normal_dir()`, which serves the N64's *per-vertex* lighting.
Shading reaches the GPU as interpolated vertex colour. Any effect that needs a surface
normal per pixel needs a G-buffer that does not exist and cannot be derived from what the
game supplies.

### Already shipped

`resolution` · `supersample` · `msaa` · `anisotropic` · `mipmaps` · `filtering`
(point / bilinear / **three-point**, the last being the real N64 filter, which a generic
modern-graphics list will not think to offer) · `fov` · `depth_bits`.

So most of a "Tier 1" list is done. What genuinely remains there is **FXAA**, which is one
fullscreen pass and has no data dependencies, and **confirming widescreen widens the
horizontal field rather than stretching a 4:3 image** -- `aspect` exists and which of those
two it does has never been measured. That measurement is worth more than any new effect on
this page, because if it stretches, every wider aspect ratio is currently wrong.

### Reachable next, in cost order

| effect | why it is reachable | what it needs |
|---|---|---|
| **FXAA** | one fullscreen pass on the existing FBO | nothing new |
| **HDR framebuffer** | the FBO exists; this changes its format | float colour attachment |
| **Tone mapping** | trivial once HDR exists | one shader |
| **Bloom** | threshold, blur, composite over HDR | two extra targets |
| **Decals (bullet holes, scorch)** | the game already computes the hit position and surface -- `chrpropAddBulletHit` and the impact buffer are in the tree | a projected-decal pass |
| **Muzzle and explosion lights** | the game already tells the renderer when a gun fires and when something explodes; a light list is small | additive light pass, no normals needed if faked as radial screen-space brightening |
| **SSAO** | depth exists | the depth attachment is a **renderbuffer**, not a texture, so it cannot be sampled today. Converting it to a depth texture is the actual task; a depth-only SSAO with no normals is lower quality but real |

That table is the honest Tier 2. Note what it implies: **HDR is the foundation**, exactly as
the design notes say, and bloom/tone mapping are nearly free afterwards. The two
game-feeding effects -- decals and muzzle lights -- are attractive precisely because the
information already crosses the boundary; nothing in the game has to change.

### Blocked on data the game does not produce

- **Normal mapping, PBR, parallax, per-pixel dynamic lighting, a physically correct
  flashlight.** All need per-pixel normals. A flashlight can be faked as a screen-space
  cone, and it will look flat, because nothing in the scene can respond to its direction.
  Tier 3 is a **content** project (authoring normal/roughness maps for every texture) with a
  rendering project attached, not the other way round.
- **TAA.** Needs motion vectors. The pipeline produces none, and TAA without them is
  smearing, not anti-aliasing. This is the one item in the source list whose difficulty is
  understated: it is not "more complicated than FXAA", it is blocked.
- **Ray tracing of any kind** -- reflections, shadows, AO, GI, path tracing. These need
  Vulkan, D3D12 or Metal. On the current GL path they are not hard, they are unavailable.
  That makes RT a **renderer-choice** question rather than an effect question, and it is the
  same conversation as RT64 in `REUSE_AUDIT.md`, which is MIT, N64-aware and already has
  those backends.
- **Dynamic resolution.** Possible, but it changes the framebuffer size every time it acts,
  and framebuffer size is already known to move outcomes in this port (`CLAUDE.md` on
  supersample and heap layout). It would make every bug report irreproducible.

### The ordering this suggests

1. FXAA, and measure what `aspect` actually does.
2. Depth attachment as a texture -- unblocks SSAO and anything else that wants depth.
3. HDR, then tone mapping, then bloom.
4. Decals, then muzzle and explosion lights.
5. Stop, and decide the renderer question honestly. Everything past this point wants either
   per-pixel normals or a modern graphics API, and both are large enough that they deserve a
   decision rather than an accumulation of effects.

## On bgfx, SPIRV-Cross, and cross-platform reach

The goal behind reaching for bgfx - one graphics target instead of fighting several APIs -
is right. The conclusion does not follow, for a specific reason:

**The abstraction already exists.** `GfxRenderingAPI` is a struct of ~20 function pointers,
and a new platform means implementing them. bgfx would not replace that layer; it would sit
*beneath* it, giving N64 RDP → Fast3D → bgfx → native. That is one more layer, not one
fewer.

**And it does not remove the expensive part.** The hard work in any Fast3D backend is
`create_and_load_new_shader(uint64_t shader_id)`, which generates a shader from an N64
colour-combiner mux. bgfx knows nothing about N64 combiners, so that work is per-backend
either way. It is most of the job.

The cheaper route to the same reach:

| target | route | cost |
|---|---|---|
| Linux | the existing GL backend | **done** - builds and runs, verified on x86_64 |
| Windows | the same GL backend under SDL2 | small; the sm64ex lineage also carries a D3D11 backend if preferred |
| Android | the same `gfx_opengl.c` under GL ES 3 | moderate; the tvOS build already exercises the ES path |
| macOS/tvOS Metal | adapt libultraship's `gfx_metal.cpp` (MIT) | moderate - `CLAUDE.md` scopes the delta at ~8 signature differences |

SPIRV-Cross only earns a place if SPIR-V is being emitted, which means Vulkan, which buys
nothing these targets do not already have.

**When bgfx would be the right call:** when Phase 5 wants shadow maps, SSAO and HDR. Those
need render passes and pipeline state that the current path expresses poorly, and at that
point a modern graphics library is solving a problem that actually exists. Adopting it now
would mean rewriting a working renderer to enable features nobody has written yet, on two
platforms that already work.

---

## Suggested order

Ordered by value over cost, not by phase number:

1. **Rulesets and horde mode** (Phase 11). Numbers the game already reads; no assets touched.
2. **The launcher.** A window over a surface that already exists; makes everything else discoverable.
3. **Lua write access.** Turns the scripting host from telemetry into modding.
4. **The orientation split** (player / camera / weapon). Unblocks Phases 3, 4 and 7-10 and gets cheaper the earlier it happens.
5. **Fixed tick with interpolation** (Phase 1 item 4). The hardest item, and the one that unblocks honest high-FPS and everything tick-coupled behind it.
6. **Metal backend.** Removes the deprecated-GL risk on Apple platforms.
7. Everything else.
