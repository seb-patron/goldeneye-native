# Porting this tree to Windows and Linux

Status as of 2026-08-22. Written from a macOS host. **No Windows machine and no Linux
machine has ever compiled this tree**, so nothing below is a claim that anything works
there. Every item is marked as done, unknown, or known missing, and the estimates are
for a developer who has the target machine in front of them.

The shipping target is macOS (`getv/build_mac.sh`) and the on-hold target is tvOS
(`getv/build.sh`, `getv/build_sim.sh`). Neither may regress for the sake of a third
platform.

## 1. What is already portable, and the evidence

The port layer is smaller and cleaner than its history suggests. Measured, not assumed:

- **The game layer contains no Apple-specific code at all.** `GE_PORT_NATIVE` is the
  portability gate and it appears 459 times across 70 files under `vendor/ge-decomp/`.
  A grep of the whole decomp for `__APPLE__`, `__MACH__`, `TARGET_OS_*`, `Darwin`,
  `Cocoa`, `CoreFoundation`, `mach_absolute_time`, `__arm64__`, `_STRUCT_ARM*` and
  `dispatch_*` returns exactly one hit, and it is a code comment
  (`vendor/ge-decomp/src/game/lv.c:212`). `GE_PORT_NATIVE` means "not an N64", not
  "an Apple platform". This is the single most important fact in this document: it
  means Windows is a port of `getv/port/**` and the build script, not of the game.

- **`GE_PLATFORM_MAC` gates eleven lines across four files**, not a tree-wide
  sprawl. Full inventory in section 3.

- **Fast3D and the window backend are already cross-platform**, and in fact already
  carry Windows branches inherited from sm64ex - see section 5.

- **The renderer needs desktop GL 2.1**, which Windows provides more readily than
  macOS does. `getv/port/fast3d/gfx_opengl.c:812` hard-fails below 2.1, and the
  shader generator emits `#version 120`. There is no Metal or D3D dependency
  anywhere.

- **SDL2 2.30.9 is built from source already** (`deps/SDL2-2.30.9`,
  `getv/build_mac.sh:57-71`). The same source builds for Windows and Linux.

- **The pointer-width work is done and is compiler-portable.**
  `vendor/ge-decomp/include/PR/ultratypes.h:83-84` defines `uintptr_t`/`intptr_t`
  from `__UINTPTR_TYPE__`/`__INTPTR_TYPE__` under `GE_PORT_NATIVE`. Those are
  compiler builtins, correct on Win64 LLP64 without change - but only under clang or
  GCC. See section 2.

## 2. The build system

`getv/build_mac.sh` is 303 lines of bash driving `clang` against an Apple SDK. Its
structure - compile the decomp into `libge.a`, keep the two harness objects outside
the archive as link roots, link - is not Apple-specific and should be reproduced
rather than redesigned.

**No CMake or MSBuild file is provided here on purpose.** A build system that nobody
in this repo can execute rots within a week, and this document is more useful than a
green-looking file that has never run.

### Use clang with a MinGW-w64 target, not MSVC

This is a recommendation with reasons, not a preference:

- `getv/port/fast3d/gfx_opengl.c:11-16` and `getv/port/fast3d/gfx_sdl2.c:3-7` gate
  every Windows code path on `__MINGW32__` and nothing else. Under MSVC or clang-cl
  those files silently compile the *non*-Windows branch: `FOR_WINDOWS` is 0, GLEW is
  never included, and `glewInit()` at `gfx_opengl.c:784-788` never runs. The build
  succeeds and then dies at the first GL 1.2+ entry point. Both files are still in flux
  elsewhere and were deliberately not edited here; if you want an MSVC build, widening
  those two gates to `defined(_WIN32)` is the first change to make.
- `vendor/ge-decomp/include/PR/ultratypes.h:83-84` needs `__UINTPTR_TYPE__` and
  `__INTPTR_TYPE__`. MSVC does not define them; clang and GCC do.
- `getv/port/src/port_vi.c:48` uses `__attribute__((constructor))`. MSVC has no
  equivalent short of a CRT section hack.
