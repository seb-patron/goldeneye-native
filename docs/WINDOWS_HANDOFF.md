# Windows bring-up: handoff

> ## STATUS 2026-08-24: the bug below is FIXED. Do not re-investigate it.
>
> **The world renders.** Every cull figure matches the macOS reference
> (`curroom=29`, `vtx total=326`, `tris submitted=908`).
>
> It was none of the four suspects listed under "Prime suspects". The cause was the order
> **GCC emits `.data` in**: it is reverse declaration order, which shattered the stan tile
> run the engine walks by pointer arithmetic. The game saw a **one-tile level**, put the
> player in room 0, and room 0 has no geometry. Clang emits in source order, which is why
> macOS never showed it.
>
> Fixed with `-fno-toplevel-reorder` and `-fno-zero-initialized-in-bss` on the asset batch,
> plus `extern` on the `tile_0` forward declaration in all 29 stan assets. **`build_linux.sh`
> is GCC too and needs the same two flags.**
>
> Two things this also fixed, without being touched: the `[getv][nostan]` unplaced objects
> (4 → 0), and the weapon/hand rendering. The `hinv=1/0` noted below as a known gap was a
> misreading -- that is the **left** hand hidden, which is correct for a one-handed PP7.
>
> **Full record with before/after numbers: `docs/WINDOWS_STAN_ORDERING.md`.**
>
> The rest of this document is kept as written, because everything in "Traps already paid
> for", "Building" and "Diagnostics available" is still current and still correct.

## The bug you are here to fix ~~(SOLVED -- see the status block above)~~

**The game runs. Characters render. The world does not.**

Reported by the user watching the screen: Bond and the guards are visible and animating, the
level geometry is not. The process does not crash, does not warn, and reports plausible
triangle counts.

That symptom is specific and useful: props and characters come from the model path, the
world comes from the background (`bg`) path -- rooms, portals and room display lists. Two
different systems. One works.

## Start here

macOS is the reference. Same stage, same frame, no input:

```
macOS, stage 9, frame 601
  [getv][cull] f=600 curroom=29 drawn=1 maxrooms=31
               rooms(front=0 straddle=1 BEHIND=0)
               vtx(total=326 front=0 straddle=326 BEHIND=0)
               cur(infront=4/8 nvtx=326)
  [getv] frame 601: tris submitted=908 drawn=331 fog=220 tiledim=5
```

Run the same thing on Windows and diff it:

```
set GETV_STAGE=9
set GETV_CULLSTAT=1
set GETV_EXIT_FRAME=601
C:\ge\getv\build-windows\goldeneye.exe > C:\ge\cull.log 2>&1
```

**If `rooms(...)` shows everything BEHIND**, room culling is rejecting the world and the
bug is in the bbox or the portal test. **If the cull numbers match macOS**, culling is fine
and the geometry is being submitted but not appearing -- look at the combiner, the texture
path or the draw order instead. That single comparison splits the search in half.

**Run from the desktop, never over SSH.** A remote session gets `GL_RENDERER=GDI Generic`
at `GL_VERSION=1.1.0`, Microsoft's software rasteriser. GLEW cannot resolve modern entry
points there, so Fast3D calls through a null function pointer and faults at PC 0x0. It looks
catastrophic and means only "no GPU driver in this session". On the desktop you get
`GL_RENDERER=Intel(R) HD Graphics 4400, GL_VERSION=4.3.0`.

## What is already ruled out

Do not spend time re-testing these.

- **It is not the toolchain.** 977 objects compile, the game batch matches macOS at 167/1.
- **It is not bitfield layout.** MinGW defaults to `-mms-bitfields`, which made
  `sizeof(StandTile)` 12 instead of 8. `-mno-ms-bitfields` is in both flag sets and the
  `_Static_assert` in `stan.c:1453` passes, which is a live check on every build.
- **It is not the endianness macro.** `bondtypes.h:48` accepts `__BYTE_ORDER__` and `_WIN32`
  as well as Clang's `__LITTLE_ENDIAN__`. `platform_info.h` takes the `TARGET_N64` branch on
  every platform, so `IS_BIG_ENDIAN`/`IS_64_BIT` are identical everywhere.
