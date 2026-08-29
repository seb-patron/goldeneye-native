# Third-party sources this repository does not ship

Goldeneye-Native does not contain a complete build tree. Two things must be supplied before it
compiles: the GoldenEye 007 ROM, which you already have to provide yourself, and fifteen source
files inherited from another project, which are fetched by a script. This document explains what
those fifteen files are, where they come from, on what terms, and why they are fetched rather
than vendored.

`docs/LICENSING.md` is the wider provenance record for the whole repository, and
`getv/port/PROVENANCE.md` is the file-level record for the port layer. This document covers only
the fetched files.

Last verified 2026-08-22 against the working tree and against upstream over the network.

---

## 1. Summary

| | |
|---|---|
| Upstream project | sm64ex |
| Upstream URL | `https://github.com/sm64pc/sm64ex` |
| Pinned commit | `d7ca2c04364a6dd0dac58b47151e04e26887e6f0` (2024-12-17) |
| Files fetched | 15 |
| Fetch script | `tools/fetch-thirdparty.sh` |
| File list | `getv/patches/thirdparty/MANIFEST` |
| Local changes | `getv/patches/thirdparty/0001-getv-port-layer.patch` |

To build, run this once from the repository root:

```
tools/fetch-thirdparty.sh
```

It clones sm64ex at the pinned commit, copies the fifteen files into place, and applies the
patch that carries every change this project made to them. `tools/fetch-thirdparty.sh verify`
re-derives all fifteen files from the pin and the patch and compares them byte for byte against
what is on disk.

---

## 2. Why these files are not in the repository

The renderer in `getv/port/fast3d/` is Fast3D. Goldeneye-Native did not write it. It came from
sm64ex, which took it from `Emill/n64-fast3d-engine`. That engine has never carried a settled
permissive licence, and the specific form of the notice that reaches this project is the
restrictive one.

The evidence, all of it from upstream's own git history:

- `LICENSE.txt` in `Emill/n64-fast3d-engine` has four commits. In the initial commit
  `a99492dd` (2020-04-24), condition 2 reads, in full: `Redistributions in binary form are not
  allowed.` That is a flat prohibition, not a conditional one.
- Commit `881eb68b` (2021-10-26, "Updating license") changed that one line, adding the
  carve-out "except in cases where the binary contains no assets you do not have the right to
  distribute".
- GitHub's own licence detection classifies the repository `NOASSERTION` - that is, it does not
  recognise the text as any standard licence.

This project's lineage runs through the pre-2021 form. sm64ex ships no root licence file at
all, and reproduces the Emill notice in exactly one place, `src/pc/README-n64-fast32-engine.md`,
in the original wording with the flat binary ban and no asset carve-out. Whatever the correct
reading of Emill's later text may be, the notice this project actually inherited is the strict
one.

Two other downstream projects label the same lineage differently. The Perfect Dark PC port
carries plain MIT in `port/fast3d/LICENSE.txt`, applied in commit `9508b136` (2023-08-01), which
replaced Emill's custom text outright while keeping the `Copyright (c) 2020 Emill, MaikelChan`
attribution line; that port's Fast3D came via libultraship, which is MIT. mgb64 goes the other
way and explicitly retracts an earlier MIT claim about the Emill engine, reading it as
BSD-2-Clause with a binary-redistribution restriction. This project takes no position on which
reading is correct, because it does not have to: it does not redistribute the code.

The same reasoning covers the audio mixer. `getv/port/audio/ge_mixer.c` is sm64ex's
`src/pc/mixer.c` - Emill's software implementation of the N64 audio microcode - with four
changes. It shares the licence question with the renderer, so it is handled the same way.

`getv/port/configfile.h` and `getv/port/fs/fs.h` are unmodified sm64ex files, still carrying
`CONFIGFILE_DEFAULT "sm64config.txt"` and the include guard `_SM64_FS_H_`. They are trivial
headers and nothing turns on them, but they are sm64ex's text and not this project's, so they
are fetched with the rest rather than being an exception that needs its own justification.

## 3. What is fetched

Fifteen files, listed in `getv/patches/thirdparty/MANIFEST`:

