# Roadmap

Current state, known issues, and planned work. Updated 2026-08-22.

## Where the port is

All 26 loadable stages boot, reach gameplay, render, and exit cleanly. That was verified
across 78 headless runs against a single frozen binary, with three runs per stage. There
are no known crashes, hangs, or stages that fail to start.

Multiplayer works, including split screen, the radar, and all 64 selectable characters.
The pause watch renders all five pages. Saves persist. The game runs at 60 fps with
configurable resolution and supersampling.

What has not been verified is a full playthrough: combat, AI behaviour over time,
objective completion, and finishing a level end to end. The port renders and reaches a
playable state everywhere; whether it can be played through from start to finish is
untested.

## Known issues

**Depot ground colour.** The ground on Depot renders saturated blue-cyan where it should
be near-neutral dark asphalt. The texel pattern matches the original exactly, and the
palette indices, palette offset, palette contents and vertex shade have all been measured
correct. The colour is introduced after the texel fetch, in the combiner or one of its
non-shade inputs.

**Frigate sky.** The sky renders as flat dark navy rather than blue with cirrus cloud.
The cloud display list runs and emits more commands than any other stage's, so the path is
active. Whether the geometry is drawn and invisible, or not drawn, has not been
established.

**Missing gold crest on the multiplayer character select.** The same crest renders
correctly on the file select screen, so the asset and its decode path are sound. The fault
is in how the character select screen requests it.

**Surface 2 fog density, Cradle sky gradient, Facility vent colour.** Three stages show
colouring that may or may not be correct. No reference captures exist for them, so they
are unresolved rather than confirmed defects.

**Select File background.** Renders flat black; the original has a faint circular
watermark behind the folders.

## Platform support

macOS on Apple silicon is the working target.

Windows and Linux are not yet buildable. The game layer itself is not platform-specific -
the portability gate means "not an N64" rather than "Apple" - so the work is confined to
the platform layer and the build script. `docs/PORTING.md` lists what remains, with file
references and estimates. Linux is the easier of the two.

tvOS was the original target and its build scripts remain, but it has not been exercised
recently and is not currently supported.

## Planned work

### Correctness first

The remaining known issues above are the priority.

The baseline that gates the enhancement work now exists. `tools/render_refs.py` reduces each
stage to an 8x6 grid of mean RGB and stores it in `tools/refs/render.txt`; `check` recaptures
and reports any stage that drifts. A fingerprint rather than a folder of captures, because a
screenshot of this game is ROM-derived and cannot be committed, and because 144 numbers per
stage is enough to catch a texture binding to the wrong surface, a palette going wrong or
geometry disappearing.

Run `check` before and after anything that touches the renderer. The tile-selection fix that
corrected Depot's ground is exactly the shape of change it is for: it had to be shown to alter
one stage and leave twenty-six alone, and that comparison was done by hand.

### Build order

Ordered by effort, not by appeal. Estimates are estimates; the Depot ground colour was filed as
a small rendering bug and took a full session, so treat anything involving the RDP with caution.

Each of the ten enhancement gates below already exists as a config key that parses, validates
and range-checks its value, then prints NOT-IMPLEMENTED. The naming and plumbing are done; the
work is the implementation behind them.

**A day or less each. Context and framebuffer settings, no engine changes.**

| item | notes |
|---|---|
| 24-bit depth buffer | `GETV_DEPTH_BITS`. An SDL GL attribute at context creation. The N64's z-fighting is a 16-bit depth artefact, so this is the highest ratio of visible improvement to code in the whole list. |
| MSAA | `GETV_MSAA`. Two SDL GL attributes and enabling multisample. |
| Anisotropic filtering | `GETV_ANISO`. One texture parameter where the sampler is configured. |
| Field-of-view | GoldenEye already has `viSetFovY`; Perfect Dark item 9. |

**Days. Confined to one subsystem.**

