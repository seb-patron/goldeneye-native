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

### 🔴 The root cause, found in the router: the policy is fine, the sensor lies to it

Evan's second capture shows the bot still wedging, and the reason is not the avoidance policy.
`ge_bot_route.c:520` picks the escape heading with:

```c
float open_h = geSenseClearestHeading(st.x, st.z, ge_br_heading, 180.0f, GE_BR_LOOKAHEAD);
```

**`geSenseClearestHeading` is a line test, and a line has no width.** The gap between the crate
and the wall passes it. So the sensor reports that gap as *the clearest heading available*, the
router commits to it for `GE_BR_AVOID_TICKS` — correctly, per the standing correction about
re-deciding every tick — and drives into a gap the body cannot enter. The commitment that stops
oscillation is what makes this grind instead of twitch.

Everything downstream of the sensor is already right. The router holds an absolute world bearing
(`atan2(dx, dz)`, `ge_bot_route.c:121`), commits to an avoidance heading rather than re-picking a
side each frame, scales forward speed down by heading error so it does not drive full-pelt into
what it is avoiding, and prefers a door over an object because a door frame reads as OBJECT. None
of that needs changing.

**So the fix is one substitution, not a rewrite:** `geSenseClearestHeading` must reject headings
whose *corridor* is narrower than `chrwidth`, not merely headings whose centre-line is blocked.
Sweep `gePortCanStandAt` along each candidate heading at the sample points `geSenseAhead` already
uses. A heading is open when a *body* fits along it, not when a ray survives it.

**Done when:** on Evan's capture the crate/wall gap is not returned by `geSenseClearestHeading`,
and the bot turns and walks around the crate instead of into the gap.

### On absolute directions, since Evan raised it

The router is already absolute — `ge_br_heading` is `atan2(dx, dz)` in world space, matching the
extractor, so "left" never enters the steering maths. That is why the fix above is a sensor change
and not a steering change.

⚠️ **The axis names matter here and have already cost three sign errors** (see the standing
correction on stick sign). In this engine the horizontal plane is **X/Z** and **up is Y** —
`prop->pos.x` / `prop->pos.z` are the ground plane and `stanGetPositionYValue` returns height. So
compass directions map to ±X and ±Z, and the vertical axis people mean when they say "the Z axis"
is **Y** here. Anything reasoning about floors, stairs or elevation is a Y question and belongs
with item 2's height work, not with horizontal routing.

The single place the absolute bearing becomes a stick deflection is
`sx = -turn * GE_BR_TURN_GAIN`. That negation is the entire "positive stick decreases heading"
convention. Keep it in exactly one place so it can only ever be wrong once.

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
## 🔴 7. THE GUN BARREL INTRO IS SCRAMBLED — cache hypothesis REFUTED by measurement

Evan captured the intro against an N64 reference. Bond and the white spotlight circle render
perfectly. **Everything that is a texture is destroyed** — monochrome, streaked into horizontal
bands, and the red at the top is gone entirely.

That split still localises it: Bond and the circle are geometry with a vertex/prim ramp and touch
no texture memory, so geometry, matrices, lighting and the combiner are all fine. The defect is
confined to texture *data*.

### What the image is made of, and this part stands

`title2.c:23`, with the decomp's own comment: the 440×299 background is far too large for the
N64's 4KB TMEM, so it is read **one 440×1 row at a time** — 299 separate `gDPLoadTextureBlock`
calls per frame, `G_IM_FMT_I` / `G_IM_SIZ_8b`, `image += 440` per row, each drawn as a
one-pixel-tall `gSPTextureRectangle`. The red comes from `gDPSetPrimColor` ramping top to bottom
under `G_CC_MODULATEI`, so the texture is a **luminance mask** and the prim colour supplies the
hue. That is why losing the texture also loses the red.

### ❌ I said the texture cache was the cause. It is not. Measured, both arms.

The hypothesis was that `gfx_pc.c:595` resets `pool_pos` without clearing `hashmap[]`, and that
299 textures a frame against `MAX_CACHED_TEXTURES` 512 was wrapping the pool mid-intro.

Raised it to 4096, rebuilt, captured frames 1350 and 1500 on both arms with `GETV_SHOTFRAME`:

```
f1350: byte-IDENTICAL     f1500: byte-IDENTICAL
```

And the profiler says why, on both arms alike:

```
[prof] f1353 | texlookup=299 hit=299 import=0
```

**299 lookups, 299 hits, zero imports.** The cache is at a 100% hit rate on the barrel frame and
is not thrashing at all. The strips are all resident and all found. Cache size is irrelevant here
and the change was reverted.

⚠️ **Where I went wrong, recorded so nobody repeats it.** I read
`[getv][texfmt] I 8b : 45793` as per-frame churn. It is a **cumulative** count since process
start, across ~185 frames of startup. The per-frame number is 299 and they all hit. A cumulative
counter read as a rate is how this cost a build.

### The `gfx_pc.c:595` defect is still real, and is now a separate, smaller item

Resetting `pool_pos` without clearing `hashmap[]` is a genuine bug in inherited sm64ex code. It
is simply not *this* bug. Worth fixing on its own terms, at low priority, with a per-frame
pool-wrap counter so it can announce itself instead of being guessed at.

### Where to look next — a lead, not a conclusion

`import=0` at the barrel frame means the strips were imported earlier and cached, so the wrong
pixels were already wrong when they went in. That points upstream of the renderer:

1. **The source pointer.** The noise in Evan's full-screen capture has glyph-like structure in the
   top band, which is what arbitrary RAM looks like read as I8 intensity. If the background asset
   is not loaded where the strip routine expects, it walks 299 rows of whatever is there. Start at
   [`docs/ASSET_LOADING.md`](ASSET_LOADING.md).
2. **`import_texture_i8`** (`gfx_pc.c:1169`) and its `ge_src_cap_1bpb` clamp.

**Do first, because it separates the two cheaply:** dump one strip's 440 source bytes at import
and compare against the same row extracted from the ROM asset offline. Equal means the renderer
is at fault; different means the pointer or the asset is, and the renderer is innocent.

⚠️ **One soft spot.** `titleRenderFolderMenuBackgroundLines` is named for the *folder select
menu*, and `sub_GAME_7F01B6E0` beside it is marked unreferenced. The intro barrel may call a third
sibling. Confirm which function the intro actually reaches before editing one.

**Done when:** the intro's barrel renders as the reference does — grey spiral on black with the
red wash at the top.

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
- **`[getv][texfmt]` counts are CUMULATIVE since process start, not per frame.** Reading 45,793
  I8 imports as per-frame churn cost a build and a wrong roadmap item. The per-frame number is in
  `[prof]` under `GETV_PROF=1`: `texlookup=299 hit=299 import=0` on the barrel.
- **The texture cache is not a suspect until `[prof]` says `import>0`.** It runs at a 100% hit
  rate on the intro. `gfx_pc.c:595` is still a real bug and still not the cause of anything seen.
