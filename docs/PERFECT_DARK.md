# What can be adopted from the Perfect Dark port

Perfect Dark and GoldenEye 007 were built by the same studio on the same engine, and the
Perfect Dark PC port has solved a number of problems this port is still working through. That
port is MIT-licensed and may be adapted with attribution. This document is a ranked, concrete
list of what is worth taking, with file references on both sides and an honest estimate of what
each would cost.

The highest-value items are not features. They are fixes to defects in code both ports inherited
from the same source.

---

## 1. Licensing and attribution

Both Perfect Dark checkouts come from `https://github.com/perfect-dark-pc-port/perfect_dark`.

| checkout | branch | commit | date |
|---|---|---|---|
| `vendor/pd-port` | `port` (tag `ci-dev-build`) | `514bf7affd3259b7919165201342ff81a026d92c` | 2026-05-29 |
| `vendor/pd-ext` | `pr653` | `e5484dee23d1e8144d92b8b98f362869d9fd0d66` | 2025-12-02 |

Each checkout carries exactly two licence files, identical across both:

- `LICENSE` - MIT, © 2022 Ryan Dwyer.
- `port/fast3d/LICENSE.txt` - MIT, © 2020 Emill, MaikelChan. This single file covers the whole
  `port/fast3d/` directory; there is no internal licence boundary separating older Emill code
  from the libultraship-era rewrite.

Note the asymmetry with this project's own renderer, recorded in `docs/LICENSING.md`: Perfect
Dark's Fast3D carries a plain MIT notice, while the sm64ex lineage this port descends from
carries Emill's stricter pre-2021 text. Material taken from `pd-port/port/fast3d/` arrives under
the MIT notice in that directory. That does not change the status of the sm64ex-derived code
already in `getv/port/fast3d/`, and mixing the two makes the provenance of individual functions
harder to track, so adaptation sites in that directory need to be marked especially carefully.

**Two attribution gaps in Perfect Dark's own tree**, to resolve before taking anything from them:

- `port/fast3d/glad/{glad.c,glad.h}` carries no licence file and no header notice. Upstream glad
  output is normally public domain or MIT, but that is not stated here.
- `port/include/external/minimp3.h` declares CC0 / public domain inline at lines 3-7.

### Both checkouts have uncommitted local modifications

`vendor/pd-port` has **17** modified or added paths and `vendor/pd-ext` has **18**. In `pd-port`
these are this project's own tvOS work: `port/src/tvos_persist.c` and
`port/include/tvos_persist.h` are new, and `PLATFORM_TVOS` edits touch `CMakeLists.txt`,
`port/fast3d/{gfx_api.h,gfx_opengl.cpp,gfx_pc.cpp,gfx_sdl2.cpp}` and eight files under
`port/src/`.

**Cite commit `514bf7a`, not the working tree.** Several things in those directories that look
like Perfect Dark features are this project's own additions, and attributing them to Perfect
Dark would be wrong in both directions.

### Required form of attribution

Every adaptation site must carry a comment naming the upstream repository, the commit, and the
upstream file, and the adaptation must also be listed in `docs/LICENSING.md`. For example:

```c
/* Adapted from perfect-dark-pc-port/perfect_dark @ 514bf7a,
 * port/include/preprocess/common.h. MIT, (c) 2022 Ryan Dwyer. */
```

---

## 2. Bug fixes for shared-ancestry code

This is the highest-value category and the least visible. Both ports inherited the same
big-endian, 32-bit-pointer, N64-layout assumptions from the same Rare codebase. Perfect Dark's
port has already worked through most of them.

### 2.1 The structural difference between the two ports

Perfect Dark concentrates its conversions in one subsystem: `port/src/preprocess/` and
`port/include/preprocess/`, **4,871 lines across 15 files**, which transcodes each asset once at
load time, out of place, after decompression and before any game code sees it. There are two
hook points: `src/game/file.c:4157-4215` (`fileLoad` calls `romdataFilePreprocess`) and
`port/src/romdata.c:300-311` (`romdataInitSegment` calls `seg->preprocess`).

This port does the opposite, and is largely forced to. The GoldenEye decompilation extracts
assets to C source that is compiled into the binary - 1,842 files under `assets/` - rather than
loading a live ROM, so there is no single load-time funnel to hook. Conversions therefore live
inline in the game source: `src/game/bg.c:1038` (`bgBE32`) and its `BG_HDR_WORD` macro at
`:1045`, used at `:1121`, `:1190`, `:1284`, `:1310`, `:1360`; `src/game/model.c:999`
(`geAnimDescBE16`), used at `:1047` and `:1107`; the `GE_SUBWORD2/3/4` macros at
`src/bondtypes.h:25-52` with 48 uses; `romptr_t` at `src/bondtypes.h:78`. In total, 459
`GE_PORT_NATIVE` gates across 70 files.

The difference in delivery does not remove the problem. The extracted assets are still
big-endian byte arrays - `assets/ge_animation_entries_segment.c:1-7` says so in its own
generated header. **This port has Perfect Dark's problem with a different delivery mechanism**,
which is why their solutions transfer even though their pipeline does not.

### 2.2 Typed byteswap with a fail-loud default

**Effort: small. Value: high.**

Perfect Dark dispatches byteswaps on the operand's type rather than trusting the call site
(`port/include/preprocess/common.h:25-49`):

```c
static inline u32 swapUnk(u32 x) { assert(0 && "unknown type"); return x; }
#define PD_SWAPPED_VAL(x) _Generic((x), \
	f32: swapF32, u32: swapU32, s32: swapS32, u16: swapU16, s16: swapS16, \
	struct coord: swapCrd, default: swapUnk)(x)
#define PD_SWAP_VAL(x) x = PD_SWAPPED_VAL(x)
```

Companion macros `PD_CONV_VAL`, `PD_CONV_ARRAY`, `PD_CONV_ARRAY2D` and `PD_CONV_PTR` are at
`port/src/preprocess/filesetup.c:12-39`.

This port has no typed swap helper. `bgBE32` and `geAnimDescBE16` are one-off untyped readers,
so every new conversion site is a fresh opportunity to swap the wrong width with no diagnostic.
The `swapUnk` arm turns exactly that mistake into a runtime assertion. Roughly thirty lines, no
behavioural change to correct code.

**Done, 2026-08-26.** `getv/port/include/ge_typed_swap.h` (`GE_SWAP`). Attributed at the file
header and in `docs/LICENSING.md` section 6, per `perfect_dark @ 514bf7a`,
`port/include/preprocess/common.h`.

### 2.3 A unified platform header

**Done, both halves.** `vendor/pd-port/src/include/platform.h` is 100 lines defining
`PLATFORM_{WIN32,POSIX,LINUX,OSX,Nswitch}`, `PLATFORM_{X86_64,X86,arm,64bit}`,
`PLATFORM_{big,little}_ENDIAN`, `PD_BSWAP{16,32,64}`, `PD_BE{16,32,64}`, `PD_LE{16,32,64}`,
`PD_BEPTR`, `PD_LEPTR` and `PD_CONSTRUCTOR`. The `PLATFORM_TVOS` block at `:17-24` is this
project's own local modification, not Perfect Dark's.

