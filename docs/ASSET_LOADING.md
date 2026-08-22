# Level asset loading — map, holes, and plan

Investigation only. Nothing in this document has been changed in the tree.
Written 2026-08-19 by `getv-assets`. All paths relative to repo root unless absolute.

**Read §0 first.** The headline is not what I expected going in.

---

## 0. The headline

> **Almost every FUNCTION on the level-load path is already real code. The holes are
> DATA and TWO NON-COMPILING TRANSLATION UNITS, not missing logic.**

I checked 32 functions on the path against `getv/port/src/ge_link_stubs.c`. **Zero of
them are stubs.** The only stubbed symbols on the path are data:

| symbol | kind | why it is stubbed |
|---|---|---|
| `setup_text_pointers` | `char *[59]` table | defined in `src/game/chraidata.c`, which **fails to compile** |
| `g_GlobalAILists` | AI list table | same file |
| `bg_*_all_p_seg` × 33 | bg segment base addresses | **no such symbol exists anywhere** — see §3 |
| `UsetuparchZ`, `UsetuplenZ` | setup files | their asset `.c` fails to compile |
| `g_Startpad`, `startpadcount`, `gBondViewCutscene`, `g_CameraLookAtBondPad`, `dword_CODE_bss_80079A14/18/1C`, `flt_CODE_bss_80079A00…10` | setup-pad / intro-camera state | defined in `src/game/bondview2.c`, which **fails to compile** (9 errors, all `-Wint-conversion`) |

Three of the three asset kinds behave completely differently in this port, and that is
the single most important thing to internalise:

| kind | on the N64 | in this build | status |
|---|---|---|---|
| **stan** (collision tiles) | 1172-packed ROM blob | **decompiled C with real pointers**, `assets/obseg/stan/*.c`, 29 files, all compile, all link | effectively ready |
| **setup** (objects/AI/pads) | 1172-packed ROM blob | **decompiled C with real pointers**, `assets/obseg/setup/*.c` (+ `e/ j/ u/` variants), 50 objects built, 2 TUs fail | 🟡 nearly ready, 2 gaps + a variant-selection hazard |
| **bg** (room geometry) | raw segment DMA'd + per-room 1172 chunks | **decompiled C with real pointers, NOT LINKED AT ALL, and structurally incompatible with the loader** | the real work |

The 1172 decompressor is **not on the stan or setup path at all** in this build, and is
only on the bg path for per-room chunks (§3.4).

---

## 1. Call graph — "user picks a level" → data in memory

Verified by reading the sources; line numbers are `vendor/ge-decomp/`.