- **It is probably not LLP64 struct drift**, though this is the least certain of the four.
  `grep -nE '^\s+(unsigned\s+)?long\s+[A-Za-z_]' src/bondtypes.h` returns nothing, so no
  game struct has a `long` field to shrink. The libc shadows under `include/` do
  (`math.h:335`, `limits.h:278`: `unsigned long i[2]` for double bit-twiddling) and those
  genuinely differ between LP64 and LLP64 -- worth a look if the cull numbers are clean.

## Prime suspects, in order

1. **Room culling rejecting everything.** `bg.c`'s `sub_GAME_7F0B5528` keep-test and
   `bgRoomCalcBB`. There is history here: room bounding boxes were once garbage across the
   whole game because vertices were byte-reversed, and the tell was a collapse in `drawn`.
   `GETV_CULLSTAT=1` reports it directly.
2. **Room vertex data.** `bgLoadRoomVtxData` byte-swaps room vertices;
   `GETV_VTXSWAP=0` disables it for an A/B. If world geometry is being transformed to
   +/-32767 it will clip away entirely while props, which come from a different converter,
   are unaffected.
3. **Room display-list conversion.** `bgConvertRoomGdl`. `GETV_NOBG=1` and `GETV_NOOBJ=1`
   isolate the two paths from each other.
4. **The asset-side struct mismatch.** The generated backgrounds initialise
   `struct bg_header header = {0, &room_data_table, ...}` from an array-of-struct address,
   which GCC 14 made an error and this build demotes back to a warning
   (`-Wno-error=incompatible-pointer-types`). It is a warning on macOS too, so it is not
   obviously the cause -- but it is the one place the world's own data is built from types
   that do not match, and it deserves a look before anything exotic.

## Building

Everything is installed. `C:\mingw64` has gcc 16.2, SDL2 2.30.9, GLEW 2.2.0;
`%USERPROFILE%\.n64tvos` has Lua 5.4.7 and Dear ImGui 1.91.9b.

```
powershell -NoProfile -ExecutionPolicy Bypass -File C:\ge\getv\build_windows.ps1 -Target all -Mingw C:\mingw64
```

Targets: `all`, `lib` (game+assets+audio+port), `port` (port layer only, seconds not
minutes), `app` (archive and link only), `clean`. Use `port` then `app` when iterating on
port-layer files; the game batch takes about fifteen minutes on this machine.

Expected counts, which must not regress:

```
windows game: 165 built, 3 failed      <- crash.c, spectrum.c, tlb_manage.c are EXCLUDED by
windows assets: 746 built, 0 failed       name and must stay that way; they are N64 hardware
windows audio: 40 built, 0 failed         stubs that ge_link_stubs.c replaces
windows port layer: 26 built, 0 failed
windows port c++: 2 built, 0 failed
windows libge.a: ~30.7 MB, 977 members (+2 roots)
windows binary: goldeneye.exe, ~18 MB
```

If a batch reports failures, the first failure in each batch prints its diagnostics. If you
need more, hand-compile that one file with the same flags -- the script prints them nowhere,
so copy them out of `build_windows.ps1`.

To re-install dependencies from scratch on another machine:
`powershell -NoProfile -ExecutionPolicy Bypass -File C:\ge\tools\fetch_deps_windows.ps1`

## Traps already paid for

These cost real time. They are all fixed, and they are all the kind of thing that comes back.

- **GNU `ar` treats `\` as an escape inside a response file.** Windows paths arrive mangled
  and members are dropped **silently**. The symptom is a link failing on symbols `nm` can
  plainly see inside an object file on disk. The script writes forward slashes and then
  checks `ar t`'s count against the list it handed over.
- **`main()` must be linked outside the archive.** It lives in `ge_mac_main.o`, nothing in
  `libge.a` refers to it, and an archive member is only pulled in to satisfy an existing
  undefined symbol. Buried in the archive, the CRT never finds it -- and the error is
  `undefined reference to WinMain`, which sends you hunting a subsystem flag.
- **`-static` is wrong here.** It pulls the static CRT, which then collides with the
  decomp's own `str.c`. Use `-static-libgcc -static-libstdc++` and copy the runtime DLLs,
  which the script does.
- **A missing runtime DLL exits with `0xC0000135` before `main()` runs.** No message, no
  log, nothing on stdout.
- **GCC 15+ defaults to `gnu23`, where `bool` is a keyword** and `bondtypes.h:85` does
  `typedef s32 bool`. `-std=gnu17` is passed explicitly.
- **A function-like macro is the wrong tool for a name the tree already declares.** Two
  shims were written as macros and both broke headers that declare the same name --
  `usleep` broke MinGW's `unistd.h`, `bcopy` broke the decomp's own `bstring.h`. Both are
  real functions now. If you are tempted to `#define` a libc name, do not.