This side never adopted that design (a single header defining every one of those macros), but
both concrete problems this section named are independently resolved:

- **The duplicate files.** `getv/port/platform.h` and `getv/port/include/platform.h` were
  byte-identical 18-line duplicates declaring only `PLATFORM_TVOS`, `sys_fatal` and `sys_sleep`.
  `f945265`/`113d33c` ("port: guard the last Apple-only include, drop the unused duplicate")
  deleted `port/include/platform.h` and kept `port/platform.h` as the one file, with its
  `TargetConditionals.h` include properly guarded behind `__APPLE__` so it does not become the
  first thing to fail a Windows or Linux build. Checked 2026-08-27: the deleted path has no
  remaining references anywhere in `getv/` or `tools/`.
- **The endianness bug.** `vendor/ge-decomp/include/platform_info.h:6-10`'s `IS_BIG_ENDIAN`/
  `IS_64_BIT` checked only `TARGET_N64`, which is also defined on native builds for unrelated
  reasons, so both were wrong on every native build - exactly the trap `src/game/lightfixture.c:73`
  and `src/game/unk_092E50.c:274` carry comments about. Fixed 2026-08-26 as
  `#if defined(TARGET_N64) && !defined(GE_PORT_NATIVE)`. It shipped as
  `0004-platform-info-native-endian.patch` and now lives in `0001-source.patch`, which absorbed
  it on a later refresh; see `getv/patches/README.md` for why 0003 to 0005 are gone.
  Those two comments' workaround (reading `words.w0`/`words.w1` by hand rather than through
  `gbi.h`'s bitfield view) stays correct and necessary regardless - it also covers a real 32-bit
  vs 64-bit word-width mismatch the endianness flag never controlled - so the comments were left
  alone rather than edited for one now-stale framing clause.

Attribute to `perfect_dark @ 514bf7a`, `src/include/platform.h`.

### 2.4 Deferred relocation of file offsets

**Effort: large. Value: highest of anything in this document.**

This is the design insight worth taking even if none of the code is.

Perfect Dark does **not** convert file offsets into machine pointers during preprocessing. It
byteswaps the 4-byte offset, zero-extends it into an 8-byte pointer-typed slot, and leaves it as
an offset (`port/src/preprocess/filesetup.c:39`):

```c
#define PD_CONV_PTR(dst, src, type) dst = (type)(uintptr_t)PD_BE32(src)
```

The base address is added later by **unmodified original N64 game code**, which is width-agnostic
because it only does `ptr += base`: `src/lib/model.c:3852-3854` defines
`#define PROMOTE(var) if (var) var = (void *)((uintptr_t)var + diff)`, used by
`modelPromoteNodeOffsetsToPointers` at `:3856-3941` and `modelPromoteOffsetsToPointers` at
`:3950-3992`. The same pattern appears in `src/lib/ultra/audio/bnkf.c:24-46`.

Nodes shared between sub-trees are handled with relocation side-tables rather than in-place
patching: `struct ptrmarker { u32 ptr_src; uintptr_t ptr_host; }` at
`port/include/preprocess/common.h:51-58`, and the richer
`struct marker { u32 src_offset; u32 dst_offset; u32 parent_src_offset; enum contenttype type; }`
at `port/src/preprocess/filemodel.c:38-43`.

**Correction, checked 2026-08-26: the premise below is stale.** This section originally said
`modelPromoteNodeOffsetsToPointers` had been reduced to a survey that returns without modifying
anything, so models do not render. Reading the live function (`model.c:8135-8143`) shows that is
no longer true: under `GE_PORT_NATIVE` it calls a real `ge_model_convert(fileramaddr, node,
"model")` rather than a no-op, and every screenshot taken this session across multiple stages
shows correctly rendered geometry with no corruption. Whether `ge_model_convert()` predates this
document or landed after it wasn't determined, but either way **this port already has a working
answer to the problem this item describes**, via a different mechanism than Perfect Dark's. The
technique below is left in place as a worked reference in case `ge_model_convert()` turns out to
have gaps the screenshots didn't exercise (it has not been read in detail against the two-pass
transcode below) - but it should not be treated as unclaimed, highest-value work without first
reading `ge_model_convert()` and finding an actual defect in it.

**Why this mattered originally, and what Perfect Dark's answer looks like.** This port's
`modelPromoteNodeOffsetsToPointers` cannot promote in place - the file slots are 4 bytes and host
pointers are 8. Perfect Dark's answer is a two-pass transcode: emit a new, larger buffer with
8-byte slots still holding offsets, record `src_offset -> dst_offset` in a marker table
(`filemodel.c` passes 1-4, `convertModel` at `:1060-1072`), resolve in pass 4 (`resolvePointer`
at `:844-852`), then let the existing unmodified promote code do the base-add. Alignment is
re-imposed per content type from a table at `filemodel.c:50-74`, applied at `:553`.

`filemodel.c` is 1,114 lines and specific to Perfect Dark's model format, so **the technique
transfers and the code does not**. But it is a proven route past the exact wall this port is
stuck at, and it is the reason Perfect Dark needed only **44 occurrences of `PLATFORM_64BIT`**
across 955 files - compared with 3,089 uses of `uintptr_t` and 534 of `PLATFORM_N64`. Deferring
relocation is what kept the 64-bit conversion from spreading through the whole tree.

Attribute to `perfect_dark @ 514bf7a`, `port/src/preprocess/{filemodel.c,common.c}` and
`src/lib/model.c`.

### 2.5 The segmented-address tag bit

**Effort: medium. Value: medium-high.**

Perfect Dark marks segmented addresses explicitly at preprocess time rather than guessing later.
When it splits each 8-byte big-endian `Gfx` into the host's 16-byte form, it sets bit 0 of `w1`
if `w1` is a segmented address - `port/src/preprocess/gbi.c:9-13` and `:175-207`
(`gbiConvertGdl`). Consumers then discriminate structurally
(`port/fast3d/gfx_pc.cpp:2263-2274`):

```c
static inline void *seg_addr(uintptr_t w1) {
    if (w1 & 1) {
        const uintptr_t seg = (w1 & 0x0f000000) >> 24;
        if (seg && segmentPointers[seg]) return (void *)(segmentPointers[seg] + (w1 & 0x00fffffe));
    }
    return (void *)w1;
}
```