```
front.c  (menu)  -> selected_stage
   |
   v
lv.c:341  lvlStageLoad(stage)
   |
   +-- texReset()                                  image.c        [real]
   +-- load_font_tables()                                         [real]
   |
   +-- lv.c:438  load_bg_file(g_CurrentStageToLoad)   ---------- BG + STAN
   |     |
   |     |  bg.c:794
   |     +-- scan levelinfotable[] (bg.c:183) for levelID -> levelentry_index
   |     |     entry gives: bg_seg_filename  "bg/bg_sev_all_p.seg"
   |     |                  bg_stan_filename "Tbg_sev_all_p_stanZ"
   |     |                  levelscale, visibility
   |     +-- lightFixtureInitTables()
   |     |
   |     +-- bg.c:824  obLoadBGFileBytesAtOffset(bgname, header[16], 0, 0x40)
   |     |      ob.c:150/206  fileGetIndex(name) -> file_resource_table[] index
   |     |                    romCopy(target, &fileentry->hw_address[offset], len)
   |     |                    port_assets.c:123 romCopy = memcpy + range guard
   |     |      GUARDED BY  `if (rom_size != 0)`  -- rom_size is 0 for every bg
   |     |                  file (see §3.2), so THIS CALL IS A SILENT NO-OP
   |     |
   |     +-- bg.c:829  ptr_bgdata_room_fileposition_list =
   |     |                 BG_SEG_TO_PTR(ptr_bg_data, ((s32*)ptr_bg_data)[1])
   |     +-- bg.c:832  size = ((room_fileposition_list[1].pPointTableBin & 0xffffff) - 1 | 0xf) + 1
   |     +-- bg.c:833  ptr_bg_data = mempAllocBytesInBank(size, MEMPOOL_STAGE)
   |     +-- bg.c:834  obLoadBGFileBytesAtOffset(bgname, ptr_bg_data, 0, size)
   |     |
   |     +-- bg.c:836  gptr_stan = _fileNameLoadToBank(stanname, 2, 0, 4)   ----- STAN
   |     |      ob.c:196  -> fileIndexLoadToBank(fileGetIndex(name), ...)
   |     |      ob.c:233  GE_PORT_NATIVE shortcut: gePortObsegSize(hw)==0
   |     |                => "this is native linked C data", return hw_address
   |     |                   directly, skip allocate + inflate.  CORRECT for stan
   |     +-- bg.c:838  stanDetermineEOF(gptr_stan, 0, gptr_stan)   stan.c:3097
   |     +-- bg.c:839  stanLoadFile(gptr_stan)                     stan.c:476
   |     |                 -> stanBuildRoomData()                  stan.c:246
   |     |                    walks tiles by list_of_tilesizes[pointCount&0xf]
   |     +-- setLevelScale / setDebugCameraScale / visibility
   |     +-- bg.c:857+ portal + envdata offset->pointer promotion (BG_SEG_TO_PTR)
   |
   +-- lv.c:518  init_load_objpos_table()          initobjects.c:43   [real]
   +-- lv.c:521  init_guards()
   +-- lv.c:523  proplvreset2(stage)   prop.c:1216   -------------- SETUP
   |     |
   |     +-- prop.c:1239  setup_text_pointers[stageId]        STUB (NULL object)
   |     +-- prop.c:1253  synthesise "Usetup<lvl>Z" or "Ump_setup<lvl>Z"
   |     +-- prop.c:1266  g_ptrStageSetupFile =
   |     |                  _fileNameLoadToBank(name, FILELOADMETHOD_DEFAULT, 256, MEMPOOL_STAGE)
   |     |                  -> same ob.c:233 native-data shortcut  CORRECT for setup
   |     +-- prop.c:1269  langLoadToAddr(...)
   |     +-- prop.c:1275-1300  rebase all 10 stagesetup segment offsets
   |     |                     onto the RAM copy: (u32)base + (u32)field
   |     +-- prop.c:1303+  in-place offset->pointer promotion of
   |     |                 waypoint.neighbours, waygroup.neighbours/waypoints,
   |     |                 AIListRecord.ailist, PathRecord.waypoints, ...
   |     +-- object/prop instantiation (setupGetPtrToCommandByIndex, loadobjectmodel.c)
   |
   +-- lv.c:530  init_path_table_links()   initpathtablelinks.c  (reads g_CurrentSetup)
   +-- lv.c:546  bondviewLoadSetupIntroSection()  bondview_r.c:75
   |
   v   (per frame, once the level is running)
bg.c:2252  bgLoadRoomVtxData(room, dst, len)
bg.c:2299  bgLoadRoomPrimaryGdl(room, dst, allocsize)
bg.c:2359  bgLoadRoomSecondaryGdl(room, dst, allocsize)
      each: obLoadBGFileBytesAtOffset(bgname, scratch, fileoffset, size)
            -> bgDecompress(scratch, dst)      bg.c:2239   [1172 / DEFLATE]
            -> texCopyGdls + texLoadFromGdl    tex.c:779
```

---

## 2. Function table — real vs stub, and what a real implementation owes

`stub?` column checked mechanically against `getv/port/src/ge_link_stubs.c`.