| upstream path in sm64ex | destination |
|---|---|
| `src/pc/gfx/gfx_cc.c` | `getv/port/fast3d/gfx_cc.c` |
| `src/pc/gfx/gfx_cc.h` | `getv/port/fast3d/gfx_cc.h` |
| `src/pc/gfx/gfx_opengl.c` | `getv/port/fast3d/gfx_opengl.c` |
| `src/pc/gfx/gfx_opengl.h` | `getv/port/fast3d/gfx_opengl.h` |
| `src/pc/gfx/gfx_pc.c` | `getv/port/fast3d/gfx_pc.c` |
| `src/pc/gfx/gfx_pc.h` | `getv/port/fast3d/gfx_pc.h` |
| `src/pc/gfx/gfx_rendering_api.h` | `getv/port/fast3d/gfx_rendering_api.h` |
| `src/pc/gfx/gfx_screen_config.h` | `getv/port/fast3d/gfx_screen_config.h` |
| `src/pc/gfx/gfx_sdl.h` | `getv/port/fast3d/gfx_sdl.h` |
| `src/pc/gfx/gfx_sdl2.c` | `getv/port/fast3d/gfx_sdl2.c` |
| `src/pc/gfx/gfx_window_manager_api.h` | `getv/port/fast3d/gfx_window_manager_api.h` |
| `src/pc/mixer.c` | `getv/port/audio/ge_mixer.c` |
| `src/pc/mixer.h` | `getv/port/audio/ge_mixer.h` |
| `src/pc/configfile.h` | `getv/port/configfile.h` |
| `src/pc/fs/fs.h` | `getv/port/fs/fs.h` |

## 4. What is not fetched, because it is this project's own work

`getv/port/fast3d/ge_sky_rdp.c` and `ge_sky_rdp.h` (517 lines) were written for this project and
stay in the repository. They have no counterpart anywhere in sm64ex. They decode the RDP
triangle commands that GoldenEye's `sky.c` assembles by hand out of `G_RDPHALF_1` / `G_RDPHALF_2`
/ `G_RDPHALF_CONT` pairs - a GoldenEye-specific construct that sm64 never emits and Fast3D
therefore never had to handle.

Everything else under `getv/port/` - `src/`, `mac/`, `include/`, `cliopts.h`, `platform.h`,
`pc_main.h`, `audio/ge_mixer.h`'s GoldenEye-specific portion - is likewise this project's own
and is not affected by any of this. Only the fifteen files in the manifest are removed.

## 5. How much of each file is upstream

Measured by line, comparing each file against its upstream counterpart at the pinned commit.
"Upstream retained" is the share of upstream's lines that survive verbatim in this project's
version; "of ours" is the share of this project's file that those lines account for.

| file | lines here | lines upstream | retained | upstream retained | of ours |
|---|---|---|---|---|---|
| `gfx_cc.c` | 47 | 41 | 40 | 97.6% | 85.1% |
| `gfx_cc.h` | 103 | 58 | 55 | 94.8% | 53.4% |
| `gfx_opengl.c` | 968 | 692 | 649 | 93.8% | 67.0% |
| `gfx_opengl.h` | 8 | 8 | 8 | 100% | 100% |
| `gfx_pc.c` | 5,513 | 1,820 | 1,126 | 61.9% | 20.4% |
| `gfx_pc.h` | 35 | 30 | 30 | 100% | 85.7% |
| `gfx_rendering_api.h` | 36 | 36 | 34 | 94.4% | 94.4% |
| `gfx_screen_config.h` | 7 | 7 | 7 | 100% | 100% |
| `gfx_sdl.h` | 8 | 8 | 8 | 100% | 100% |
| `gfx_sdl2.c` | 432 | 346 | 340 | 98.3% | 78.7% |
| `gfx_window_manager_api.h` | 25 | 25 | 25 | 100% | 100% |
| `ge_mixer.c` | 1,244 | 871 | 852 | 97.8% | 68.5% |
| `ge_mixer.h` | 103 | 53 | 35 | 66.0% | 34.0% |
| `configfile.h` | 67 | 67 | 67 | 100% | 100% |
| `fs.h` | 138 | 138 | 138 | 100% | 100% |
| `ge_sky_rdp.c` | 405 | - | 0 | - | 0% |
| `ge_sky_rdp.h` | 112 | - | 0 | - | 0% |

