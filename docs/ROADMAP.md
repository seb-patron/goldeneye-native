# Roadmap

One list, both machines. Every item says what "done" means, because a task without a finish
condition gets reported as finished by whoever is tired.

Lane rules are in `docs/COLLABORATION.md`: work flows Surface → Mac → `main`, the Mac integrates
file by file. Sync with `tools/sync_surface.sh` — it pushes, pulls, prints the diffstat without
merging, and lists the other side's uncommitted work.

---

## The objective we are both working toward

**A bot completes a mission objective on Train, and the same code does it on a second level.**

Everything below is ordered by what blocks that. The CLI (`GETV_CLI=1`) is the measuring
instrument: if a person can play a level from the report, the API is complete and the rest is
policy. Today a person gets 754 units closer and then stalls, and we know exactly where.

---

## 🔴 1. CLEARANCE, NOT DISTANCE — Surface, and it is the current blocker

**Every position in the pack is a POINT and the world is made of solids.**

A crate reported as "278 away" is 278 units to its *centre*. A bot that still sees room has
already walked into the corner of it. Evan's latest capture is the sharper version of this: the
bot is not bumping a crate, it is trying to **fit through the gap between a crate and a wall**.
Two solids, and the gap between them is narrower than Bond.

That distinction matters, because prop extents alone do not fix it. Knowing the crate surface is
at 158 tells you nothing about whether what is left between that surface and the wall is wide
enough for a body. Distance is the wrong question. The question is clearance.

### 🔑 The engine already answers it exactly, and it is drivable

This is the part that changes the plan. `stan.c` carries the test the guard AI runs before every
single step it takes:

```c
s32 stanTestVolume(StandTile **tile, f32 x, f32 z, f32 width,
                   s32 cdtypes, f32 ymin, f32 ymax);
s32 stanTestLineUnobstructed(StandTile **tile, f32 x0, f32 z0, f32 x1, f32 z1,
                             s32 cdtypes, f32 height, f32 a, f32 b, f32 c);
```

`chr.c:1468` calls both back to back — line first, then volume at the destination — and treats a
**negative** return from `stanTestVolume` as "a body of `width` fits here". A non-negative return
is the index of whatever is in the way. `chraction.c:3448` runs it with
`CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PLAYERS | CDTYPE_CHRS | CDTYPE_PATHBLOCKER`.

The body width is `chrwidth`, set to `20.0f` at `chr.c:1936`. So a gap needs roughly 40 units of
clear span, and `chraction.c:4119` shows the engine's own margin: it steps out to
`chrwidth * 1.2f`.

Three reasons this beats the offline route:

- **It is drivable.** `gePortPropExtent` landed and compiles but cannot be driven, because the
  engine exposes no enumerable prop list. `stanTestVolume` needs no list — you hand it a point
  and a width and it consults the collision world directly.
- **It already knows about doors, other characters and path blockers**, which no prop-extent
  table will ever carry.
- **It has no rotation caveat.** The model bounding box is unrotated, which is why item 1 used to
  warn that a crate at forty-five degrees occupies more width than its half-extent suggests.
  `stanTestVolume` tests the real collision volume and does not care.

**Do:** add two sense primitives wrapping these — `gePortCanStandAt(x, z)` and
`gePortPathClear(x0, z0, x1, z1)` — using `chrwidth` for the querying body and the same cdtypes
mask `chraction.c:3448` uses. Then the CLI's `near` lines report **gap width**, and a follower
rejects a step before taking it instead of discovering the wall by walking into it.

**Done when:** the CLI player on Train walks past the crate it currently traps itself on, and
`gePortCanStandAt` returns false for the point between that crate and the wall.

### Prop extents are still worth having, for reporting

Emit `hx`, `hz` and `radius` per prop in the level knowledge so a reader knows what it is near and
how big that thing is. `pack_world.py` gains three floats per prop record; `GeWorldProp` gains
them. This is the human-readable half; `stanTestVolume` is the decision half.

⚠️ **Scale them.** Extents are asset-space lengths and the pack is runtime space now — a radius
left unscaled is wrong by `1/levelscale`, which is 6.7× on Train.

⚠️ **The model box is unrotated.** Report the radius as well and say which is which; do not
silently pick one.

## 🔴 2. NAVMESH NODES — Surface

Pads are prop markers, not places to walk. Measured with the teleport probe: **139 of Train's 180
nodes cannot be stood on**, and across the twenty solo levels it runs 33 to 240 each. A follower
given targets a body cannot occupy is short by however far the pad sits off the floor, always.