| function | file | stub? | state / what it still must do |
|---|---|---|---|
| `lvlStageLoad` | lv.c:341 | real | already instrumented with `gePortBootMark` at every call — use it |
| `load_bg_file` | bg.c:794 | real | **must be rewritten for the native path.** Currently reads a 0x40 file header into a stack array and derives everything from segmented offsets. With linked C bg data there is no file and no offsets. See §5 step 4 |
| `obLoadBGFileBytesAtOffset` | ob.c:150 (`!LEFTOVERDEBUG`) / ob.c:206 | real | **fails silently.** Guarded by `if (rom_size != 0)`; `rom_size` is 0 for every bg file, so it returns without touching `target` and without any diagnostic. Its out-parameter is left as caller stack garbage. This is exactly the "stub that ignores its out-parameter" failure mode, except it is real code |
| `fileGetIndex` | ob.c:423 | real | linear `strcmp` over `file_resource_table`; if not found, tries indy (stubbed) and returns 0. **Returning 0 is indistinguishable from index 0** |
| `fileIndexLoadToBank` | ob.c:232 | real | carries the `GE_PORT_NATIVE` shortcut that makes stan/setup work. Correct as-is |
| `_fileNameLoadToBank` | ob.c:190 | real | thin wrapper |
| `load_resource` | ob.c:33 | real | `romCopy` + `decompressdata`; huft workspace already sized in ENTRIES |
| `resource_load_from_indy` | ob.c:106 | real | dead path natively (`indycomm*` are stubs) |
| `romCopy` | port_assets.c:123 | real | memcpy + images-segment range guard |
| `decompressdata` | decompress.c:32 | real | 1172: 2 magic bytes then raw DEFLATE |
| `obInit` | ob.c:117 | real | sizes every resource via `gePortObsegSize()`; returns 0 for anything not in `ge_obseg_sizes.c` (which is every stan/setup/bg symbol — verified, 0 matches) |
| `stanDetermineEOF` | stan.c:3097 | real | relocates every pointer by `delta = (s32)newBase - origBase`. Called as `(gptr_stan, 0, gptr_stan)`, so `delta` = **truncated 64-bit pointer**, and it then adds that to already-correct pointers. Must be `delta == 0` on the native path. Also **not idempotent** — it writes into linked `.data`, so a second level load re-relocates |
| `stanLoadFile` | stan.c:476 | real | already carries a port fix (`stan_prefix = file` instead of the half-pointer write) |
| `stanBuildRoomData` | stan.c:246 | real | walks tiles by `list_of_tilesizes`. Depends on tile-to-tile adjacency in the linked object — **verified to hold**, see §4.1 |
| `bgDecompress` | bg.c:2239 | real | **`u8 buffer[0x2100]` passed as `struct huft *`.** This is the exact bug that was fixed in `ob.c`, `image.c` and `music.c` and it is still live here. `struct huft` is 16 bytes on arm64 vs 8 on N64, so this buffer holds **half** the entries `inflate()` will carve out of it, unchecked, on the **stack**. Expect `__stack_chk_fail` → SIGABRT with no usable backtrace the first time a room streams. Fix: `struct huft buffer[GE_HUFT_ENTRIES];` (`src/inflate/inflate.h:30`) |
| `bgLoadRoomVtxData` | bg.c:2252 | real | offset arithmetic **already fixed** to wrap in `u32` |
| `bgLoadRoomPrimaryGdl` | bg.c:2299 | real | 🟠 offset arithmetic **not** fixed: `(s32)((u8*)pPriMappingBin + ptr_bg_data) - ptr_bg_data`. Truncates through `s32`. Works only by accident while the field holds a 32-bit segmented value |
| `bgLoadRoomSecondaryGdl` | bg.c:2359 | real | same, `pSecMappingBin` |
| `texLoadFromGdl` / `texCopyGdls` | tex.c:779 | real | |
| `texLoad` | image.c:2381 | real | writes `*(uintptr_t*)updateword` — an **8-byte** store through an `s32*`. See §4.4 |
| `texLoadFromDisplayList` | image.c:2322 | real | 🟠 `texLoad((u32*)((s32)bytes + 4), ...)` — `(s32)bytes` truncates a 64-bit pointer |
| `proplvreset2` | prop.c:1216 | real | the setup loader. Blocked only by `setup_text_pointers` |
| `setupGetPtrToCommandByIndex` | loadobjectmodel.c:253 | real | |
| `init_load_objpos_table` | initobjects.c:43 | real | |
| `init_guards` / `init_path_table_links` | initguards.c / initpathtablelinks.c | real | consume `g_CurrentSetup` |
| `bondviewLoadSetupIntroSection` | bondview_r.c:75 | real | but its collaborators live in `bondview2.c`, which does not compile |
| `mempAllocBytesInBank` | memp.c | real | OOM = `while(1)` → **silent hang**, wrapped in `GE_MEMP_DIE` — do not remove |
| `setup_text_pointers` | chraidata.c:816 | **STUB** | `void *setup_text_pointers = 0;` — an 8-byte object. `setup_text_pointers[stageId]` for stageId up to 58 reads **464 bytes past it** into whatever the linker put next. Guaranteed garbage, possibly a non-NULL garbage pointer that passes the `&&` guard |
| `g_GlobalAILists` | chraidata.c | **STUB** | 256 KB of 0xFF |
| `g_Startpad`, `startpadcount`, `gBondViewCutscene`, `g_CameraLookAtBondPad` | bondview2.c | **STUB** | intro camera + spawn pad |

### Non-compiling TUs on this path (from `getv/build/failing.txt`)

