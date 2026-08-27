# Setup

The full step-by-step guide, with the expected output of every command and a troubleshooting
section, is [`docs/SETUP.md`](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/SETUP.md)
in the repository. This page is the shape of it, so you know what you are agreeing to before
you start.

## You need your own ROM

Nothing in the repository contains game data and nothing ever will. You need a dump of the
NTSC (US) cartridge.

| Property | Value |
|---|---|
| Size | 12,582,912 bytes exactly |
| Byte order | Big-endian `z64` |
| Header magic | `80371240` |
| Internal name | `GOLDENEYE` |
| SHA-1 | `abe01e4aeb033b6c0836819f549c791b26cfde83` |

That SHA-1 is the value in `ge007.u.sha1` in the decompilation, so a dump matching it is
byte-identical to what a correct US build produces. A `.n64` or `.v64` extension, or a header
of `37804012`, means the file is byte-swapped and has to be converted to big-endian first.

Every texture, model, animation, sound bank and level layout is read out of your dump at build
time and emitted as C. Around 746 of the translation units this build compiles are generated
that way, which is why the ROM is not optional and why a prebuilt binary is not something that
can be handed out.

## Four things get fetched or supplied

A fresh clone is deliberately incomplete:

- **The decompilation** is cloned from `n64decomp/007` and patched.
- **Fifteen third-party port-layer files**, the Fast3D renderer and audio mixer inherited from
  sm64ex, are fetched from a pinned upstream commit by `tools/fetch-thirdparty.sh`. Their
  redistribution terms are unresolved, so they are not vendored into this repository.
- **SDL2 2.30.9** is supplied by you in `deps/SDL2-2.30.9` and built from source.
- **Your ROM**, which the asset generation step reads.

Lua (for mods) and Dear ImGui (for the launcher) are optional at every level. Without them the
build omits the feature and the entry points compile away.

## The order

```bash
tools/fetch-thirdparty.sh fetch                              # the fifteen files
git clone https://github.com/n64decomp/007 vendor/ge-decomp  # the game's C source
# apply getv/patches/0001-source.patch, place your ROM, generate the asset
# sources from it, then apply getv/patches/0002-assets.patch
cd getv && ./build_mac.sh sdl && ./build_mac.sh all && ./build_mac.sh run
```

The asset-generation step is the one that cannot be shortened. It is a sequence of extraction
and code-generation passes rather than a single command, and skipping any of them produces a
tree that either fails to compile or misbehaves quietly. `docs/SETUP.md` sections 2.4 and 3
walk through it.

## Per platform

**macOS.** Needs macOS 13 or newer on Apple silicon, the Xcode Command Line Tools, CMake,
Python 3, and the stock `/bin/bash` 3.2. Nothing newer. `./build_mac.sh sdl` builds SDL2 into
`~/.n64tvos/sdl2-mac`, deliberately outside the repository because the repository path contains
a space and that has broken header search paths before. Once per machine.

**Linux.** `getv/build_linux.sh` takes the same targets.

```
sudo apt install build-essential pkg-config libsdl2-dev libgl1-mesa-dev
CC=gcc ./getv/build_linux.sh all
```

**Windows.** Native, with mingw-w64. No MSYS2, no Cygwin, no WSL. One command installs the
toolchain, SDL2, GLEW, Lua and Dear ImGui:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_deps_windows.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File getv\build_windows.ps1 -Target all
```

The Windows build is driven from PowerShell on purpose: it needs a compiler, not a working
POSIX emulation layer.

## What a good build looks like

```
  mac FAILED: src/tlb_manage.c
mac game: 167 built, 1 failed
mac assets: 746 built, 0 failed
mac audio: 40 built, 0 failed
mac port layer: 60 built, 0 failed
```

**The one failure is expected.** `src/tlb_manage.c` programs the N64's MIPS R4300 translation
lookaside buffer. There is no TLB here and nothing links against it, so it is left to fail
rather than papered over with a stub. Seven further N64-hardware and SGI-dev-host files are
excluded by name in the build script for the same reason.

Any second name in a `FAILED:` line is a real problem. Check all the counts rather than
grepping for one: a broken audio file shows up only as a changed number.