- `getv/port/src/port_os.c:65` uses `__attribute__((aligned(16)))` on the 32 MB game
  arena. (These two are the only GCC attributes in any translation unit that actually
  gets compiled; the `__attribute__((noreturn))` in `getv/port/platform.h:15` is in a
  dead header - see section 9.)
- `-Wno-everything` (`build_mac.sh:88`) is clang-only. It is not cosmetic: per
  plain `-w` defeats `-Werror=return-type` in both orders, and
  `-Werror=return-type` is the only thing catching the accidental-`$v0`-return bug
  family. Whatever compiler you pick, prove that `-Werror=return-type` still fires by
  compiling a function that falls off the end.
- `-fms-extensions` (`build_mac.sh:76`) is needed for the anonymous struct/union
  members the decomp relies on. Both clang and GCC accept it; MSVC has the behaviour
  natively but warns (C4201).

Estimate: **2-4 days** to get a script that compiles the same file set, assuming
MinGW-w64 clang and a source build of SDL2. Most of that is SDL2 and GLEW plumbing,
not the game.

## 3. `GE_PLATFORM_MAC`: the complete inventory

Eleven code sites. Two of them are misnamed and are the main source of avoidable work.

| Site | What it gates | Windows action |
|---|---|---|
| `getv/port/src/port_input.c:830-1074` | The keyboard-as-port-0 device, ~240 lines | **Rename the gate.** The body is pure SDL2 (`SDL_GetKeyboardState`) with nothing Apple in it. Windows needs it verbatim. |
| `getv/port/src/port_input.c:1099`, `:1124`, `:1182` | Three call sites into the above | Same gate |
| `getv/port/src/port_support.c:87-160` | `configWindow` = resizable 1280x960 window, plus `gePortMacWindowConfig()` reading `GETV_WINDOW`/`GETV_FULLSCREEN` and clamping to the usable display area | **Rename the gate.** The `#else` at `:162` is the tvOS fixed 1920x1080 with no windowing, which is wrong for Windows. Body is pure SDL2. |
| `getv/Sources/ge_tvos_main.c:147` | Calls `gePortMacWindowConfig()` before `gfx_init()` | Same gate |
| `getv/port/src/port_save.c:113` | Diagnostic wording only, since 2026-08-22 | None |
| `getv/port/src/port_paths.c:71` | The macOS user-data directory, since 2026-08-22 | None - the `#else` is already correct on Windows |

**DONE 2026-08-22.** `GE_PLATFORM_DESKTOP` now exists, defined by `build_mac.sh` and to be
defined by the future Windows and Linux scripts, but not by `build.sh` or `build_sim.sh`. The
keyboard device and its three call sites in `port_input.c`, the `configWindow` block in
`port_support.c`, and the caller in `ge_tvos_main.c` are on it. What is left under
`GE_PLATFORM_MAC` is the macOS user-data directory and one diagnostic string, which are
genuinely Apple.

Verified on macOS: build unchanged at 167/1 game, 746/0 assets, 40/0 audio, 23/0 port layer;
the window still comes up 1280x960 resizable; the keyboard code is in the binary (37 keyboard
symbols, `SDL_GetKeyboardState` linked); and `tools/render_refs.py check` reports 27 of 27
stages unchanged.

Without this a Windows build would have had no keyboard input and a fixed 1920x1080 window.

## 4. User-data paths and directory creation - DONE 2026-08-22

Previously there were four independent Apple assumptions:

- `port_save.c` built `$HOME/Library/Application Support/Goldeneye-Native` inline and
  called `mkdir()` from `<sys/stat.h>`.
- `ge_config.c` built `$HOME/Library/Application Support/GoldenEye` inline in two
  separate functions, and one of them created the directory by calling
  `system("/bin/mkdir -p '<path>'")`.

They now go through `getv/port/src/port_paths.{c,h}`:

- `gePortUserDataDir(org, app, out, outsz)` - `$HOME/Library/Application Support/<app>`
  on macOS, `SDL_GetPrefPath(org, app)` everywhere else. On Windows that is
  `%APPDATA%\<org>\<app>\`, which is the correct answer, so **no Windows-specific code
  is needed here at all.**
- `gePortMakeDir(path, mode)` and `gePortMakeDirTree(path, mode)` - `mkdir()` on POSIX,
  `_mkdir()` from `<direct.h>` under `_WIN32`, treating an existing directory as
  success. `gePortMakeDirTree` accepts `\` as a separator as well as `/` on Windows.

The `_WIN32` branch of `port_paths.c` has never been compiled. It is eight lines and
it is the only speculative Windows code in the tree.

macOS behaviour is unchanged; the proof is recorded in section 9.

**Still outstanding, deliberately not changed:** `getv/port/src/port_audio.c:389-394`
writes the `GETV_AUDIO_WAV` debug dump to `$HOME/Documents/getv_audio.wav` and decides
whether a supplied path is absolute with `e[0] == '/'`. It is a debug-only gate in a
file that is still in flux elsewhere. One line of work whenever it is next touched.

## 5. The renderer and window backend

Both files are still in flux elsewhere and were read, not edited.

- `getv/port/fast3d/gfx_sdl2.c` - SDL2 throughout. Its only non-portable include is
  `<unistd.h>` at `:37`, which MinGW provides.
- `getv/port/fast3d/gfx_opengl.c` - desktop GL. Already has `FOR_WINDOWS` branches at
  `:11-16`, `:18-21` (GLEW static) and `:784-788` (`glewInit()`), all keyed on
  `__MINGW32__`.

So on a MinGW-w64 build the renderer needs **GLEW** as a new dependency that the macOS
build does not have (macOS links `OpenGL.framework`, which exports everything
`gfx_opengl.c` uses, and `gfx_sdl2.c:19-26` documents that choice). GLEW is the only
genuinely new third-party dependency Windows introduces.

What is **not** portable in these files: nothing found. The GLES branches are gated on
`USE_GLES`, which only the tvOS scripts define.

Estimate: **1 day**, mostly building GLEW static for the same triple as everything else.

## 6. The crash handler

`getv/Sources/ge_tvos_main.c:57-113` is the most Apple-specific code in the tree, and
it also mattered - it is what cracked the Perfect Dark TCC crash
and it is the primary debugging tool on this port.

Non-portable pieces:

- `<execinfo.h>` (`:16`) for `backtrace`/`backtrace_symbols_fd` - glibc has it, Windows
  does not.
- `<dlfcn.h>` (`:18`) for `dladdr`/`Dl_info` (`:92`) - POSIX, not Windows.
- `<sys/ucontext.h>` (`:17`) plus `_STRUCT_ARM_THREAD_STATE64`,
  `uc->uc_mcontext->__ss`, `__darwin_arm_thread_state64_get_pc/_lr/_sp`
  (`:88-107`) - Darwin arm64 specific. Linux's `ucontext_t` is a flat `gregs` array
  and is a different shape on every architecture; Windows has no `ucontext` at all.
- `sigaction` with `SA_SIGINFO` (`:118-128`) - Windows has only `signal()`, with no
  `siginfo_t` and no faulting address.

The Windows equivalent is `SetUnhandledExceptionFilter` plus DbgHelp
(`SymFromAddr`, `StackWalk64`), reading the faulting address from
`EXCEPTION_RECORD.ExceptionInformation[1]` and the PC from `CONTEXT.Rip`. That is a
rewrite, not a port, and it deserves its own file behind a small interface
(`gePortInstallCrashHandler()`) rather than more `#ifdef` in the harness.

Estimate: **2-3 days on Windows**, half a day on Linux (glibc has `execinfo.h` and
`dladdr`; only the register dump needs an architecture branch). A build that skips the
handler entirely is 30 minutes and is a reasonable first milestone - but expect the
first Windows crash to be much harder to diagnose than the equivalent macOS one.

## 7. `#pragma weak` and other ELF-only constructs

`#pragma weak A = B` is an ELF weak alias. Mach-O has no equivalent and clang reports
every use of the alias as "reference is ambiguous". COFF is a third case: GCC on
MinGW accepts the pragma using PE weak externals, MSVC ignores it entirely as an
unknown pragma (C4068). None of the three behave the same.