| file | errors | kind | difficulty |
|---|---|---|---|
| `src/game/chraidata.c` | 71 | 49× `pasting formed '_(' `, 11× "too many arguments to function-like macro", plus 4 syntax + `unknown type name 'Switch'` | **preprocessor/macro DSL**, not a 64-bit problem. The AI-command macros. Mechanical but fiddly |
| `src/game/bondview2.c` | 9 | all `-Wint-conversion` (`s32` ↔ pointer) | **easy** — the same `(s32)ptr` family already triaged in ROADMAP §"The `(u32)`/`(s32)` pointer-cast class" |
| `assets/obseg/setup/UsetuparchZ.c` | 2 | `pasting formed '_('` / `')_'` | macro paste, same family as chraidata |
| `assets/obseg/setup/{u,e,j}/UsetuplenZ.c` | 2 | `CreditsEntry (*)[272]` into an `s32` field; "initializer element is not a compile-time constant" | a **pointer stored in a 32-bit asset field** — this one is a genuine layout problem, not a typo |

---

## 3. The bg problem, in detail (this is the actual work)

### 3.1 What the assets actually are — VERIFIED

`assets/obseg/bg/bg_sev_all_p.c` begins:

```c
struct bg_header header = {0, &room_data_table, &portal_data_table, &global_visibility_commands, 0};
struct room_data_table_entry room_data_table[] = {
    {0},
    {&point_table_binary_1, &pri_mapping_binary_1, 0, -758.0, 189.0, -2027.0},
    ...
```

Real C, real `&` pointers. On the N64 build these were compiled and linked through
`assets/obseg/bg/bg_all_p.ld`, which places `.data` at **`0x0F000000`** — so every
pointer inside the linked segment became a `0x0Fxxxxxx` **segmented address**, and the
game's `BG_SEG_TO_PTR` strips that tag by 32-bit wraparound. That is where the whole
segment model comes from.

### 3.2 Why bg is not connected — VERIFIED with `nm`

- `file_resource_table.inc.c:6` maps `"bg/bg_sev_all_p.seg"` → `&bg_sev_all_p_seg`.
- `bg_sev_all_p_seg` is produced by `ob_seg.s:70` (`bg_file_seg bg_sev_all_p_seg, bg_sev_all_p`) — **assembly the port does not build**.
- `nm -g build/obj/asset_assets_obseg_bg_bg_sev_all_p.o` shows `_header`, `_room_data_table`, `_portal_data_table`, `_global_visibility_commands` — and **no `_bg_sev_all_p_seg`**.
- So `bg_sev_all_p_seg` is undefined at link and is satisfied by a **256 KB 0xFF-poisoned array** in `ge_link_stubs.c`. 33 of them.
- `ge_obseg_sizes.c` has **0** matches for any stan/setup/bg symbol (verified by grep), so `gePortObsegSize(bg_sev_all_p_seg)` returns 0, so `obInit` records `rom_size = 0`, so `obLoadBGFileBytesAtOffset` takes its `if (rom_size != 0)` branch and **does nothing at all**.

Net effect at runtime: `load_bg_file` reads its 0x40-byte header out of an
**uninitialised stack array**, then dereferences whatever that garbage says.

### 3.3 The duplicate-symbol wall — VERIFIED

Every one of the 23 `bg_*_all_p.o` objects defines the **same four global symbols**
(`header`, `room_data_table`, `portal_data_table`, `global_visibility_commands`).
Confirmed identical between `bg_sev_all_p.o` and `bg_dam_all_p.o`.

They currently link only because nothing references those names, so the archive never
pulls a single bg member. **You cannot simply link them all.** Options, in the order I
would try them:

1. **Namespace them at compile time.** Compile each `bg_*.c` with
   `-Dheader=bg_sev_header -Droom_data_table=bg_sev_room_data_table …`, and generate a
   table `{levelID → &bg_sev_header}`. No asset regeneration, no format work.
   *(This is my recommendation. Untested — see §6.)*
2. Post-process each object with `ld -r` + symbol renaming. More moving parts on Mach-O.
3. Re-extract bg as **packed 1172 blobs** the way `chr`/`gun`/`prop` are done, and let
   the existing `obLoadBGFileBytesAtOffset` → `romCopy` → `bgDecompress` path run
   unchanged. This is the *lowest-risk-to-the-game-code* option and the one that keeps
   the segment model honest — but I could not find a `.bin` source for bg in the tree
   (`gen_obseg_blobs.py` looks for `assets/obseg/<dir>/<sym>.bin`, and `assets/obseg/bg`
   contains only `.c`/`.h`/`.ld`). Whether `scripts/extract_baserom.u.sh` can emit them
   is **an open question I did not test**.

### 3.4 The 1172 decompressor and level data