This port uses a magnitude heuristic instead (`getv/port/fast3d/gfx_pc.c:5024-5031`, moved from
the `:4686-4693` this section originally cited as the port's own code grew around it): a value
below `0x10000000` with a non-zero segment nibble and a populated segment table entry is treated
as segmented.

**Calibrated honestly: the heuristic is mostly sound.** On arm64 macOS and tvOS real pointers
land above `0x1_0000_0000` and pass through correctly, so this is a robustness improvement
rather than a fix for a live crash. What it buys is diagnosis. A 64-bit pointer truncated to 32
bits is currently indistinguishable from a legitimate segment-5 address, and that ambiguity sits
underneath the most common bug class in this port. The tag bit makes it decidable.

The cost is that display lists arrive here from heterogeneous sources - compiled-in `gs*` C
initialisers such as `assets/font_dl.c`, versus raw GDLs embedded in model blobs. The raw model-
blob GDLs are still 64-bit and will need conversion regardless; that conversion is the natural
place to introduce the tag bit, and doing both at once costs little more than doing one.

**Checked 2026-08-27: still genuinely open, and item #1's supersession does not shrink it.**
`ge_model_convert()` (`model.c:7969`) - the mechanism that made item #1's deferred-relocation
transcode stale - converts the **model node graph** (`ModelNode` headers, `Parent`/`Next`/
`Prev`/`Child`/`Data`, resolved through `ge_conv_lookup()`'s offset table). It never touches a
`Gfx` command word. Whatever this port does with a model's embedded display list still goes
through the same generic `seg_addr()` every other display list uses - item #1 closed a
different, adjacent problem in Perfect Dark's own design, not this one. So the "raw model-blob
GDLs... will need conversion regardless" cost this section describes was not paid as a
side-effect of other work, and picking this up would still mean touching `seg_addr()` itself,
called on every `Gfx` word this renderer processes.

Not implemented this session. Both real evidence and real caution point the same way: this
project's own extensive live play (widescreen, HD textures, mipmaps, sky rendering, split-screen
co-op, and a fresh 46-stage sweep) has produced zero observed symptom consistent with the
ambiguity this section describes, and the fix this section proposes touches the single hottest
function in the renderer for a benefit its own text already calls diagnostic rather than
corrective. That is a real but low-urgency improvement, not a live bug - stays open rather than
implemented blind against a hot path with no reproduced failure to verify the fix against.

Attribute to `perfect_dark @ 514bf7a`, `port/src/preprocess/gbi.c` and `port/fast3d/gfx_pc.cpp`.

### 2.6 Bitfield declaration-order flipping

**Effort: medium. Value: medium.**

Perfect Dark declares bitfield groups in both orders under `#ifdef PLATFORM_BIG_ENDIAN`:
`src/include/types.h:329-339` (`struct packedpad`), `:3451-3476` (`union soundnumhack`), and in
`include/PR/gbi.h` for `Tri` (`:1037-1045`), `Gdma` (`:1322-1336`), `Gtri` (`:1341-1350`),
`Gtri4` (`:1352-1390`), `GunkC0` (`:1568-1594`) and `Gvtx` (`:1596-1612`). `Gtri4` additionally
inserts `pad2[4]`/`pad3[4]` under `PLATFORM_64BIT`. A necessary warning sits at `gbi.h:1329`:
changing signedness mid-int breaks the bitfield even on big-endian platforms.

The `GE_SUBWORD2/3/4` macros at `vendor/ge-decomp/src/bondtypes.h:25-52` are the same idea,
arrived at independently, with 48 uses. The value in Perfect Dark's set is as a **checklist of
which structs need the treatment**, particularly the `Gfx`-adjacent ones. Any adoption must be
verified with compiled `__builtin_offsetof` rather than by inspection - note that `offsetof` is
shadowed in this tree.

### 2.7 Fail-loud growth budgets

**Effort: small. Value: medium.**

`port/src/romdata.c:590-606` gives each load type a hardcoded expansion factor under
`PLATFORM_64BIT` - background 1.1x, tiles 1.1x, language 1.3x, setup 1.5x, pads/model/gun 1.7x -
and every `preprocess*File` calls `sysFatalError("overflow when trying to preprocess...")` if the
budget is undershot (`filesetup.c:1182-1184`, `filemodel.c:1083-1085`, `filepads.c:282-284`).

The constants themselves appear to be empirically tuned rather than derived, which makes them
fragile for modified assets. The transferable part is the discipline: fail loudly on overflow.
This port's `romCopy` clamp in `getv/port/src/port_assets.c` currently clamps and warns.

### 2.8 What Perfect Dark does not have

Checked so that time is not spent looking. There is **no `PD_PTR`, no `u32ptr`, no `guswap`, no
pointer-swizzling helper, and no `__attribute__((packed))` anywhere** in the tree. "Swizzle" in
Perfect Dark refers to texture swizzling (`src/game/texdecompress.c:1916,1974,1983`), unrelated.
`PD_LE16/32/64` are defined and unused. There is **no code generator**: `tools/` holds only
`assetmgr.py` (an N64-side big-endian asset packer) and the asm-processor, so all 4,871
preprocess lines are hand-written and hand-maintained. `filemodel.c:79` carries the author's own
note that the host structs there are a workaround he had not yet resolved.

Two fragilities in Perfect Dark's design that should **not** be inherited: `filesetup.c:41-103`
(`objSizeN64`) and `src/game/setuputils.c:19-93` (`setupGetCmdLength`) are two switches of about
57 cases each that must agree case-for-case with nothing enforcing it; and `PD_CONV_ARRAY2D`
(`filesetup.c:34-37`) uses `ARRAYCOUNT(dst)` for both loop bounds, which is correct only for
square matrices. This port's generated layout assertions from `tools/gen_asset_fileview.py` are
a stronger mechanism than Perfect Dark's manual padding and should be kept.

---

## 3. Renderer and platform layer

### 3.1 The Fast3D generation gap

|  | this port (`getv/port/fast3d/`) | Perfect Dark (`vendor/pd-port/port/fast3d/`) |
|---|---|---|
| `gfx_pc` | 5,515 lines, C | 2,834 lines, C++ |
| `gfx_opengl` | 968 | 1,424 |
| `gfx_sdl2` | 432 | 494 |
| `gfx_window_manager_api.h` | 25 lines, 10 methods | 54 lines, 33 methods |
| `gfx_rendering_api.h` | 36 lines, 25 methods | 61 lines, 43 methods |
| `gfx_api.h` | absent | 63 lines |

The two are different generations of the same renderer. The clearest marker is the shader
identifier: Perfect Dark's `create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1)`
(`gfx_rendering_api.h:22-23`) is the 96-bit libultraship-era form, while this port's
`gfx_rendering_api.h:14` still takes the single `uint64_t` of the 2020 sm64ex API.

What Perfect Dark has that is structurally absent here:

- **Correction, checked 2026-08-26: the paragraph below is stale.** MSAA, anisotropic
  filtering and three-point filtering were already fully implemented at the time of this
  check (`GETV_MSAA`/`GETV_ANISO`/`GETV_FILTERING`, engine + config + launcher UI); mipmaps
  was the one genuine gap and has since been implemented the same way. This port's
  framebuffer/post-processing pipeline has also grown well past a single supersampling FBO --
  it is MSAA-aware, composes with FXAA and scanline/mask effects, and resolves a multisampled
  target before the post shader runs (`gfx_opengl.c`, `pp_fbo`/`pp_color`, not the
  `gfx_opengl.c:712-760` line range this originally cited, which is now stale too). It is
  still a different shape than PD's generic `create_framebuffer`-style CRUD API, and the LRU
  texture cache with range invalidation genuinely remains absent (see below) -- but "none of
  it present here" was not an accurate description even before this check, let alone after.
- **An LRU texture cache with explicit invalidation - resolved, checked 2026-08-27, not just
  restated.** `gfx_texture_cache_lookup` (`gfx_pc.c:610-651`) keys on `(addr, fmt, siz)` with
  no range-invalidation path, same shape PD's version has one for. But PD's own reason to need
  it does not carry over: their `addr` is a live RDRAM address that gets reused as different
  ROM segments load in and out of a fixed pool over a session, so the same key can validly mean
  different content at different times. This port's assets are individually compiled C arrays
  (section 2.1's "different delivery mechanism"), and `G_SETTIMG`'s address arrives through
  `seg_addr(cmd->words.w1)` (`gfx_pc.c:5368`), which resolves an N64 segmented address straight
  to that texture's own compiled-in array -- a permanent, unique, process-lifetime address, not
  a slot that later holds something else. The scenario range invalidation exists to handle
  cannot occur here. Not a gap; a problem that does not exist for this port's asset pipeline.
- **A widescreen aspect system** - `gfx_adjust_x_for_aspect_ratio` (`gfx_pc.cpp:1037-1041`) and
  `gfx_update_aspect_mode` (`:1636-1660`), driven by custom GBI commands
  `G_ASPECT_{wide,left,right,center}_EXT` that the game emits per element.
- **Display-mode enumeration, fullscreen modes, refresh-rate query and swap-interval control**
  in the window-manager API.

### 3.2 The boundary on renderer adoption is narrower than "do not cross-port"

Perfect Dark's `Vtx` is **12 bytes with an indexed colour** (`include/PR/gbi.h`, typedef ending
at `:999`): position, flags, a `colour` byte, and `s`/`t`. `gfx_sp_vertex` resolves it through a
separate colour table loaded by `gSPColor`. GoldenEye's `Vtx` is the standard 16-byte
`union { Vtx_t v; Vtx_tn n; long long force_structure_alignment; }` with inline RGBA and normals.

So `gfx_sp_vertex`, `gfx_sp_tri*` and the lighting inner loop are genuinely incompatible and
must not be cross-ported. **Everything below the vertex format is vertex-format-agnostic**: the
framebuffer subsystem, the texture cache and its invalidation, the filtering and MSAA and
anisotropy paths, the aspect-ratio module, the window-manager API, and the GL backend's shader
generator. That is a narrower exclusion than a blanket prohibition, and it does not disturb the
settled decision to keep this port's own Fast3D - the point is selectively lifting subsystems
into it, not replacing it.

### 3.3 The custom GBI extension layer

**Effort: small (as reference). Value: medium.**

`vendor/pd-port/src/include/gbiex.h` (328 lines) documents the Rare-family GBI that GoldenEye
also uses: `gSPTri4`/`Tri3`/`Tri2`/`Tri1` (opcode `G_TRI4` = `0xB1`, twelve 4-bit indices),
`gSPColor` (`G_COL`), and `gDPLoadTLUT06`. `gfx_pc.cpp` dispatches `G_TRI4` at `:2340` and
carries a full extension block at `:2334-2523` covering `G_EXTRAGEOMETRYMODE_EXT`,
`G_SETTIMG_FB_EXT`, `G_SETGRAYSCALE_EXT`, `G_SETINTENSITY_EXT`, `G_SETSUBPIXELOFFSET_EXT`,
`G_FILLRECT_WIDE_EXT`, `G_TEXRECT_WIDE_EXT`, `G_IMAGERECT_EXT`, `G_SETFB_EXT`, `G_COPYFB_EXT`,
`G_INVALTEXCACHE_EXT`, `G_RDPFLUSH_EXT` and `G_CLEAR_DEPTH_EXT`.

This well has been partly drawn from already - `getv/port/fast3d/gfx_pc.c:5264` handles
`G_TRI4` and `:5382` handles `G_LOADTLUT` (moved from `:4930-4943`/`:5048` as the port's own
code grew around them), both with comments citing Perfect Dark. `vendor/pd-port/src/rsp/gsp.s`
is an annotated copy of the same microcode GoldenEye runs and remains the primary source for
microcode semantics questions.

**Checked 2026-08-27, empirically: none of the other thirteen extension opcodes are a live
gap.** `gfx_pc.c`'s unhandled-opcode path (added after `G_TRI4` hid silently the same way -
every unimplemented opcode used to be discarded with no counter and no log) prints one line
per distinct opcode per process. Swept every valid solo stage ID (400 frames each, six known
MP-only IDs skipped, a handful of other IDs crash on solo load and are the same already-
documented "eleven ids can never load" category, not an opcode finding) and collected every
line it printed. The only opcodes it ever logs, across the whole sweep, are `0xE6`-`0xE9` -
already identified in that same code as RDP sync commands with no meaning without a real RDP
pipeline. None of `G_COL`, `G_EXTRAGEOMETRYMODE_EXT`, `G_SETTIMG_FB_EXT`, `G_SETGRAYSCALE_EXT`,
`G_SETINTENSITY_EXT`, `G_SETSUBPIXELOFFSET_EXT`, `G_FILLRECT_WIDE_EXT`, `G_TEXRECT_WIDE_EXT`,
`G_IMAGERECT_EXT`, `G_SETFB_EXT`, `G_COPYFB_EXT`, `G_INVALTEXCACHE_EXT`, `G_RDPFLUSH_EXT` or
`G_CLEAR_DEPTH_EXT` fired even once. GoldenEye's own asset data, across the whole campaign,
never emits any of them - `gbiex.h` documents opcodes the shared microcode *can* produce, not
opcodes this game's own toolchain *does* produce.