`gfx_pc.c` is the outlier in both directions. It has grown from 1,820 lines to 5,513, so only a
fifth of the file is inherited - but 1,126 of upstream's lines are still in it verbatim, which
is why it is fetched rather than kept.

## 6. The patch

`getv/patches/thirdparty/0001-getv-port-layer.patch` is a single unified diff, roughly 298 KB,
from the pinned upstream files to this project's versions. It is generated with **zero context
lines** (`diff -U0`). Zero context is safe here because the patch is only ever applied to one
exact commit, so there is nothing for context to disambiguate, and it keeps unmodified upstream
lines out of a file the repository does distribute.

The patch is the substance of the port's rendering work, not a thin adaptation layer. Across the
fifteen files it introduces 70 distinct `GETV_*` switches and diagnostic probes. The corrections
that matter most for output correctness are:

- `GETV_CVGSEL` - coverage-vs-alpha selection. Fast3D blended against a value the RDP discards;
  roughly three-quarters of a typical frame was affected.
- `GETV_ZCMP` - depth comparison was being read from RSP geometry mode rather than from the RDP
  other-modes word, which is where the hardware actually keeps it.
- `GETV_RECTFLIP` - flipped texture rectangles had their coordinate negation applied twice.
- `GETV_RGBA16BE` - RGBA16 texture byte order. Mode 1 is the default; 0 restores the old order.
- `GETV_FILTCLAMP` - texture filter clamping at tile edges.
- `GETV_PROBE_AFTER` and the surrounding probe family - the instrumentation used to measure all
  of the above.

The remainder are listed in the patch itself. `tools/fetch-thirdparty.sh regen` rewrites the
patch from the current working tree; run it after editing any manifest file, because the patch
is the only place such an edit is recorded.

## 7. Terms

This project does not relicense anything it fetches. The fifteen files remain under whatever
terms sm64ex and `Emill/n64-fast3d-engine` impose on them, and fetching them puts you in the
same position as cloning sm64ex directly. Goldeneye-Native's own contribution to those files is
the patch, and the patch is covered by this repository's terms.

Nothing in this arrangement is a claim that redistributing those files would be unlawful. It is
a decision not to redistribute code whose terms are unresolved, taken because the project can
achieve the same result without doing so.

## 8. Reproducibility

The pin is a full commit SHA, not a tag or a branch. `tools/fetch-thirdparty.sh` fetches that
SHA directly, so it keeps resolving if upstream's default branch moves or the repository is
archived. As of 2026-08-22, `d7ca2c04` is also the tip of sm64ex's default branch (`nightly`).

One thing that cannot be established from the repository: which sm64ex commit these files were
*originally* copied from. The in-tree clone at `vendor/sm64ex` is a depth-1 checkout with no
history, so there is no local record to compare against, and none of the copied files carries a
version marker. `d7ca2c04` is pinned because it is verifiable rather than because it is proven
to be the original source: at that commit six of the fifteen files are byte-identical to this
project's copies, and the other nine differ only in ways attributable to this port's own work.
If the files came from a slightly earlier commit, the patch absorbs the difference - it is
generated against `d7ca2c04` and round-trips to the current tree byte for byte, which
`tools/fetch-thirdparty.sh verify` checks on demand.

## 8b. Lua, Dear ImGui, and the projects deliberately not read

**Lua 5.4.7 (MIT).** Fetched and built by `tools/fetch_lua.sh` into `~/.n64tvos/lua-*`, never
vendored, on the same terms as SDL2: `deps/` is not tracked. It backs the mod scripting host in
`getv/port/src/ge_lua.c`. MIT imposes nothing on the rest of the tree, and the build works
without it -- absent `liblua.a`, the hooks compile to empty functions. Upstream is
`https://www.lua.org/ftp/lua-5.4.7.tar.gz`, sha256
`9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30`, checked by the fetch script
before anything is compiled into the game.