| item | notes |
|---|---|
| Per-pixel fog | `GETV_FOG_PERPIXEL`. A shader change in the Fast3D backend. |
| Positional audio | `GETV_AUDIO_3D`. The mixer exists; this is panning and attenuation from emitter positions. |
| Muzzle-flash lighting | `GETV_MUZZLE_LIGHTS`. Needs a light injected into the shade path, which is per-vertex here. |
| Mipmapping | `GETV_MIPMAPS`. Harder than it sounds: the game ships its own mip pyramid and selects LODs itself, so this has to cooperate with `TRILERP` rather than replace it. |
| State and control API | A socket exposing player, chr list and objectives, and accepting pad state. Frame-stepped so runs stay reproducible. Unlocks automated playthrough verification, which is what "full playthrough unverified" needs. |
| Two-player co-op bring-up | The engine is already N-player generic; the hook is one integer at `boss.c:489` and multiplayer spawns can be reused. Perfect Dark item 5, no Perfect Dark code required. |

**Weeks. Architecture, or breadth.**

| item | notes |
|---|---|
| VFS and asset override | Perfect Dark item 11. Without it every mod is a rebuild, so this is the mod-pack foundation rather than a mod. |
| External and HD texture packs | Perfect Dark item 14, and it depends on the VFS above. |
| Fixed simulation tick with interpolation | The real fix for frame-coupled gameplay. `framerate=30` is the current mitigation. This is the single largest correctness item left. |
| Widescreen | Perfect Dark item 16, rated large there. |
| In-game options menu | Perfect Dark item 17. Ends the config file being the only interface. |
| Multiplayer bots | Native work reusing `chraction.c`. Not a Perfect Dark import: simulants are welded to Perfect Dark's own structures. |

**Open-ended. Do not schedule these against a date.**

| item | notes |
|---|---|
| Remaining rendering defects | Frigate's sky, the multiplayer character-select crest, the Select File watermark. Each is a hunt, and the last one of these took a session. |
| SSAO, real-time shadows, per-pixel lighting | `GETV_SSAO`, `GETV_SHADOWS`, `GETV_PERPIXEL_LIGHT`. These change the look enough to need art direction, not just a flag. Per-pixel lighting departs furthest from the original, whose lighting is per-vertex. |
| Full co-op with AI, objectives and cutscenes | Perfect Dark item 18, rated very large. Bring-up above is the tractable part. |

Run `tools/render_refs.py check` before and after anything in the first three groups.

### Enhancements

Configuration keys for these are already parsed and validated. `depth_bits` and `msaa` are
now implemented; the rest currently do nothing.
Enabling one prints a notice saying so. See `docs/CONFIGURATION.md`.

The first group removes hardware limits without changing artistic intent: a wider depth
buffer (the N64's z-fighting is a 16-bit depth limitation), anisotropic filtering, MSAA,
mipmapping, and per-pixel fog. Together these are a modest amount of code and no new art.

The second group is small but visible: dynamic lighting on muzzle flashes, which currently
light nothing in dark levels, and positional audio.

The third group changes the look enough to need art direction rather than a flag:
screen-space ambient occlusion, real-time shadows, per-pixel lighting, and HD or upscaled
textures. Per-pixel lighting in particular departs furthest from the original, whose
lighting is per-vertex.

### Larger features

**Co-op campaign.** More tractable than it appears. The engine is already player-count
aware in 163 places because of split-screen multiplayer, and Perfect Dark - which shares
this engine's ancestry and is MIT licensed - shipped both co-op and counter-op. The
difficult part is level data: Perfect Dark's co-op needs second spawn points and co-op AI
lists authored per level, and GoldenEye's solo levels have exactly one spawn and none of
that. Budget it as content authoring with tool support, not as engine work.

**Online multiplayer.** The largest item here, and the only one that turns the project from
shipping software into operating a service. GoldenEye's multiplayer is local split screen
with no networking to extend, and Perfect Dark has none either, so this is new work rather
than adaptation. The architecture decision hinges on whether the simulation is
deterministic enough for lockstep; that experiment is cheap and should be run before any
netcode is designed. A direct-connect implementation with no hosted service is the variant
that keeps the project's existing shape.

**Third-person camera.** The camera system is already parameterised for several modes. The
open question is whether the third-person player model is drawn during gameplay at all;
establish that before estimating the rest.

**Keyboard and mouse.** Not yet supported.

## Contributing

The roughly 250 `GETV_*` environment gates are the practical extension surface. Each
defaults to preserving stock behaviour and can be disabled to compare against it. See
`docs/MODDING.md`.