**Already solved, and solved in a way that is correct for COFF as well as Mach-O** -
these were fixed by replacing the alias with a plain `#define` under `GE_PORT_NATIVE`,
which is a preprocessor-level rename and involves no object-format feature:

- `vendor/ge-decomp/src/game/lv.c:210-227` - four `g_DebugPortalsInputBufferSource*`
- `vendor/ge-decomp/src/game/spectrum.c:51-67` - `spec_keyboard_row_caps_z_x_c_v`
- `vendor/ge-decomp/src/music.c:654-663` - five `_*SegmentRomStart` audio segments
- `vendor/ge-decomp/src/game/objecthandler.c:19` - expressed as pointers instead

**Not yet handled - three ungated sites:**

- `vendor/ge-decomp/src/libultra/gu/sinf.c:33-34` -
  `#pragma weak fsin = __sinf` and `#pragma weak sinf = __sinf`
- `vendor/ge-decomp/src/libultra/gu/cosf.c:33-34` - the same for `fcos`/`cosf`
- `vendor/ge-decomp/src/game/objective_status.c:290` -
  `#pragma weak objectiveGetStatus_WEAK = get_status_of_objective`

All three compile today on Mach-O only because clang warns and ignores them and
`-Wno-everything` hides the warning. The `sinf`/`cosf` pair is the risky one: if the
pragma *is* honoured on the target, it defines a weak global `sinf` and `cosf` that
compete with the C library's. Note that both files immediately `#define fsin __sinf`
on the next line, so the pragmas are already redundant for the port; wrapping all four
in `#ifndef GE_PORT_NATIVE` is the same fix already applied to `spectrum.c`, and it is
provable on macOS because the current behaviour is "ignored" either way.

`objective_status.c` is still in flux elsewhere and was not edited.

Estimate: **1 hour**, plus whatever it takes to agree on
`objective_status.c`.

## 8. Link-time: four undefined symbols that macOS currently hides

This is the item most likely to be mistaken for a compiler problem.

`getv/build_mac.sh:239-252` documents that `-dead_strip` is required rather than an
optimisation: ld64 dead-strips **before** it checks for undefined symbols, so a
reference from an unreachable function is not an error. Neither `lld-link /OPT:REF` nor
GNU `ld --gc-sections` is guaranteed to behave that way - unresolved externals are
generally diagnosed independently of section garbage collection.

Measured on 2026-08-22 by linking the current archive without `-dead_strip`. The
complete list is four symbols:

```
__efontchardataSegmentRomStart   referenced from langGetJpnCharPixels  (src/game/language.o)
__jfontchardataSegmentRomStart   referenced from langGetJpnCharPixels  (src/game/language.o)
_osPiReadIo                      referenced from tokenReadIo           (src/token.o)
_osViSetMode                     referenced from viVsyncRelated        (src/fr.o)
```

Every one is reachable only from a function this port never calls: the Japanese font,
a PI register read, and a VI mode set.

**DONE 2026-08-22.** `getv/port/src/port_n64_unused.c` defines all four: two zeroed `u32`
data symbols and two no-op functions. Not `ge_link_stubs.c`, which is a generated
diagnostic scaffold meant to be deleted, whereas these are permanent and correct.

Verified by linking the archive without `-dead_strip`, both ways: with that object removed
the link fails on exactly those four symbols, and with it present the link is clean and
produces an 18 MB binary. The dependency on ld64's strip-before-diagnose ordering is gone
on every platform.

Note that `osEepromRead`/`osEepromWrite` used to be on this list and are not any more:
`getv/port/src/port_save.c` defines all five EEPROM entry points for real.

Estimate: **1 hour.** This is worth doing on macOS first, where it is verifiable, and
it makes the macOS link less fragile as a side effect.

## 9. Repository hygiene that blocks any non-macOS checkout

`getv/port/include/` contains two **absolute** symlinks into the author's home
directory:

```
getv/port/include/PR               -> <repo>/vendor/ge-decomp/include/PR
getv/port/include/platform_info.h  -> <repo>/vendor/ge-decomp/include/platform_info.h
```