**This rules out one specific theory about the untextured-prop, splayed-limb and CI8/TLUT
defects** - a missing GBI opcode handler - not the defects themselves, which remain open and
still need their own investigation. Scope honestly: solo campaign only (multiplayer setups
were not swept), 400 frames per stage rather than a full playthrough, so an opcode used only in
a late-game or MP-only moment would not have been caught. Given the sweep already covers every
level's opening minutes across the whole campaign, where the large majority of a level's asset
variety loads, that residual gap is judged small.

### 3.4 Non-renderer platform layer

Perfect Dark's `port/src/` is 16,049 lines against this port's 8,100. Files with no counterpart
here:

| file | lines | what it provides |
|---|---|---|
| `port/src/optionsmenu.c` | 2,042 | In-game options built on the game's own `menuitem`/`menudialogdef` system: resolution, fullscreen, vsync, framerate limit, MSAA, filtering, anisotropy, field of view, crosshair, screen shake, stick speed and deadzone, and fully rebindable input with four binds per action and per-player profiles |
| `port/src/config.c`, `include/config.h` | 320 + 21 | An ini config where modules self-register via `configRegisterInt/UInt/Float/String(key, &var, min, max)` from `PD_CONSTRUCTOR`, so there is no central table to keep in sync |
| `port/src/crash.c` | 384 | Signal handler using `backtrace()`, `backtrace_symbols()` and `dladdr()`, with a Darwin PC extraction at `:299-302` (`ucontext->uc_mcontext->__ss.__pc`), `SA_ONSTACK`, and handlers for SIGSEGV/SIGABRT/SIGBUS/SIGILL |
| `port/src/mod.c`, `mod.h` | 533 + 19 | Asset override: replacement textures, animations, sequences and per-stage music from a `modconfig.txt` |
| `port/src/fs.c`, `fs.h` | 302 + 27 | A VFS with `$S`/`$E`/`$H` path-prefix expansion and a mod-directory search order |
| `port/src/libultra.c` | 508 | A much fuller libultra shim |