You already emit tile adjacency and 1,100 stairways. The tiles *are* the graph: a node per floor
tile, or per cluster, is standable by construction, has a real height, and needs no snapping.

**Done when:** routing Train produces a line through seven doors — the walkthrough says seven
brake units in a linear chain of carriages, and the CLI says every landmark is 90° to the right.
If the graph reproduces that shape it is right, and this is checkable without a bot.

## 3. THE DOOR THAT WILL NOT OPEN — either of us

The CLI player stalls against `wall door object 278 away` where `use` does nothing. Locked, needs
an objective first, or wants a closer approach? The walkthrough says Train's early doors open
normally on Agent, which points at approach distance or the use action not reaching the door.

**Done when:** a CLI player opens a door on Train and walks through it.

## 4. ENEMY FACING — Surface

`gePortEnemyFacing` refuses and `geSenseNoticedBy` falls back to line of sight, so a bot hides
from a guard facing away and strolls past one staring at it. `ChrRecord` has no facing field I
could find; `chr.c:2319` reads `chr->aimsideback` into a `yrot` when building the model matrix,
which is where the answer probably starts.

**Done when:** Train reports fewer watchers than it has guards with a clear line, and the
difference is guards facing away.

## 5. Objective completion — Mac

Nothing yet detects that an objective has been *done*. `objectiveregisters1` and the status table
exist; `GETV_OBJ_DEBUG` prints them. Without this a bot cannot know it succeeded.

**Done when:** a CLI player destroying a brake unit sees the objective flip to complete.

## 🔴 6. CONFETTI EXPLOSIONS ARE BACK — and the fix is a default nobody set

Evan is seeing the magenta/random-colour explosions again. This is not a new regression to
diagnose: it is [`docs/COLOUR_BUGS.md`](COLOUR_BUGS.md), already measured, already narrowed to
RGBA16 texel byte order, with mode 1 recorded as the mode that fixes explosions.

The reason it is "back" is that the fix never became the default:

```c
/* getv/port/fast3d/gfx_pc.c:787 */
if (m < 0) { const char *e = getenv("GETV_RGBA16BE"); m = (e && *e) ? atoi(e) : 0; }
```

`GETV_RGBA16BE` defaults to **0**, so every fresh build and every machine that has not exported
it renders explosions wrong. A fix behind an env gate that defaults off is not a fix, it is a
note. Same class of mistake as the config-template default we already wrote up in the README.

**Do:** confirm mode 1 against the Bunker 1 frame-680 capture the doc used, then make 1 the
default and keep 0 and 2 as escape hatches. If mode 1 turns out to break any other RGBA16
consumer, say which one — the doc notes the explosion flare is the only RGBA16 consumer observed
anywhere, so the blast radius should be nil.

**Done when:** a rocket explosion on a clean build with no environment set renders orange, and
the doc's magenta measurement no longer reproduces.

## 🔴 7. THE GUN BARREL INTRO IS SCRAMBLED — and the cause is a real cache bug, read in the code

Evan captured the intro against a reference. Bond and the white spotlight circle render
perfectly. **Everything that is a texture is destroyed** — monochrome, streaked into horizontal
bands, and the red at the top is gone entirely.

That split is the whole diagnosis. Bond and the circle are geometry with a vertex/prim ramp and
touch no texture memory. So the geometry, matrices, lighting and combiner are all fine, and the
defect is confined to texture upload.

### Why it streaks horizontally

`title2.c:23`, `titleRenderFolderMenuBackgroundLines`, with the decomp's own comment:

> The 440x299 8-bit background texture is far too large to fit in the N64's 4KB texture cache
> (TMEM), so instead the texture is read one 440 x 1 pixel row at a time.

So the background is **299 separate `gDPLoadTextureBlock` calls in a single frame**, each a
440×1 `G_IM_FMT_I` / `G_IM_SIZ_8b` strip, `image += 440` per row, each drawn as a one-pixel-tall
`gSPTextureRectangle`. The red comes from `gDPSetPrimColor` ramping top to bottom under
`G_CC_MODULATEI` — the texture is a luminance mask and the prim colour supplies the hue, which
is why losing the texture also loses the red.

Horizontal banding is therefore the expected failure signature: the image *is* horizontal
strips, and each strip is a separate texture that can independently get the wrong data.

### 🔑 The defect, confirmed by reading `gfx_pc.c:595`

```c
if (gfx_texture_cache.pool_pos == MAX_CACHED_TEXTURES) {
    // Pool is full. We just invalidate everything and start over.
    gfx_texture_cache.pool_pos = 0;
    node = &gfx_texture_cache.hashmap[hash];
}
```