**Dear ImGui 1.91.9b (MIT).** Fetched and built by `tools/fetch_imgui.sh` into
`~/.n64tvos/imgui-*`, never vendored, on the same terms as Lua and SDL2. It backs the dev
overlay in `getv/port/src/ge_imgui.cpp`, which is what a launcher or debug UI gets built on.
 MIT imposes nothing on the rest of the tree, and the build works without it --
absent `libimgui.a`, the entry points compile to empty functions and `gfx_sdl2.c` needs no
`#ifdef`. It is additionally off at runtime unless `GETV_IMGUI=1`, so having it installed does
not change how the game behaves. Upstream is
`https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz`, sha256
`8e1bbc76c71d74fef2fb85db7e7ca8eba13d6a86623c54992b60162db554ffdb`, checked by the fetch script
before anything is compiled into the game.

Five ImGui core sources plus two backends are compiled: `imgui_impl_sdl2.cpp` and -- note --
`imgui_impl_opengl2.cpp`, the *fixed-function* renderer backend rather than the usual
`imgui_impl_opengl3.cpp`. `build_mac.sh` deliberately takes macOS's legacy GL 2.1 context
because `gfx_opengl.c` emits `#version 120` shaders, and the GL3 backend unconditionally calls
`glGenVertexArrays`, which that context does not have. The consequence is that ImGui cannot
restore the bound shader program or array buffer itself (its own source says so), so
`ge_imgui.cpp` saves and clears both around the draw. Both files carry that reasoning in full.

**Tracy 0.14.1 (BSD-3-Clause).** Fetched and built by `tools/fetch_deps_windows.ps1` into
`~/.n64tvos/tracy-win`, never vendored, same terms as Lua and ImGui. Backs the frame-cost
profiling `getv/port/include/ge_tracy.h` wraps -- see that file's own header for why the wrapper
exists and where its one deliberate deviation from upstream's disabled-branch macro is.
`TracyClient.cpp` is a literal unity build (it `#include`s the client/common sources itself), so
compiling that one file is the entire client library; only the C API header (`tracy/TracyC.h`)
and its two dependency headers are copied out, since this port's own code is C and the C++ macro
API is unused. BSD-3-Clause imposes nothing on the rest of the tree, and the build works without
it -- absent `libtracy.a`, `GE_WITH_TRACY` is not defined and ge_tracy.h's own fallback macros
make every instrumented call site a no-op. It is additionally inert unless `TRACY_ENABLE` was
also defined at compile time, which only happens together with `GE_WITH_TRACY`. Upstream is
`https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.zip`, sha256
`908f3a2917fa86a247abfcf85dcf04bad1db6986a4d40f94b70512f3e9e98d5b`, checked by the fetch script
before anything is compiled into the game.

**`Graslu/1964GEPD` -- GPL-2.0, quarantined (checked 2026-08-24).** A fork of Joel Middendorf's
1964 emulator (1999-2002) carrying GoldenEye and Perfect Dark fixes, notably around input and
frame pacing. It joins GoldenRecomp, `cblock85/GoldenEye64Recomp` and `chrissotraidis/goldenpad`
on the do-not-read list, and for a reason worth stating explicitly: **permission from the fork's
maintainer would not be sufficient.** The upstream emulator is someone else's GPL work, so a fork
maintainer can relicense only the parts they wrote themselves, and separating those from a GPL
codebase is precisely the entanglement this rule exists to avoid.

What may be taken from it is nothing at all in the way of code, and everything in the way of
*which problems are worth solving*: behaviour is not copyrightable, and a list of symptoms is not
a derivative work. That distinction costs this project little, because the asymmetry runs the
other way. That project patches a running emulator from the outside with no access to the game's
source. This one has the decompiled source and can fix the same class of defect at its origin.
A bug list is the useful artefact; the patches are not.

## 9. Related

- `docs/LICENSING.md` - provenance for the repository as a whole, including the ROM and assets.
- `getv/port/PROVENANCE.md` - the file-level origin record for `getv/port/`.
- `getv/patches/thirdparty/README.md` - operational notes on the patch.