On the last row: Perfect Dark implements the complete Controller Pak suite (`osPfsAllocateFile`,
`osPfsChecker`, `osPfsDeleteFile`, `osPfsFileState`, `osPfsFindFile`, `osPfsFreeBlocks`,
`osPfsInitPak`, `osPfsIsPlug`, `osPfsNumFiles`, `osPfsReSizeFile`, `osPfsReadWriteFile`), EEPROM,
VI (`osViSetMode`, `osViSetXScale`, `osViSetYScale`, `osViSwapBuffer`, `osViBlack`), threads and
the Rumble Pak. This port's `getv/port/src/port_os.c` has 23 symbols and nothing beyond
`osPfsInit`. GoldenEye uses both families: EEPROM is already handled in
`getv/port/src/port_save.c:235-305`, but `osPfs{Init,IsPlug,GetStatus,GetInitData,RequestData,
Checker}` are called from `vendor/ge-decomp/src/motor.c` and `src/joy.c`.

**Effort:** crash handler small - one file, and the Darwin path is already written. Config system
small. VFS small to medium. `osPfs*` medium. Options menu large: 2,042 lines tightly coupled to
Perfect Dark's menu structs, while GoldenEye's `front.c` is a different implementation of the
same idea, making this a rewrite with reference rather than a port. Framebuffer subsystem and
filtering large. Widescreen `G_ASPECT_*_EXT` large, because it needs game-side emission at every
2D draw site.

### 3.5 Field of view and widescreen

**Effort: small for FOV. Large for full widescreen.**

The lineage here is confirmed shared. Perfect Dark has `viSetFovY`, `viGetFovY`,
`viSetFovAspectAndSize` and `camSetPerspective` (`src/include/lib/vi.h:44-48`,
`src/include/game/camera.h:9`). GoldenEye has the same functions under the same names -
`vendor/ge-decomp/src/fr.c:903` (`viSetFovY`), `:919` (`viGetFovY`), `:922` (`viSetFov`) -
feeding `guPerspectiveF(g_viProjectionMatrixF, &g_viPerspNorm, g_ViBackData->fovy,
g_ViBackData->aspect, ...)` at `fr.c:710`. GoldenEye already applies `WIDESCREEN_ASPECT` at
`src/game/bondview2.c:8444,8463,8464`.

So Perfect Dark's FOV slider (`optionsmenu.c:1334`) maps onto an existing, identically named
GoldenEye API. The projection is the easy half; the hard half of widescreen is 2D HUD element
placement, which is what the `G_ASPECT_*_EXT` machinery exists to solve.

### 3.6 tvOS renderer fixes: cross-validation rather than opportunity

The local modifications in `vendor/pd-port` contain three tvOS/EAGL fixes that are **already
independently present** in this port's Fast3D. Recording them because agreement between two
independent solutions is worth something:

- Forcing `gl_es = true` at init because SDL's UIKit backend misreports the profile mask -
  here via `USE_GLES` at `getv/port/fast3d/gfx_opengl.c:27` and `gfx_sdl2.c:206-217`.
- Adopting SDL's real screen FBO because EAGL has no default framebuffer 0, and rebinding SDL's
  renderbuffer before `SDL_GL_SwapWindow` or `presentRenderbuffer` fails with
  `GL_INVALID_OPERATION` - here at `gfx_opengl.c:718-720` and `:914-928`.
- Casting `float(three_point_filter)` because GLSL ES 3.0 has no `mix()` overload taking an
  integer interpolant. This one is latent here: it only bites once three-point filtering exists.
  Worth carrying forward if the filtering work in 3.1 is adopted.

### 3.7 What `pd-ext` adds

`pd-ext` is **older** than `pd-port` (2025-12 against 2026-05) and is a fork rather than a
superset - its `gfx_opengl.cpp` is smaller, 1,337 lines against 1,424. A directory comparison
shows the entire delta is: new `port/src/ext_tex.c` (468 lines), `port/include/ext_tex.h` and
`port/include/external/stb_image.h`, plus edits to `gfx_{api.h,opengl.cpp,pc.cpp,pc.h,
rendering_api.h}`, `glad/`, `input.{c,h}`, `video.h`, `fs.c`, `main.c`, `mod.c`, `optionsmenu.c`,
`pdmain.c`, `romdata.c` and `preprocess/{filemodel.c,gbi.c,gbi.h}`.

So `pd-ext` is worth consulting for exactly two things: **external and HD texture packs**
(`ext_tex.c`, keyed by `(type, id, texnum)` behind a `gfx_external_textures_enabled` runtime
toggle) and the **mouse-look input work** in the `input.c` and `optionsmenu.c` deltas. Everything
else should be taken from `pd-port`, which is six months newer.

---

## 4. Co-op and Counter-op

The conclusion here is not the one the framing anticipates, so the evidence comes first.

### 4.1 GoldenEye's engine is already N-player generic

There is one player array, not a solo path and a multiplayer path
(`vendor/ge-decomp/src/game/player.c:10-11`):

```c
struct player *g_playerPointers[4];
struct player_data g_playerPlayerData[4];
```

`MAX_PLAYER_COUNT` is 4 (`src/bondconstants.h:2249`). There is one initialiser and it takes a
count - `init_player_data_ptrs_construct_viewports(s32 playercount)` at `player.c:81-103`, which
loops `initBONDdataforPlayer(i)` over the count. `getPlayerCount()` (`player.c:105-115`) simply
counts non-NULL entries. `g_CurrentPlayer` (`player.c:16`) is a swapped pointer.

The hook is a single unguarded integer (`vendor/ge-decomp/src/boss.c:478-489`):

```c
localSelectedNumPlayers = 0;
if (g_StageNum != LEVELID_TITLE) {
    localSelectedNumPlayers = 1;
    if (get_selected_num_players() >= 2) { localSelectedNumPlayers = get_selected_num_players(); }
}
init_player_data_ptrs_construct_viewports(localSelectedNumPlayers);
```

**There is no solo-versus-multiplayer branch here.** Solo stages get one player only because
`get_selected_num_players()` returns less than 2 in solo mode. Force it to 2 on a solo stage and
the engine constructs two `struct player`s and two viewports.