These break on any other machine, on any operating system, and Windows additionally
requires Developer Mode or `core.symlinks=true` for git to materialise symlinks at all.
Replace them with relative symlinks, or drop them and add
`-I vendor/ge-decomp/include` to the port flags. The second option is preferable - the
comment at `getv/Sources/ge_tvos_main.c:29-32` explains that the symlink exists
specifically to expose `PR/` *without* exposing the decomp's `math.h`/`string.h`/
`stdlib.h`/`stddef.h`, which shadow the system headers, so the include path would have
to point at a directory containing only `PR/` and `platform_info.h`.

`getv/port/platform.h` and `getv/port/include/platform.h` were byte-identical duplicates,
described here as included by no translation unit. That was wrong, and acting on it breaks
the build: `getv/port/platform.h` is included as `"../platform.h"` by `gfx_opengl.c:44`,
`gfx_pc.c:28`, `fs/fs.h:9` and `port_support.c:15`, and it declares `sys_fatal` and
`sys_sleep`. Only the `include/` copy was genuinely unused.

It was also the more urgent of the two problems rather than the lesser. The header pulled in
`<TargetConditionals.h>` unconditionally, so on Windows or Linux it would have been the first
thing to fail, in four translation units at once.

**Fixed 2026-08-22.** The Apple include and the `TARGET_OS_TV` test it guards now sit behind
`#ifdef __APPLE__`; `PLATFORM_TVOS` stays undefined elsewhere, which is correct. The unused
`include/` duplicate is deleted. That removes the last unconditional Apple-only include from
the port layer, and the macOS build is unchanged at 167/1, 746/0, 40/0, 24/0.

`getv/port/fs/fs.h` was described here as likewise dead. It is not: `gfx_pc.c:30` and
`port_support.c:17` both include it, so removing it breaks the build. It is one of the
fifteen fetched third-party files and stays. Whether its declarations are ever implemented
is a separate question; the header itself is still required today.

Estimate: **1 hour.**

### The asset pipeline was macOS-only, and is not any more

Worth stating separately because it sits before the build rather than in it. Two tools the
asset pipeline depends on shelled out to `xcrun` unconditionally:

- `tools/uniquify_asset_symbols.py`, section 3.6
- `tools/gen_asset_fileview.py`, section 3.5

`xcrun` exists only on macOS. Off Apple both left the SDK path empty and passed
`-isysroot ''`, which fails every compile. In `uniquify` that failure is silent in the worst
way: a file that will not compile is reported as one `SKIP` among hundreds of `ok` lines and
is left with colliding symbols, so the tool renames nothing while appearing to work.

**Fixed 2026-08-22.** Both now pass a sysroot only on Apple, and nothing at all elsewhere,
where the system headers are already on the default search path. `gen_asset_fileview.py` was
additionally asking for the `appletvos` SDK, the same wrong-SDK bug `uniquify` had.

Verified unchanged on macOS: `gen_asset_fileview.py` still reports 21 of 21 file views
matching the N64 layout, and `uniquify` still comes back clean with no skips on chr, prop,
setup and stan.

## 10. Ordered plan for Windows

1. Repository hygiene - relative include paths, delete the three dead headers. 1 hour.
2. Four link stubs in `ge_link_stubs.c`, verified on macOS. 1 hour.
3. Gate the four ungated `#pragma weak` sites, verified on macOS. 1 hour.
4. Split `GE_PLATFORM_DESKTOP` out of `GE_PLATFORM_MAC`, verified on macOS. Half a day.
5. Build SDL2 2.30.9 and GLEW for the MinGW-w64 triple. 1 day.
6. Write `build_win.sh` (or a batch equivalent) mirroring `build_mac.sh`'s file set,
   flag set and archive-plus-two-roots link structure. 2-3 days.
7. First link. Expect a second round of undefined symbols that macOS's `-dead_strip`
   was hiding and that step 2 did not predict, because the reachable set differs once
   the compiler and its inlining decisions change.
8. Crash handler on `SetUnhandledExceptionFilter` + DbgHelp. 2-3 days.
9. Debugging the first run. **Unbounded and deliberately not estimated.**