- **PowerShell's `$ErrorActionPreference = 'Stop'` turns a native tool's stderr into a
  terminating error.** The build sets `Continue` and checks `$LASTEXITCODE` explicitly.
  `Write-Host` also does not reliably reach a redirected stdout; the script uses
  `Write-Output`.
- **MSYS2 is not used and should not be.** Its `fork()` emulation fails on this machine
  (`0xC0000142` on every call) and `rebaseall` made it worse -- it left bash and MSYS2's own
  gcc unable to spawn `cc1.exe`. `getv/build_windows.sh` exists for healthy MSYS2 hosts;
  `build_windows.ps1` is what this machine uses and needs no POSIX layer at all.

## Diagnostics available

All are environment variables, all off by default, all documented at their definition.

| gate | what it gives you |
|---|---|
| `GETV_CULLSTAT=1` | per-frame room culling: how many rooms in front, straddling, behind |
| `GETV_NOBG=1` / `GETV_NOOBJ=1` | draw only objects / only background, to isolate the two paths |
| `GETV_VTXSWAP=0` | disable room vertex byte-swapping, for an A/B |
| `GETV_EXIT_FRAME=<n>` | end the run after n rendered frames. **Use this for every measurement** -- a wall-clock timeout stops at a different frame every run and makes two runs incomparable |
| `GETV_STAGE=<id>` | boot straight into a stage (9 = Bunker 1, 33 = Dam) |
| `GETV_STATE=<n>` | player positions, objectives, and the controls-locked/paused/clock flags |
| `GETV_GUN_DEBUG=1` | weapon state per frame, including the shot counter |
| `GETV_IMGUI=1` | Dear ImGui dev overlay |
| `GETV_SHOTFRAME=<n>` | capture a frame to a file. **Use this rather than a screenshot tool** |

`GETV_SCRIPT="<frame>:<keys>[:<hold>],..."` injects input. Keys are
`A B X Y START BACK Z L R DU DD DL DR CU CD CL CR`, plus `SX=<n>`/`SY=<n>` for the stick,
-80..80. Example: `GETV_SCRIPT="60:START:6,240:SY=70:400"`.

**Known harness gap: no button reaches the trigger.** The stick works, menus work, `Z`
engages the sight (`sightmode` 0 -> 2), but `trigger_down` never sets and `shots` stays 0
with a loaded PP7 in hand. The likely reason is on the same output line: `hinv=1/0`, the
right hand is invisible, so no weapon is drawn. **Fixing this is valuable beyond Windows** --
it currently blocks measuring fire rates and verifying horde waves from a real kill.

## Rules

- **Never `git commit`, `git add`, or `git push`.** Leave changes in the working tree. The
  user pushes, from their own terminal, and was clear about that.
- **Never move, copy or commit a ROM or extracted asset.** `roms/`, `*.z64`, `*.n64`,
  `*.v64`, `base.zip` and `getv/build-*/` are excluded everywhere for a reason.
- **Do not "fix" the 165/3 game split.** Those three files are excluded deliberately.
- **Measure before and after.** This project's standard is that a number enters the record
  with the command that produced it. `GETV_EXIT_FRAME` is what makes two runs comparable.
- Kill any game process you start. `GETV_EXIT_FRAME` makes it exit on its own, which is
  better than remembering to.

## Where things stand

Verified working on Windows: builds (977 objects), links, boots, opens a window, creates a
GL 4.3 context, loads Lua mods, runs the launcher, and rendered 1801 frames of Bunker 1 with
no exceptions. The Win32 crash handler works and has been exercised in anger.

Not working: the world does not appear. That is the whole job.