It resets `pool_pos` and **never clears `gfx_texture_cache.hashmap[]`**. The old chains still
point into the pool, and the nodes they point at are about to be handed out again and rewritten
with different `texture_addr`, `fmt` and `siz` — while keeping the same GL `texture_id`. A later
lookup can walk a stale chain into a node that has been repurposed, and select a GL texture
holding some other row's pixels.

`MAX_CACHED_TEXTURES` is **512** (`gfx_pc.c:250`). The barrel alone wants 299 entries in one
frame, before Bond's model, the logo and everything else. The cache is never reset per frame, so
it wraps continuously and the wrap lands mid-intro.

⚠️ **This is inherited from sm64ex and is not GoldenEye-specific.** Mario 64 never asks for 299
textures in a frame, so upstream never hit it. Anything else in this port that uploads many
distinct textures per frame is exposed to the same bug.

**Do, in this order, because the first step is nearly free and settles it:**

1. Raise `MAX_CACHED_TEXTURES` to 4096 (the value the `EXTERNAL_DATA` branch already uses at
   `gfx_pc.c:247`) and re-run the intro. If the barrel comes back, the diagnosis is confirmed
   and the rest is a proper fix rather than a hunt.
2. Then fix it properly: clear the hashmap when the pool wraps, so a wrap costs re-uploads and
   never wrong pixels. `memset(gfx_texture_cache.hashmap, 0, sizeof ...)` at the reset.
3. Add a counter for pool wraps per frame and print it under the existing profiling, so this
   announces itself next time instead of being read out of a screenshot.

⚠️ Do **not** reach for `GETV_RGBA16BE` here. The barrel background is `G_IM_FMT_I` / 8b, not
RGBA16 — item 6 is a separate bug that happens to also live in texture handling. The Rareware
logo in the same sequence *is* RGBA16 and is item 6's best test frame; see
[`docs/COLOUR_BUGS.md`](COLOUR_BUGS.md).

**Done when:** the intro's barrel spiral renders as the reference does, grey spiral on black
with the red wash at the top, and a per-frame pool-wrap counter reads zero through the intro.

---

## After the objective is reached

**Bot personalities against real levels.** Eighteen archetypes tested against a placeholder.
Difficulty should mean reaction time, accuracy and whether a bot retreats.

**Netplay determinism over a long run.** Transports and discovery are in. Two peers staying
identical over thousands of ticks is unproven, and the seed fingerprint exists for exactly that.

**Horde mode.** The cheat system gives guards any weapon and the graph knows where they come
from. That is most of a wave spawner.

**Co-op verified with two humans.** `GETV_SCRIPT` moves nobody, so this needs the player API
driving slot 1 or a person.

**Frame timing step 3.** Fire rates and reaction stepping counted in time rather than iterations.

---

## Standing corrections — do not re-derive

- **`levelscale`**: runtime = asset / levelscale. The pack scales at the boundary. 19 of 20
  spawn "failures" were this.
- `player->pos` is not the world position; `prop->pos` is.
- `GETV_SCRIPT` does not move the player. Use `gePlayerPost`.
- Doors are not walls. `CDTYPE_DOORS` in a mask makes every room read as sealed.
- The tile argument to `bondviewTestLineUnobstructed` is an **input**; NULL reports everything
  obstructed. Seed it by standing at the node.
- The asker's own body sets `GE_SENSE_BODY`. Steer on `GE_SENSE_SOLID`.
- Positive stick DECREASES heading. The turn sign has been wrong three times.
- Commit to a manoeuvre. Re-deciding every tick has caused four separate oscillation stalls.
- An unset `lastknowntargetpos` is (0,0,0), a real coordinate. Report it absent.
- `vendor/` is gitignored: decomp symbols travel by patch, and `git apply` says "Skipped patch"
  on the Surface's tree, so deliver the file and verify by name.
- Run the control. Never sample a trace with `tail -1`.
- **Clearance is `stanTestVolume`, not geometry you compute.** `stan.c:2073`, negative return =
  a body of `width` fits. Body width is `chrwidth`, `20.0f` at `chr.c:1936`. The guard AI runs it
  before every step (`chr.c:1468`). Do not approximate it with parallel line samples.
- **The Fast3D texture cache wraps without clearing its hashmap** (`gfx_pc.c:595`), 512 entries.
  Any frame uploading hundreds of distinct textures can render one texture's pixels under
  another's address. Inherited from sm64ex; not a GoldenEye bug.