Steps 1-4 are worth doing regardless of whether anyone builds for Windows: they are all
verifiable on macOS today and every one of them makes the macOS build more robust.

**Honest overall assessment.** Getting to a first *link* on Windows is on the order of
**one to two weeks** for someone with the machine, assuming they follow the MinGW route.
Getting to a *playable* Windows build is not estimable from here, because the entire
class of problems this port has actually spent its time on - endianness, pointer
truncation, uninitialised state, heap-layout sensitivity - is exactly the class that
behaves differently under a different compiler, a different libc and a different
allocator. Several bugs on this project had symptoms that moved with
the framebuffer size. Those will not reproduce identically on Windows, and some latent
ones that macOS happens to tolerate will surface there.

Do not read "one to two weeks to a link" as "one to two weeks to a Windows release."

## 11. Ordered plan for Linux

Linux is substantially closer than Windows and would make a better second platform:

- Steps 1-4 above are shared.
- `#pragma weak` is native - the ELF sites in section 7 work as originally written, so
  step 3 is optional there.
- `SDL_GetPrefPath` already returns the correct XDG directory
  (`$XDG_DATA_HOME/<app>/`), so section 4 needs nothing further.
- GLEW is not required if the build uses SDL2's GL loader, matching the macOS
  arrangement.
- The crash handler needs only an architecture branch for the register dump;
  `execinfo.h`, `dladdr` and `sigaction` are all present on glibc.
- `<unistd.h>`, `usleep`, `mkdir`, `_exit` are all native.

Estimate to a first link: **2-4 days.** To a running build: unknown, for the same
reasons as Windows, but the toolchain is close enough to Apple clang that fewer
surprises should be expected.

## 12. Explicitly unknown

Things this document does not claim to know, listed so nobody mistakes silence for
confidence:

- Whether the four link stubs in section 8 are the complete set on a non-macOS linker.
  They are the complete set for ld64 on arm64 today.
- Whether the `_WIN32` branch of `port_paths.c` compiles. It has never been through a
  compiler.
- Whether GLEW's static build interacts correctly with SDL2's GL context creation on
  Windows in this configuration. `gfx_opengl.c:784-788` is inherited from sm64ex and
  presumably worked there.
- Whether the decomp's remaining 32-bit assumptions are all found. The project has recorded
  251 identified struct-layout hazards; a different ABI will exercise a different
  subset.
- Anything about audio on Windows. `getv/port/src/port_audio.c` is SDL2 audio and
  looks portable, but the mixer path has never been exercised anywhere but Apple
  platforms.

## 13. Build baselines

Any change made for portability must leave these unchanged
(`getv/build_mac.sh all`):

```
mac game:        167 built, 1 failed     (src/tlb_manage.c is N64 TLB hardware, expected)
mac assets:      746 built, 0 failed
mac audio:        40 built, 0 failed
mac port layer:   23 built, 0 failed
```

The port-layer count moved from 22 to 23 on 2026-08-22 with the addition of
`port_paths.c`. `getv/build.sh` and `getv/build_sim.sh` glob `port/src/*.c` the same
way, so their port-layer counts each rise by one as well - `build_sim.sh`'s documented
baseline of 18 should become 19. That is inferred from the glob, not from a simulator
run; confirm it on the next `build_sim.sh port` rather than treating a 19 as a
regression.

Behavioural proof that the section 4 refactor left macOS untouched, captured before and
after on the same host:

- `--write-config` under a synthetic `$HOME`: identical stdout, identical resulting
  path, identical `0755` directory mode, byte-identical `goldeneye.cfg`.
- Config discovery: `[getv][config] file .../Library/Application Support/GoldenEye/goldeneye.cfg`
  identical, i.e. search step 4 still resolves.
- `--write-config` with `$HOME` unset: `[getv][config] no $HOME`, exit 1, unchanged.
- The EEPROM path across all five reachable cases - no `Application Support` parent,
  success, no `$HOME`, `GETV_SAVE=0`, `GETV_SAVEDIR` - identical strings and identical
  `PROBE`/`ENABLED` values.
- A two-frame run diffed line by line: 260 lines before, 260 after, differing only in
  millisecond timings and one ASLR-dependent heap address.