- Container: `0x11 0x72` then raw DEFLATE. `tools/gen_obseg_blobs.py` re-creates it as
  gzip minus the 10-byte header and 8-byte trailer. `decompress.c` / `src/inflate/`
  consume it. Already proven working for chr/gun/prop models.
- **stan and setup never touch it** in this build — they are linked C, and `ob.c:233`
  short-circuits before the inflate path. Verified by reading the code and by
  `ge_obseg_sizes.c` containing no stan/setup entries.
- **bg touches it for per-room chunks only.** The bg segment itself is raw; each room's
  point table and display lists are individually 1172-packed *inside* it, and
  `bgDecompress` inflates them per room. So if you go the route-1/route-2 "link the C
  directly" path, you must decide what `pPointTableBin` points at: in the decompiled C
  it points at a `point_table_binary_N` array which is (I believe, **not verified**)
  already the *compressed* chunk. **Check this before writing any code.**

---

## 4. Which structures are asset-file layout vs runtime

Classification rule I used: *is this struct's field layout dictated by bytes that exist
outside the compiler's control?* In this port that reduces to: **is the asset a packed
blob (layout is fixed) or decompiled C (layout is whatever clang says, on both sides)?**

That distinction is the whole answer, and it is different per asset kind.

### 4.1 `StandTile` / `StandFileHeader` — asset layout, but SAFE. **VERIFIED.**

`StandTile` = `u32 id:24; u8 room; union{s16} mid; union{s16} tail; StandTilePoint points[];`
`StandTilePoint` = 4 × `s16` = 8 bytes. **No pointers.** Header = 8 bytes at any width.

`list_of_tilesizes` (stan.c:79) = `{0x20,0x20,0x20,0x20,0x28,0x30,0x38,0x40,0x48,0x50,0x58,0}`
→ for `pointCount = n`, size = `8 + 8n`. Identical on MIPS and arm64.

I verified the adjacency assumption empirically rather than reasoning about it:

```
nm -g build/obj/asset_assets_obseg_stan_Tbg_sev_all_p_stanZ.o | grep ' D _tile_'
→ 1066 tiles, first at 0x14, last at 0x89c4, _footer at 0x89e8
→ every consecutive gap ∈ {32,40,48,56,64,72,80,88}, ZERO anomalies
```

So clang laid the flexible-array-initialised `StandTile` globals out **exactly** the way
`list_of_tilesizes` predicts, tightly packed, in declaration order. The walk in
`stanBuildRoomData` and `stanDetermineEOF` works.

