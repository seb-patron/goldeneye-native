# Windows: the world did not render

Resolved 2026-08-24. This is the record for the bug `docs/WINDOWS_HANDOFF.md` was written to
hand over: on Windows the game ran, characters and props rendered and animated, and the level
geometry did not.

The cause was not in the renderer, the culler, or any of the four suspects the handoff listed.
It was the **order GCC emits `.data` in**, which shattered the stan tile run that the engine
walks by pointer arithmetic.

---

## 1. What the measurement said

The handoff's first instruction was to run the macOS reference on Windows and diff it. That
split the search immediately, though not the way it predicted.

```
set GETV_STAGE=9
set GETV_CULLSTAT=1
set GETV_EXIT_FRAME=601
goldeneye.exe > cull.log 2>&1
```

| | macOS f=600 | Windows f=600 (before) |
|---|---|---|
| `curroom` | 29 | **0** |
| `rooms` | front=0 straddle=1 BEHIND=0 | front=1 straddle=0 BEHIND=0 |
| `vtx total` | 326 | **0** |
| `cur` | infront=4/8 nvtx=326 | 8/8 nvtx=0 |

`BEHIND=0`, so nothing was being culled away. The player was simply in **room 0**, which on
this level has no portals and no geometry -- `GETV_ROOMTRACE=1` showed `pri=NULL vtx=NULL
adj=0`, so the portal walk had nowhere to go and the draw list contained one empty room.

`g_BgCurrentRoom` comes from `bondviewGetCurrentPlayersRoom()` (bondview2.c), which reads
`field_488.current_tile_ptr_for_portals->room`. The tile pointer was **not** null and the
NULL-stan ROOMGUARD never fired, so `tile->room` was genuinely reading 0.

The line that named the cause was already being printed:

```
[getv][stanEOF] first=00007FF6F38291F8 end=00007FF6F38291F8 tiles=1 bytes=0
```

**One tile.** The entire level's stan was a single tile, and the player's tile pointer
(`...218`) was the zero word 32 bytes past it -- not a tile at all, which is why `room` read 0.

---

## 2. The cause

`nm --numeric-sort` on the built object:

```
0000000000000000 B Tbg_sev_all_p_stanZ_tile_1066   <- .bss, not .data
...
00000000000089b8 D Tbg_sev_all_p_stanZ_tile_1
00000000000089e0 D Tbg_sev_all_p_stanZ             <- header wedged between them
00000000000089f8 D Tbg_sev_all_p_stanZ_tile_0      <- END of .data
```

Three separate things were wrong, and all three break the same invariant.

The stan format is **one flat contiguous run of tiles in declaration order**, terminated by an
all-zero tile. The engine walks it by adding each tile's byte size (`stanDetermineEOF`), and
resolves tile links as `standTileStart + (link << 3)`. Both depend on the tiles being adjacent
in memory, in source order, with nothing in between.

1. **GCC emits `.data` in reverse declaration order.** `tile_0` landed at the end of the
   section and `tile_1` before it, so walking forward from `tile_0` ran straight off the end
   after one tile. Clang emits in source order, which is why macOS never showed
   this and why the bug looked platform-specific in a way that suggested endianness.

2. **The all-zero terminator tile went to `.bss`.** `tile_1066` is entirely zero, so GCC put
   it in `.bss` by default, detaching it from the run it is supposed to terminate.

3. **The `tile_0` forward declaration was a tentative definition, not an `extern`.** Every
   generated stan file opens with `StandTile Tbg_..._tile_0;` under a `// forward
   declarations` comment. Without `extern` that is a tentative *definition*, so GCC allocated
   `tile_0` at that point in the file and the 24-byte `StandFileHeader` ended up sitting
   between `tile_0` and `tile_1`.

---

## 3. The fix

Two compiler flags on the **asset batch only** (`build_windows.ps1`):

```
-fno-toplevel-reorder        # emit .data in declaration order
-fno-zero-initialized-in-bss # keep the all-zero terminator tile in .data
```

and `extern` added to the forward declaration in all 29 files under
`assets/obseg/stan/`:

```c
-StandTile Tbg_sev_all_p_stanZ_tile_0;
+extern StandTile Tbg_sev_all_p_stanZ_tile_0;
```

Scoped to assets deliberately. The game batch is code; the only place adjacency of top-level
data is load-bearing is the level data. `src/snd.c`'s "declaration order matters" comment is
about stack locals for matching and is unrelated.

Verified at the object level before rebuilding -- header first, then tiles contiguous at
exactly their byte sizes, nothing in `.bss`:

```
0000000000000000 D Tbg_sev_all_p_stanZ
0000000000000018 D Tbg_sev_all_p_stanZ_tile_0
0000000000000038 D Tbg_sev_all_p_stanZ_tile_1
0000000000000058 D Tbg_sev_all_p_stanZ_tile_2
```

---

## 4. After

Same command as section 1:

```
[getv][stanEOF] first=00007FF7B0B70CD8 end=00007FF7B0B79688 tiles=1066 bytes=35248
[getv][cull] f=601 curroom=29 drawn=1 maxrooms=31 rooms(front=0 straddle=1 BEHIND=0)
             vtx(total=326 front=0 straddle=326 BEHIND=0) cur(infront=4/8 nvtx=326)
[getv] frame 601: tris submitted=908 drawn=329 fog=220 tiledim=5
```

Every cull figure matches the macOS reference. `tris submitted=908` matches exactly; `drawn`
is 329 against macOS's 331, a two-triangle difference at the same frame index.

Downstream symptoms that were the same bug and are now gone without being touched:

- **`[getv][nostan]` unplaced objects: 4 → 0.** Objects whose pad lookup returned a null stan
  were failing because the tile walk could not reach their tiles.
- **The weapon and right hand render.** `WINDOWS_HANDOFF.md` recorded `hinv=1/0` as "the right
  hand is invisible, so no weapon is drawn" and listed it as a known harness gap. `hinv=1/0`
  is `hand_invisible[0]=1, [1]=0` -- the **left** hand hidden, which is correct for a
  one-handed PP7. The frame capture shows the PP7 drawn with the ammo HUD reading `7 | 93`.

---

## 5. What this means for the other platforms

The Linux build (`build_linux.sh`) uses GCC and compiles the same asset tree, so it is
affected by exactly this and needs the same two flags. The `extern` change is in the shared
asset sources and covers it already. macOS is clang and is unaffected either way, but the
flags are GCC spellings and must not be added to the clang batches.

Say this plainly, because the failure mode is silent: nothing warns, nothing
crashes, triangle counts stay plausible, and the game runs at full speed rendering an empty
world.