The split-screen machinery is player-count-driven rather than mode-driven:
`src/game/bondview2.c:8225-8330` computes viewport width, height and origin, branching on
`getPlayerCount() >= 2` at `:8273` and `== 2` at `:8319`, feeding `set_cur_player_screen_size`,
`set_cur_player_viewport_size`, `viSetViewSize` and `viSetViewPosition` at `:8473-8478`, with
per-player aspect at `:8444-8469`. Around ten further `getPlayerCount() >= 2` gates in the same
file cover HUD, damage display and scenario logic. `g_playerPointers[...]` is dereferenced 407
times across the tree, and the character and prop layers already resolve which player is
involved generically through `getPlayerPointerIndex(prop)` (`src/game/chr.c:2729`,
`chraction.c:2437`, `propobj.c:1658,1676,13204`, `chrprop.c:1947,2129,2788`).

**So most of the engine-generic machinery - split-screen viewports, multiple player structs,
per-player camera and HUD - GoldenEye already has, and none of it is gated behind a multiplayer
flag.**

### 4.2 The level data: one spawn per solo level, no exceptions

Spawn pads are populated at `vendor/ge-decomp/src/game/bondview_r.c:207-226` from
`INTROTYPE_SPAWN` records into `g_Startpad[16]` and `startpadcount`
(`src/game/bondview2.c:523-526`), with each record filtered by
`check_ramrom_flags() == ((struct SetupIntroSpawn *)intro_record)->is_demo_playback`.

Counting live records (`is_demo_playback == 0`) across the setup files in
`vendor/ge-decomp/assets/obseg/setup/` and `.../setup/u/`:

|  | solo setups | multiplayer setups |
|---|---|---|
| files | 21 | 17 |
| live spawns | **exactly 1 in every one** | 0, or 5 to 8 |

All 21 solo setups - `arch, ark, azt, cave, control, cryp, dam, depo, pete, run, sevb,
sevbunker, sevx, sevxb, crad, dest, jun, len, silo, statue, tra` - have exactly one live spawn.
Several have three or four spawn records in total, but the extras are all attract-mode spawns
with `is_demo_playback == 1`. Multiplayer setups with real spawns: `ame` 8, `ark` 8, `ash` 5,
`cave` 7, `crad` 8, `cryp` 6, `dish` 5, `imp` 5, `oat` 8, `ref` 7, `sevb` 5, `statue` 8, `arch`
8; `dam`, `depot`, `dest` and `run` have none.

The premise is confirmed. **The consequence is smaller than it sounds**, for three reasons:

1. **A spawn is an integer, not a data structure.** `bondview_r.c:213` reads
   `g_Startpad[n] = &g_CurrentSetup.pads[record->index]` - the record is an index into the
   level's existing pad list, and those lists are already large: 160 to 537 pads per solo level
   (dam 464, arch 537, cave 486, ark 448, sevx 437, control 430, depo 408). Authoring co-op
   spawns means choosing about 21 pad indices near the existing start. That is a spreadsheet,
   not a content pipeline.
2. **Zero authored data still boots.** `bondviewGetRandomSpawnPadIndex`
   (`src/game/bondview.c:1235-1382`) cycles `pad_index = counter % startpadcount`. With
   `startpadcount == 1` both players get pad 0 and spawn co-located - ugly, but not a crash. A
   crude two-player co-op is reachable before any level data is authored.
3. **Five solo stages get spawns for free.** Solo and multiplayer setups coexist for `ark`,
   `cave`, `cryp`, `sevb` and `arch`, with 5 to 8 multiplayer spawns each. Those pad indices are
   directly reusable.

This port has also already done spawn-spreading work on solo-derived data:
`src/game/bondview.c:1257-1285`, the `GETV-NOSTAN`/`GETV-MP` block, with a recorded observation
that on Complex with the fallback-only guard, players 2 and 3 both landed on pad 6.

### 4.3 Where Perfect Dark's co-op code actually lives

Counting case-insensitive `coop` across `vendor/pd-port` - 994 hits in 87 files:

| area | hits | files | character |
|---|---|---|---|
| `src/setups/` | **568 (57%)** | 23 | per-level AI scripts |
| `src/game/` | 329 (33%) | 35 | engine |
| `src/assets/*/lang` | 46 | 18 | menu strings |
| `src/include/` | 27 | 6 | types and constants |
| `src/lib/` | 17 | 4 | engine |
| `port/` | 7 | 1 | port layer |

Engine-generic is about 38%; per-level data about 57%. The heaviest engine files are
`mainmenu.c` 75, `endscreen.c` 33, `player.c` 29, `chraction.c` 26, `propobj.c` 23,
`gamefile.c` 23, `chraicommands.c` 17.

The raw split understates the difficulty, because of **what** those 568 references are. They are
not spawn coordinates. They are AI script logic. From `vendor/pd-port/src/setups/setuplee.c`:

```
812:   set_target_chr(CHR_COOP)
1390:  if_chr_death_animation_finished(CHR_COOP, /*goto*/ 0x2d)
1391:  if_chr_in_room(CHR_COOP, 0x00, 0x003a, /*goto*/ 0x06)
2434:  if_chr_dead(CHR_COOP, /*goto*/ 0x2c)
2684:  set_chr_chrflag(CHR_COOP, CHRCFLAG_HIDDEN)
```

`CHR_COOP` is `0xf5` (`vendor/pd-port/src/include/constants.h:471`), alongside `CHR_P1P2` `0xf2`
and `CHR_P1P2_OPPOSITE` `0xf1`. **Rare authored second-player awareness into every guard script
and every objective script, per level.**

GoldenEye has no such vocabulary. Its entire special-character set is at
`vendor/ge-decomp/src/bondconstants.h:4696-4707`: `CHR_BOND_CINEMA -8`, `CHR_CLONE -7`,
`CHR_SEE_SHOT -6`, `CHR_SEE_DIE -5`, `CHR_PRESET -4`, `CHR_SELF -3`, `CHR_OBJECTIVE -2`,
`CHR_FREE -1`. There is no `CHR_COOP`, no `CHR_P1P2`, and not even a generic `CHR_BOND` target -
GoldenEye's AI addresses the player implicitly. (`CoopyObjectRecord` at `bondtypes.h:3609` is a
misspelling of "copy" and is unrelated.)

### 4.4 Assessment

**Nearly free**, because GoldenEye already has it: the player array, per-player structs,
split-screen viewport geometry and aspect, per-player camera and HUD, per-player control
profiles, the `getPlayerCount() >= 2` gates, spawn selection with separation logic, and one
unguarded call site at `boss.c:489` to force the count. **Small to medium** for a technically
functional two-player co-op that boots on a solo stage.

**Must be authored by hand:**

- About 21 second-spawn pad indices, 16 stages needing new ones and 5 able to reuse existing
  multiplayer spawns. **Small**, but each needs playtesting.
- **Every guard's reaction to player 2.** This is the large, irreducible item. GoldenEye guards
  have no target-selection concept beyond "the player". Supporting a second Bond means either
  extending the AI command vocabulary with a `CHR_COOP` equivalent and then editing per-level AI
  lists - Perfect Dark's 568 script references are the scale marker - or a generic engine-level
  nearest-player substitution, which is far cheaper and will behave wrongly in every scripted
  set-piece. **Large to very large.**