This is a **link-order adjacency** dependency (ROADMAP's found-5×-already bug family)
that happens to hold today. It is not guaranteed by any standard. Add a boot-time
assertion, don't just rely on it.

Endianness: **native, not big-endian.** The tile values are C integer literals compiled
by clang. The `u32 id:24` bitfield lands in the low bits on arm64 and the high bits on
MIPS, but since the *value* is written as `0x1de322` in source, both builds get the
right number. **No byte-swapping anywhere on the stan path.** (This is the trap that
already cost a cycle with the font.)

`StanPrefixRecord` (stan.c:15) is `{s32 stanfile; StandTile *ptr_firstroom;}` and
`StandFileHeader` (bondtypes.h:472) is `{void *unk1; StandTile *firstTile; u8 unk2[];}`.
Both compiled natively by the same compiler → offset 8 on both. Consistent.
`tile_0` sits at 0x14 = 20 bytes in, which matches `8 + 8 + 4`. Confirms the header
layout too.

### 4.2 `stagesetup` and the setup record structs — asset layout, but ALSO SAFE. VERIFIED.

`stagesetup` (bondtypes.h:3997) is 10 pointers. On the N64 the file stored 10 × `u32`
offsets. In this build `assets/obseg/setup/UsetupdamZ.c` is:

```c
stagesetup UsetupdamZ = { &pathwaypoints, &pathsets, &intro, &propDefs, ... };
```

— real 8-byte pointers, and `prop.c` compiles against the same struct. **They agree.**

**But `prop.c:1275` then does `(void*)(((u32)local_stage) + ((u32)local_stage->pathwaypoints))`.**
That is the file-offset rebase, and on native data it is *doubly* wrong: it truncates
both operands to 32 bits **and** adds a base to an already-absolute pointer. Every one
of the ~10 rebases and the ~6 in-place promotion loops that follow has the same shape.

On the native path all of those must become **identity**. The cleanest expression of
that is a single `#ifdef GE_PORT_NATIVE` branch: `g_CurrentSetup = *local_stage;` and
skip the promotion loops entirely. *(Design suggestion — not verified by building.)*

Same idempotency hazard as stan: the promotion loops rewrite the linked `.data` in
place, so loading a second level would double-promote.

### 4.3 `bg_header` / `room_data_table_entry` / `bg_portal_data_entry` — asset layout, NOT SAFE

These are the structures the port has to make a decision about.

| N64 | arm64 |
|---|---|
| `room_data_table_entry` = 3 ptr + 3 f32 = **24 B** | 3 ptr + 3 f32 + pad = **40 B** |
| `bg_header` = 1 u32 + 3 ptr + 1 u32 = **20 B** | 8 + 24 + 8 (aligned) = **40 B** |
| `bg_portal_data_entry` = ptr + 4 u8 = **8 B** | **16 B** |

- If bg becomes a **packed blob** (route 3), all three need 32-bit mirrors via
  `tools/gen_asset_fileview.py` — which today only covers `ModelNode`,
  `ModelAnimation` and `union ModelRoData` members (read the tool: `struct_names()`
  hardcodes exactly those). Level structs would have to be added to it.
- If bg is **linked as C** (routes 1/2), both sides are compiled by clang and agree —
  **no mirror needed** — but `load_bg_file` and the three `bgLoadRoom*` functions must
  stop doing segment arithmetic.

I could not determine which route is correct without knowing whether
`point_table_binary_N` in the decompiled bg is pre-compressed. **That is the single
biggest unknown in this whole document.**

### 4.4 `sImageTableEntry` — asset layout, currently CORRUPTING. VERIFIED.

```c
typedef struct sImageTableEntry { u32 index; u8 width, height, level, format, depth, flagsS, flagsT, pad; }
```
12 bytes on N64. `texLoad(s32 *updateword, ...)` (image.c:2381) is handed
`&textures[i]` — i.e. `&entry.index` — and rewrites it from a texture NUMBER into a
POINTER. In the port it currently does:

```c
*(uintptr_t *)updateword = (uintptr_t)osVirtualToPhysical(tex->data);   /* image.c:2494 */
```

That is an **8-byte store into a 4-byte field**, which silently overwrites
`width/height/level/format`. It is on the level path via
`bgLoadRoomPrimaryGdl → texLoadFromGdl`. It needs the mirror machinery (a separate
resolved-pointer side array keyed by index), not a cast. This is the known-blocked
example the brief named, and I confirmed it is still in this shape.

Also `image.c:2322`: `texLoad((u32 *)((s32)bytes + 4), arg1)` truncates a 64-bit
pointer through `s32` before adding 4.

### 4.5 `BG_SEG_TO_PTR` — a pointer truncation. VERIFIED.

```c
#define BG_SEG_TO_PTR(base, off) ((void *)(((u32)(base)) + (((u32)(off)) + 0xF1000000)))
```
`0xF1000000` is `unsigned int`, so the whole expression is `u32` arithmetic. The
`(u32)(off) + 0xF1000000` wrap is the *intended* segment-tag strip and must be kept.
But `(u32)(base)` **truncates the 64-bit heap pointer**. Correct form keeps the base as
a pointer and only wraps the offset. Used at bg.c:829, 859, 871, 881, 885.

### 4.6 Pointers held in 32-bit types on this path — VERIFIED

- `bg.c:94  s32 ptr_bg_data;` — assigned a stack address and a `mempAllocBytesInBank` result
- `bg.c:97  s32 gptr_stan;` — assigned `_fileNameLoadToBank(...)`
- `bg.c:1838 s32 ptr_bgdata_offsets;`
- `stan.c:3097` `delta`, and the `(s32)tile + …` walk inside `stanDetermineEOF`
- `bondview_r.c:328` `(s32)g_ptrStageSetupFile + (s32)intro_credits->unk04`

All four bg/stan globals must become pointer types before anything on this path can run.

### 4.7 Endianness summary

| data | endianness | evidence |
|---|---|---|
| stan tiles | **native** | C integer literals in `assets/obseg/stan/*.c` |
| setup records | **native** | C literals + `&` in `assets/obseg/setup/*.c` |
| bg geometry (as linked C) | **native** | C literals in `assets/obseg/bg/*.c` |
| bg per-room 1172 chunks | **big-endian**, *inferred not verified* | they are ROM-format DEFLATE payloads containing `Vtx` and F3D display lists; the model path already found model files are big-endian (ROADMAP §"The model file is BIG-ENDIAN") and these are the same class of data. **Verify before assuming.** |
| 1172 container header | byte-oriented | `0x11 0x72` — no swap needed |

---

## 5. Ordered work plan

Riskiest unknowns first, so a wrong assumption is discovered cheaply.

**0. Answer the bg format question before writing any bg code.** *(riskiest unknown)*
Dump `point_table_binary_1` from `assets/obseg/bg/bg_sev_all_p.c` and check whether it
starts with `0x11 0x72`. If yes, the decompiled C is a faithful container of packed
chunks and route 1 (symbol namespacing) is viable with the existing `bgDecompress`
path intact. If no, the extraction already inflated them and the loader's whole
compress/decompress structure has to be bypassed. **Everything in steps 4–7 depends on
this answer.**

**1. Fix `bgDecompress`'s huft workspace.** One line, bg.c:2239:
`u8 buffer[0x2100]` → `struct huft buffer[GE_HUFT_ENTRIES];`. This is a known-fatal
bug of a family already diagnosed three times; leaving it costs a full deploy cycle the
moment a room streams. Cheapest high-value fix in this document.

**2. Make `bondview2.c` compile.** 9 errors, all `-Wint-conversion`, same family as the
already-triaged pointer-cast class. Removes 12 stubs including `g_Startpad`,
`startpadcount` and `gBondViewCutscene`. Low risk, unblocks the intro camera.

**3. Make `chraidata.c` compile.** 71 errors, all preprocessor (`pasting formed '_('`,
"too many arguments to function-like macro") in the AI-command macro DSL. Removes
`setup_text_pointers` and `g_GlobalAILists`. **Without this, `proplvreset2` cannot even
find the setup filename**, so no level has objects, guards or AI. Same macro fix almost
certainly also fixes `assets/obseg/setup/UsetuparchZ.c`.
ROADMAP rule #1: a subsystem must be ALL-STUB or ALL-REAL. Land 2 and 3 together
with `proplvreset2` in mind, and expect the crash to move *earlier* first.

**4. Neutralise the setup rebase for native data.** `prop.c:1275-1400`: under
`GE_PORT_NATIVE`, copy `*local_stage` into `g_CurrentSetup` and skip every promotion
loop. Add a re-entry guard so a second level load does not double-promote. Verify by
walking `g_CurrentSetup.pads[0]` and printing a coordinate that matches the asset `.c`.

**5. Neutralise the stan rebase.** `bg.c:838`: on the native path `delta` must be 0.
Retype `gptr_stan` to `StandFileHeader *`. Add a boot assertion that
`(u8*)tile_next - (u8*)tile == list_of_tilesizes[pointCount & 0xf]` for the first N
tiles — it turns the §4.1 adjacency assumption from luck into a checked invariant.
**After 4 and 5, stan + setup should be live with no format work at all.**

**6. Retype the bg pointer globals** (`ptr_bg_data`, `gptr_stan`, `ptr_bgdata_offsets`)
and fix `BG_SEG_TO_PTR` to keep `base` as a pointer. Mechanical, but must precede 7.

**7. Bridge bg.** Route chosen by step 0. If route 1: generate a per-level
`-Dheader=bg_<lvl>_header …` compile plus a `{levelID → &header}` table, rewrite
`load_bg_file` under `GE_PORT_NATIVE` to look up that table instead of doing the
0x40-byte header read and the `size` computation, and fix
`bgLoadRoomPrimaryGdl`/`SecondaryGdl` to compute their offsets the way
`bgLoadRoomVtxData` already does. If route 3: re-extract bg as `.bin`, extend
`gen_obseg_blobs.py`/`ge_obseg_sizes.c`, extend `gen_asset_fileview.py` with
`bg_header`, `room_data_table_entry`, `bg_portal_data_entry`, and convert at load.

**8. Fix `texLoad`'s 8-byte-into-4-byte store** before rooms render, or room display
lists will corrupt their own image tables. Needs a resolved-pointer side table keyed by
image index — the mirror machinery, per the brief.

**9. Guard the language-variant collision.** `assets/obseg/setup/{u,e,j}/` each define
`UsetupsiloZ`, `UsetupstatueZ`, `UsetuptraZ`, `UsetupdestZ`, `UsetupcradZ`,
`UsetupjunZ`, `Ump_setuparchZ` — **three definitions of each**, all compiled into
`libge.a`. Which one links is whichever archive member the linker reaches first.
Silo/Statue/Train/Frigate could silently load the **European** setup. Build only the
`u/` variant under `-DVERSION_US`.

**10. Make `obLoadBGFileBytesAtOffset` fail loudly.** Its `if (rom_size != 0)` no-op is
the single most dangerous line on this path: it leaves the caller's out-parameter as
stack garbage with no signal. Add a one-shot diagnostic on the else branch.

---

## 6. Verified vs inferred — read this before acting

### VERIFIED (I ran the command or read the exact line)
- 32 named level-path functions are **not** in `ge_link_stubs.c`; `setup_text_pointers` and `g_GlobalAILists` **are**.
- `bg_*_all_p_seg` × 33 are stubs; **no object defines that symbol** (`nm` on `asset_assets_obseg_bg_bg_sev_all_p.o`).
- Every `bg_*.o` defines the same 4 globals — checked `bg_sev` vs `bg_dam` with `nm`.
- `ge_obseg_sizes.c` contains **0** stan/setup/bg entries (`grep -c` = 0), therefore `gePortObsegSize` returns 0 for them, therefore `obInit` sets `rom_size = 0`, therefore `obLoadBGFileBytesAtOffset` is a no-op for bg.
- Stan tile adjacency: 1066 tiles in `Tbg_sev_all_p_stanZ.o`, every gap ∈ {32…88}, zero anomalies. Header 20 bytes, `_footer` immediately after the last tile.
- `stan/*.c` and `setup/*.c` are decompiled C with real `&` pointers (read the files).
- `bgDecompress` still declares `u8 buffer[0x2100]` (bg.c:2241) while `ob.c`/`image.c`/`music.c` were fixed.
- `BG_SEG_TO_PTR` truncates `base` through `(u32)` (bg.h:23).
- `ptr_bg_data`, `gptr_stan`, `ptr_bgdata_offsets` are declared `s32` (bg.c:94, 97, 1838).
- `texLoad` writes `*(uintptr_t*)updateword` into a `u32` field (image.c:2381-2494); `texLoadFromDisplayList` truncates via `(s32)bytes` (image.c:2331).
- Error counts and error *kinds* for `chraidata.c` (71), `bondview2.c` (9), `UsetuparchZ.c` (2), `u/UsetuplenZ.c` (2) — I compiled each with the real build flags.
- `setup/{u,e,j}/` triple definitions — from the object listing in `getv/build/obj`.
- `prop.c` performs the 10-field rebase and 6 promotion loops with `(u32)` arithmetic.
- `gen_asset_fileview.py`'s `struct_names()` covers only `ModelNode`, `ModelAnimation`, `ModelRoData_*`.

### INFERRED — treat every one of these as a guess
- 🔶 **That symbol namespacing (route 1) will work for bg.** I did not try compiling a bg TU with `-Dheader=...`. There may be internal cross-references between the four tables that a `-D` rename breaks, and `bg_ame_all_p.c` is 128 KB — I read only its first 20 lines.
- 🔶 **That `point_table_binary_N` in the decompiled bg is still 1172-compressed.** Not checked. Step 0 exists precisely because this is unknown and everything downstream depends on it.
- 🔶 **That bg per-room chunks are big-endian.** By analogy with the model files. Not verified.
- 🔶 **That the setup rebase can simply be skipped.** I reasoned it from the asset being native C, but I did not check whether any setup consumer *depends* on the rebase having happened (e.g. reads a raw offset elsewhere and expects it promoted).
- 🔶 **That the chraidata macro errors are purely mechanical.** The error *kinds* are verified; that fixing them is easy is a guess. `unknown type name 'Switch'` may hide something structural.
- 🔶 **That fixing `bondview2.c` + `chraidata.c` gets a level to load.** There are 12 other non-compiling files I did not analyse, and 161 generated `Model.c` files that still fail.
- 🔶 **That stan tile adjacency holds for all 29 stan files.** I measured exactly one (`Tbg_sev_all_p_stanZ`). It is the strongest sample available but it is one sample, at one optimisation level.
- 🔶 The `docs/ROADMAP.md` "Current state" table (113/159 objects, boot dies at `texReset`) is **stale** relative to what I measured (`failing.txt` shows 14 failures). I used `failing.txt` and the build objects as ground truth, not the ROADMAP prose.

### NOT INVESTIGATED
- The other 12 non-compiling files (`crash.c`, `indy_*`, `init.c`, `ramrom.c`, `rmon.c`, `sched.c`, `sprintf.c`, `tlb_manage.c`, `usb.c`, `othermodemicrocode.c`, `spectrum.c`).
- Whether Fast3D can execute a **room** display list (it executes the front-end's). GoldenEye's `gsp3D` microcode has commands Fast3D does not implement — ROADMAP flags Dam skybox / Frigate water as the known wall.
- `chr` model loading for guards, and the `lang` bank load inside `proplvreset2`.