- **Objective and cutscene logic.** GoldenEye objectives assume one Bond, and intro and exit
  cutscenes use `CHR_BOND_CINEMA` with a single third-person model. **Medium to large per level.**
- Front-end plumbing to offer co-op at all, in `front.c` mission select. **Medium.**

**Counter-op is strictly harder.** It needs guard-possession machinery on top of all of the
above, and GoldenEye has no equivalent of `CHR_P1P2_OPPOSITE` targeting.

Co-op that *runs* is more accessible than the Perfect Dark entanglement suggests, because
GoldenEye's engine never took the solo shortcut. Co-op that *plays well* is a level-design
project rather than a porting project.

---

## 5. Other findings

**Perfect Dark's SIMD mixer work is already partly adopted.**
`getv/port/audio/ge_mixer.c:34-44` and `vendor/pd-port/port/src/mixer.c:12-25` carry the same
`__SSE4_1__` / `__ARM_NEON` dispatch block. This port's file header correctly notes that Perfect
Dark's mixer uses Rare's N-Audio ABI and would not fit wholesale, but the SIMD kernels for the
shared primitives were taken. Perfect Dark has SIMD paths at 18 sites (`mixer.c:191,197,219,222,
230,281,383,433,600,602,610,634,645`) across `aADPCMdec`, `aResample`, `aEnvMixer` and `aMix`.
Any of those still scalar here is remaining headroom on Apple silicon. **This adoption is
currently unattributed and should be marked.**

**Perfect Dark's `docs/` is design documentation, not engineering** - `ailists.md`, `chrs.md`,
`challenge7bug.md`, `piracychecks.md`. `ailists.md` is nonetheless the best prose explanation of
the AI-list system GoldenEye shares through `chraidata.c` and `bondaicommands.h`, and is worth
reading before any work in section 4.

**The crash handler's Darwin path is already written** - `port/src/crash.c:299-302` extracts the
PC via `ucontext->uc_mcontext->__ss.__pc`, with `SA_ONSTACK` and `backtrace_symbols()`. Given
that `mempAllocBytesInBank` currently fails by spinning in `while(1);` - a silent hang that
raises no signal - and that block-buffered stdout loses the last kilobyte before a crash, the
discipline in that file is worth more than the code: install early, log the PC, write to a fixed
buffer.

---

## 6. Ranked shortlist

| # | item | effort | value | attribute to |
|---|---|---|---|---|
| 1 | ~~Deferred-relocation model transcode~~ - **stale, see §2.4**: `ge_model_convert()` already solves this a different way | n/a | n/a | superseded |
| 2 | ~~`_Generic` typed byteswap with fail-loud default~~ - **done, 2026-08-26**: `getv/port/include/ge_typed_swap.h` (`GE_SWAP`), verified standalone (all typed arms round-trip correctly, the default arm asserts on an unhandled type). Not yet used at any real conversion site - see the header's own "what this does not do" note. | n/a | n/a | done, `514bf7a` `port/include/preprocess/common.h` |
| 3 | ~~Unified `platform.h` for endian, arch and bswap~~ - **done, both halves, checked 2026-08-27**: the duplicate `port/platform.h`/`port/include/platform.h` files were merged (`f945265`, mac's work), and the `platform_info.h` endianness bug both named as the reason to want one header was fixed 2026-08-26 (then `0004-platform-info-native-endian.patch`, since absorbed into `0001-source.patch`). Neither side adopted PD's single-header design wholesale, but both concrete problems section 2.3 raised are gone. | n/a | n/a | done, `514bf7a` `src/include/platform.h` |
| 4 | Crash handler with Darwin PC and backtrace | small | high | `514bf7a` `port/src/crash.c` -- **not applicable to Windows**, checked 2026-08-26: it is POSIX `backtrace()`/`dladdr()`/signal-based, none of which exist there. A Windows equivalent needs SEH/`MiniDumpWriteDump`, which is a different design PD's own doc does not address. Still real work for whoever owns the Mac build. |
| 5 | ~~Two-player co-op bring-up~~ - **stale, checked 2026-08-26**: already done. `boss.c:501-518` already reads `gePortCoopPlayers()` (`GETV_COOP=<n>`) and forces the count; see `docs/COOP.md`, whose own opening line is "Co-op movement works... playable." | n/a | n/a | superseded |
| 6 | Segment tag bit for model-blob GDLs | medium | medium-high, but honestly diagnostic not corrective | `514bf7a` `port/src/preprocess/gbi.c`, `port/fast3d/gfx_pc.cpp` -- **checked 2026-08-27, stays open**: `ge_model_convert()` (item 1's supersession) converts the model node graph, never a `Gfx` word, so it does not shrink this - the raw-GDL conversion this section says would be "the natural place" for the tag bit was never built as a side effect of other work. Not implemented: touches `seg_addr()`, the hottest function in the renderer, called on every `Gfx` word, for a benefit its own text calls diagnostic rather than corrective, against zero reproduced failure across this session's extensive live play. See section 2.5 for the full reasoning. |
| 7 | ~~Config system with self-registering modules~~ - **not worth it, checked 2026-08-26**: `ge_config.c` is a real, working 1375-line config system with 53 key handlers, and every feature added this session (widescreen, HD textures, FOV, anisotropic, MSAA, mipmaps) went through it without friction. PD's design avoids a central table; this one has a central table and no measured pain from it. Replacing 1375 working lines to avoid a downside that has not actually cost anything here is a worse trade than the status quo. | n/a | n/a | not applicable |
| 8 | ~~`gbiex.h` as the opcode checklist for `gsp3D` gaps~~ - **checked 2026-08-27, empirically no live gap**: swept every valid solo stage ID and collected every distinct unhandled opcode `gfx_pc.c` logged across the whole run. Only `0xE6`-`0xE9` fired (already-documented RDP sync opcodes, no real pipeline to sync). None of `gbiex.h`'s other 13 extension opcodes fired even once - GoldenEye's own assets never emit them. Rules out "missing GBI opcode" as the explanation for the untextured-prop/splayed-limb/CI8-TLUT defects; those stay open on their own, unexplained by this. | n/a | n/a | done, GoldenEye-native measurement, no PD code adopted |
| 9 | ~~Field-of-view slider~~ - **stale, checked 2026-08-26**: already done. `GETV_FOV` (`fr.c:711-725`, applied to the projection argument, not the stored fovy, so aim-zoom scaling is untouched), a config key (`ge_config.c:700`) and a real launcher slider (`ge_launcher.cpp:1674`) all exist. Verified live: FOV 100 vs 160 produces a large, real difference in the rendered frame. | n/a | n/a | superseded |
| 10 | ~~Fail-loud growth budgets in asset conversion~~ - **not applicable, checked 2026-08-26**: PD's version verifies a preprocessing size-multiplier estimate, which assumes PD's preprocess/ pipeline this port does not have (section 2.1). The nearest analog, `port_assets.c`'s `romCopy` clamp, already does the right thing for what it actually guards -- an over-long read that real N64 hardware would harmlessly satisfy from adjacent cartridge space. Converting it to a hard abort, which is what "fail loud" would mean here, trades a benign, hardware-accurate clamp for a crash. Not a gap to close. | n/a | n/a | not applicable |
| 11 | ~~VFS and mod / asset-override system~~ - **mostly covered, checked 2026-08-26**: `ge_lua.c` (1233 lines) is a working mod-scripting host, and this session's own hash-based HD-texture-pack system (`ge_texpack_dir`, `fs_walk`/`fs_load_file` in `port_support.c`) is a working asset-override VFS with its own path-prefix resolution -- different syntax than PD's `$S`/`$E`/`$H`, same job. Not a byte-for-byte match to PD's design, but not an open gap either. | n/a | n/a | mostly covered |
| 12 | ~~`osPfs*` Controller Pak suite~~ - **not worth it, checked 2026-08-27, this time conclusively**: `osPfsChecker` -- not a libultra call GoldenEye makes, a function retail GoldenEye's own source *defines*, `vendor/ge-decomp/src/joy.c:173-176` -- unconditionally `return PFS_ERR_INCONSISTENT;`, no hardware access at all. `joyRumblePakInit` right below it calls `osPfsInit` and only proceeds past `PFS_ERR_ID_FATAL`/`PFS_ERR_DEVICE`. Controller Pak support is disabled in the **retail 1997 game source**, independent of anything a port provides -- a real `osPfs*` implementation would be observably identical to the current stub, because GoldenEye's own logic never reaches the code that would exercise it. Not "unclear value" as this row previously said; zero value, provably. | n/a | n/a | not applicable |
| 13 | ~~Bitfield order-flip audit against Perfect Dark's set~~ - **done, checked 2026-08-26**: audited every bitfield-packing struct in `vendor/ge-decomp` (a dozen or so, not hundreds -- most textual bitfield-syntax hits were ternaries, not real bitfields). No live, unhandled endian bug found: this codebase has already independently invented and applied the fix multiple times (`GE_NIB`/`GE_TRIPLE_NIB` in `stan.c`, a named-field fix in `image.c`, a direct-assignment fix in `lv.c`), and every other candidate is dead code, single-field (no cross-field order to get wrong), or self-consistent by construction (written and read by the same native compiler, so bit-order is irrelevant). One real finding: `stan.h` had a set of doubly-wrong, unused macros duplicating the pre-`GE_NIB` bug -- removed, see the commit. | n/a | n/a | done, GoldenEye-native, no PD code involved |
| 14 | ~~External and HD texture packs~~ - **done, GoldenEye-native, checked 2026-08-27**: not a port of `ext_tex.c`, but the same job by a different mechanism - `configHDTextures`/`GETV_TEXPACK` (`port_support.c`) content-hashes each N64 texture and checks a pack directory for an override before upload (`ge_texpack_try_override`, `gfx_pc.c:770`), off by default and a no-op with no pack present. `GETV_TEXPACK_DUMP` extracts a baseline pack from the game's own textures. Full launcher UI: checkbox and pack-folder field (`ge_launcher.cpp:1710-1711`). | n/a | n/a | done, GoldenEye-native, no `pd-ext` code involved |
| 15 | ~~Framebuffer subsystem, three-point filtering, mipmaps, anisotropy, MSAA~~ - **mostly stale, checked 2026-08-26**: three-point filtering, anisotropy and MSAA were already fully implemented (engine + config + launcher UI) before this row was checked -- none of it from Perfect Dark, three-point cites mupen64plus-libretro directly in the source. Mipmaps was a genuinely open gap, a `key_todo_flag` placeholder with no consumer; implemented directly (`glGenerateMipmap` + mipmap `GL_TEXTURE_MIN_FILTER` variants), same treatment as the other three. **Texture-cache range invalidation resolved 2026-08-27, not needed** -- see the detailed section above; this port's compiled-in assets mean the address-reuse scenario it exists for cannot occur here. **Still genuinely open**: only the generic framebuffer subsystem (this port has its own post-processing FBO pipeline already, a different shape than PD's `create_framebuffer`-style CRUD, and no current use case needs PD's version). | n/a for the done/resolved parts; medium if the generic framebuffer CRUD is ever wanted | n/a | done and resolved; framebuffer CRUD GoldenEye-native if ever picked up, no PD code needed |
| 16 | Widescreen `G_ASPECT_*_EXT` | large | medium | `514bf7a` `port/fast3d/gfx_pc.cpp:1636-1660`, `src/include/gbiex.h` |
| 17 | ~~In-game options menu~~ - **small, honest version done, checked 2026-08-27**: PD's `optionsmenu.c` shape (a real settings page reachable in-game) does not map cleanly onto this port -- `front.c` (9,451 lines) has no reusable menu-item abstraction, and the one genuinely data-driven settings surface, the Watch's `game_options_entries[]` (`options.c`), is blocked by an explicit maintainer warning on `WATCH_NUMBER_SCREENS` ("do not change this value until player struct is fully shiftable") that a new settings page would have required violating. Built the subset that is honestly live without touching that struct: F9 (vsync), F5/F6 (FOV -+10%, clamped 50-160%), same F-row convention as the pre-existing F11 fullscreen toggle, all in `gfx_sdl2.c`'s `gfx_sdl_onkeydown()`. `gePortSetVsync`/`gePortGetVsync` (`port_support.c`) and `gePortSetFovScale` (`fr.c`, `0006-fov-live-setter.patch`) do the real work; both reuse apply paths that already existed (`configWindow.settings_changed`, and the per-frame projection recompute in `viSetupCurrentPlayerView`) rather than adding new ones. Deliberately deferred: window-size/resolution cycling, and everything else PD's menu has that this port cannot make honestly live without a restart (widescreen, filtering, HD textures, anisotropic, mipmaps, MSAA, supersample, framerate -- all one-shot `getenv`/constructor reads). Not a Watch page, not PD code. | n/a | n/a | done (small scope), GoldenEye-native, no PD code involved |
| 18 | Full co-op: AI, objectives, cutscenes | very large | high | GoldenEye-native authoring; Perfect Dark `src/setups/` is the scale reference |

---

## 7. Confidence

Every claim above carrying a file and line reference was verified by reading the source in this
working tree. The following are inference or judgement rather than verified fact, and are marked
as such so they are not mistaken for measurements:

- That Perfect Dark's growth-factor constants in `romdata.c` were empirically tuned rather than
  derived. The values are consistent with tuning; no derivation is documented.
- That this port's mixer SIMD coverage is incomplete relative to Perfect Dark's 18 sites. The
  dispatch blocks were confirmed to match; the individual kernels were not diffed.
- All effort estimates. These are judgement calls informed by line counts and coupling, not
  measurements.

The claims about GoldenEye's co-op-relevant engine structure in section 4.1 and the spawn counts
in section 4.2 were derived from `vendor/ge-decomp` source directly. The claims about Perfect
Dark's engine-side co-op internals in section 4.3 are counted rather than read in full.
