# Setup

This is the long-form build guide for Goldeneye-Native. It assumes you are comfortable with a
terminal and have never seen this project or any other decompilation port. Every command below
is meant to be pasted as written. Where a step exists for a non-obvious reason, the reason is
given, because the two most surprising parts of this build - you supply the ROM, and you supply
the renderer sources - are both things you would otherwise assume were a mistake.

`README.md` is the short version. This document is the exhaustive one.

## If you just want it built

```bash
bash tools/install.sh
```

On Windows, in PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1
```

Either one runs everything in this document, in the order it has to happen in, and skips any
step already done so re-running is how you resume. It never downloads a ROM and never
runs `sudo`; where a system package is missing it prints the command for your package manager
and stops.

Read the rest of this document when the installer stops on something, or when you want to know
why a step is where it is. Every ordering constraint the installer encodes is explained below,
and several of them are the kind that fail silently rather than loudly: the namespacing pass in
3.6 corrupts the tree if it runs twice, `enable_bg_extraction.py` must run before extraction
rather than after, and `0002-assets.patch` goes on after the namespacing pass and not before.

Read in order:

1. [What you need before starting](#1-what-you-need-before-starting)
2. [Getting the source](#2-getting-the-source)
3. [Supplying your own copy of the game](#3-supplying-your-own-copy-of-the-game)
4. [Building](#4-building)
5. [Running](#5-running)
6. [Controllers](#6-controllers)
7. [Troubleshooting](#7-troubleshooting)
8. [Verifying it works](#8-verifying-it-works)

A fresh clone of this repository does not build. Three things are deliberately absent from it and
you have to put them there: the decompiled game source, fifteen third-party port-layer files, and
the SDL2 source tree. Sections 2 and 3 cover all three. If you skip them, section 7 tells you what
the resulting errors look like.

---

## 1. What you need before starting

### Hardware and OS

| Requirement | Detail |
|---|---|
| macOS 13 or later | The build hard-codes the target triple `arm64-apple-macos13.0`. |
| Apple silicon (arm64) | The same triple. There is no Intel build and no universal build. |

Intel Macs are not supported. The target triple in `getv/build_mac.sh` is a literal `arm64-...`,
not something derived from the host, so an Intel Mac will produce arm64 objects it cannot link
into anything it can run. Windows, Linux and tvOS are likewise not buildable today.

Check what you have:

```bash
sw_vers -productVersion
uname -m          # must print: arm64
```

### Shell

`getv/build_mac.sh` and `tools/fetch-thirdparty.sh` both begin `#!/usr/bin/env bash`.

**The minimum is bash 3.2 - the `/bin/bash` that ships with macOS. You do not need to install a
newer bash.** The parallel compile step used to use `mapfile`, which is a bash 4 builtin, and
older revisions of `README.md` told you to `brew install bash` because of it. That is no longer
true: the read loop in `run_batch()` is now a `while IFS= read -r` loop, which 3.2 has. Nothing
else in either script needs bash 4 - the features used are arrays, `local -a`, `+=` on arrays,
`${BASH_SOURCE[0]}`, `set -uo pipefail` and process substitution, all of which are 3.2 features.

Verified: a complete `./build_mac.sh all` was run under `/bin/bash` 3.2.57 with `PATH` restricted
to `/usr/bin:/bin:/usr/sbin:/sbin`, and produced the correct object counts and a working binary.

You can confirm your stock bash is 3.2 and that this is fine:

```bash
/bin/bash --version | head -1
# GNU bash, version 3.2.57(1)-release (arm64-apple-darwin25)
```

### Xcode Command Line Tools

Supplies `clang`, `xcrun`, `ar`, `lipo`, `git` and the macOS SDK. The build resolves the SDK with
`xcrun -sdk macosx --show-sdk-path`, so if `xcrun` is missing or unconfigured, every compile fails
with an empty `-isysroot`.

```bash
xcode-select --install
```

If it is already installed that command says so and exits. Verify:

```bash
xcrun -sdk macosx --show-sdk-path
# /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.2.sdk
clang --version | head -1
```

A full Xcode installation is not required. The Command Line Tools alone are enough; the path
above simply reflects a machine that also has Xcode.

### Homebrew

Needed only to install CMake. If you have CMake by other means you can skip Homebrew entirely.

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

### CMake

Used once, by `./build_mac.sh sdl`, to build SDL2. Not used by any other step.

```bash
brew install cmake
cmake --version
```

### Python 3

Used by the asset-generation scripts in section 3. Those scripts import only the standard library
(`gzip`, `io`, `os`, `re`, `subprocess`, `sys`, `struct`), so no `pip install` is needed. The
`python3` from the Command Line Tools is sufficient.

```bash
python3 --version
```

### git

Comes with the Command Line Tools. You need it for this repository, for the decompilation, and for
`tools/fetch-thirdparty.sh`, which reads files out of a git object store rather than a working
tree.

```bash
git --version
```

### SDL2 - source, not Homebrew

**Do not `brew install sdl2`.** The build does not look for it and will not use it. SDL2 is built
from source, for arm64, and installed to `~/.n64tvos/sdl2-mac`, which is outside the repository.
There are two reasons, both recorded in the header comment of `getv/build_mac.sh`:

- A Homebrew running under Rosetta produces an **x86_64** SDL2, which cannot be linked into an
  arm64 binary. This is a silent trap: `brew install sdl2` succeeds, and the failure only appears
  at link time as undefined symbols.
- The install prefix is deliberately outside the repository because the repository path contains a
  space (`.../Code Projects/...`), and a space in a header search path has broken this build before.

The SDL2 source is **not in a fresh clone.** `deps/` is listed in `.gitignore` and nothing under it
is tracked. You must supply `deps/SDL2-2.30.9` yourself - see section 2.3.

### xcodegen

**Not needed.** `xcodegen` generates the Xcode project for the tvOS target from `getv/project.yml`.
The macOS build never invokes it, and the tvOS target is on hold and does not currently build.
Ignore any instruction elsewhere that tells you to install it, unless you are working on tvOS.

---

## 2. Getting the source

There are three separate acquisitions. Only the first is this repository.

### 2.1 Clone this repository

```bash
git clone https://github.com/SegfaultEvan/goldeneye-native.git
cd goldeneye-native
```

At this point `git ls-files | wc -l` reports about 83 files. That is the whole tracked tree: the
port layer's own code, the patches, the documentation and the tooling. It is not a build tree.

### 2.2 Fetch the third-party port-layer sources

Fifteen files that the renderer and the audio mixer are built from are **not in this repository**.
They are inherited from sm64ex, which took the renderer from `Emill/n64-fast3d-engine`, and the
redistribution terms on that lineage are unresolved - the notice this project actually inherited
forbids binary redistribution outright, and GitHub classifies the upstream repository
`NOASSERTION`. Rather than vendor code whose terms are unsettled, the repository keeps only its own
work on those files, as a patch, and you fetch the unmodified upstream text yourself from a pinned
commit. This is the same bring-your-own arrangement the ROM is under, and for a related reason.

`docs/THIRD_PARTY.md` is the full account. `getv/port/PROVENANCE.md` is the file-level history.

From the repository root:

```bash
tools/fetch-thirdparty.sh fetch
```

`fetch` is also the default, so a bare `tools/fetch-thirdparty.sh` does the same thing.

Expected output - one line per file, then a count:

```
upstream: /path/to/goldeneye-native/vendor/sm64ex-cache.git @ d7ca2c04364a6dd0dac58b47151e04e26887e6f0
  getv/port/fast3d/gfx_cc.c
  getv/port/fast3d/gfx_cc.h
  getv/port/fast3d/gfx_opengl.c
  ... eleven more ...
  getv/port/fs/fs.h
fetch-thirdparty: 15 files in place
```

What it did:

1. Made a bare, depth-1 clone of `https://github.com/sm64pc/sm64ex.git` in
   `vendor/sm64ex-cache.git`, fetching the pinned SHA `d7ca2c04364a6dd0dac58b47151e04e26887e6f0`
   directly. Fetching a bare SHA rather than a branch means the pin keeps resolving if upstream's
   default branch moves or the repository is archived.
2. Copied the fifteen files named in `getv/patches/thirdparty/MANIFEST` into place, reading them
   with `git show` out of the object store rather than from a checkout, so a dirty working tree
   cannot leak in.
3. Applied `getv/patches/thirdparty/0001-getv-port-layer.patch`, which carries every change this
   project made to those files. It is a zero-context diff (`diff -U0`) of about 298 KB, and it is
   the substance of the port's rendering work, not a thin adaptation layer.

Confirm what is on disk:

```bash
tools/fetch-thirdparty.sh status
```

```
upstream : https://github.com/sm64pc/sm64ex.git
commit   : d7ca2c04364a6dd0dac58b47151e04e26887e6f0
patch    : /path/to/goldeneye-native/getv/patches/thirdparty/0001-getv-port-layer.patch

  present  getv/port/fast3d/gfx_cc.c            <- src/pc/gfx/gfx_cc.c
  present  getv/port/fast3d/gfx_cc.h            <- src/pc/gfx/gfx_cc.h
  ...
  present  getv/port/fs/fs.h                    <- src/pc/fs/fs.h
```

Every line must read `present`. Any `ABSENT` means a partial fetch; see section 7.6.

A stronger check, which re-derives all fifteen files from the pin plus the patch in a scratch
directory and compares them byte for byte:

```bash
tools/fetch-thirdparty.sh verify
```

```
ok       getv/port/fast3d/gfx_cc.c
...
fetch-thirdparty: 15/15 files match pristine + patch
```

The other subcommands are `clean` (remove the fetched files, returning the tree to its published
state) and `regen` (rewrite the patch from the current working tree - only relevant if you edit one
of the fifteen files, in which case the patch is the only place that edit is recorded).

**If you skip this step**, the build stops before it starts. `build_mac.sh`'s `require_thirdparty()`
preflight checks for `getv/port/fast3d/gfx_pc.c` and, not finding it, prints:

```
error: third-party port sources are missing.
       run tools/fetch-thirdparty.sh fetch   (see docs/THIRD_PARTY.md)
```

and exits 1. The check exists because without it the compile emits a long list of missing headers
with nothing in it that points at the cause.

### 2.3 Supply the SDL2 source

`deps/` is not tracked either. `./build_mac.sh sdl` expects the SDL2 **2.30.9** source tree at
`deps/SDL2-2.30.9`, relative to the repository root - that exact directory name, because the path
is hard-coded in the script:

```bash
SDLSRC="$HERE/../deps/SDL2-2.30.9"
```

The version is pinned, not incidental: the CMake invocation passes
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` specifically because SDL 2.30.9 declares
`cmake_minimum_required(3.0)`, which CMake 4 refuses outright.

Obtain the 2.30.9 source release from SDL's own releases and unpack it so that
`deps/SDL2-2.30.9/CMakeLists.txt` exists. The release tarball for that version is published at
`https://github.com/libsdl-org/SDL/releases/tag/release-2.30.9`. (The exact tarball filename and
its checksum are not recorded anywhere in this repository, so they are not asserted here - take
them from SDL's release page.)

Verify placement:

```bash
ls deps/SDL2-2.30.9/CMakeLists.txt
grep -E 'SDL_(MAJOR_VERSION|MINOR_VERSION|PATCHLEVEL)' deps/SDL2-2.30.9/include/SDL_version.h
# #define SDL_MAJOR_VERSION   2
# #define SDL_MINOR_VERSION   30
# #define SDL_PATCHLEVEL      9
```

### 2.4 Clone and prepare the decompilation

The game's C source is not in this repository either. It comes from the `n64decomp/007`
decompilation, which is cloned into `vendor/ge-decomp` - also gitignored, also untracked.

```bash
git clone https://github.com/n64decomp/007 vendor/ge-decomp
cd vendor/ge-decomp
git apply ../../getv/patches/0001-source.patch
```

The port's changes to the decompilation are split across two patches, because one of them has to
be applied *after* the assets exist:

- **`0001-source.patch`** - this one, applied now. Roughly 1.4 MB across 140 files: 131 under
  `src/`, eight under `include/`, and one new generator under `tools/`. It touches no asset paths
  at all, and it does not touch `scripts/`. It also creates five files that do not exist upstream,
  including `tools/gen_propdef_layout.py`, which section 3.5 calls.
- **`0002-assets.patch`** - 212 KB across eight generated asset files. It cannot be applied yet,
  because the files it patches do not exist until you generate them from your ROM. Section 3.5
  applies it at the correct point.

Applying `0002-assets.patch` here will fail, and that failure is expected rather than a problem
with your checkout.

Do not continue past this point until you have the ROM in place. Return to the repository root:

```bash
cd ../..
```

---

## 3. Supplying your own copy of the game

**No ROM, no extracted assets and no game data are distributed with this repository, and none ever
will be.** This is not a licensing formality that a mirror somewhere quietly works around: the
build genuinely has nothing to compile without it. The decompilation is a description of the
game's code, and every texture, model, animation, sound bank and level layout is read out of your
cartridge dump at build time and emitted as C. Roughly 746 of the translation units this build
compiles are generated that way.

You need your own legal copy of the **NTSC (US)** cartridge, dumped to a file.

### 3.1 What a correct dump looks like

| Property | Value |
|---|---|
| Size | 12,582,912 bytes exactly |
| Byte order | Big-endian `z64` |
| Header magic | `80371240` |
| Internal name | `GOLDENEYE` |
| SHA-1 | `abe01e4aeb033b6c0836819f549c791b26cfde83` |

The SHA-1 is not this project's invention. It is the value in `ge007.u.sha1` in the decompilation
itself, so a dump with that hash is byte-identical to what a correct US build must reproduce.

### 3.2 Where to put it

The decompilation's extraction scripts read the ROM from a fixed name at the root of the decomp
checkout:

```
vendor/ge-decomp/baserom.u.z64
```

The convention this project uses is to keep dumps in `roms/` at the repository root and symlink one
into place, so that the ROM lives in exactly one directory and `.gitignore` covers it there. From
the repository root:

```bash
mkdir -p roms
cp /wherever/your/dump.z64 roms/ge007.u.z64
cd vendor/ge-decomp
ln -sf ../../roms/ge007.u.z64 baserom.u.z64
cd ../..
```

`.gitignore` blocks `roms/`, every `*.z64` / `*.n64` / `*.v64` / `*.elf`, `vendor/` and `deps/`
entirely, every `getv/build-*` directory (they hold object files compiled from extracted ROM data),
`*.o` / `*.a` anywhere, and `*.bmp` frame captures (frame dumps are derived from the ROM's own
art). Do not defeat those rules. A `git add -A` without them would commit derived game data.

### 3.3 Verify before you build

```bash
ls -l roms/ge007.u.z64
# -rw-r--r--  1 you  staff  12582912  ... roms/ge007.u.z64

shasum -a 1 roms/ge007.u.z64
# abe01e4aeb033b6c0836819f549c791b26cfde83  roms/ge007.u.z64

xxd -l 4 roms/ge007.u.z64
# 00000000: 8037 1240                                .7.@

dd if=roms/ge007.u.z64 bs=1 skip=32 count=20 2>/dev/null | xxd
# 00000000: 474f 4c44 454e 4559 4520 2020 2020 2020  GOLDENEYE
```

Cross-check against the decompilation's own recorded hash:

```bash
cat vendor/ge-decomp/ge007.u.sha1
# abe01e4aeb033b6c0836819f549c791b26cfde83  build/u/ge007.u.z64
```

The path on the right differs because that file describes the decomp's own build output; only the
hash matters here.

### 3.4 If it is wrong

- **Header reads `37804012`, or the file is named `.n64` / `.v64`.** The dump is byte-swapped.
  It must be converted to native big-endian `z64` before anything will work. Extraction will read
  garbage otherwise.
- **Wrong size.** It is not a plain US cartridge dump - it may be trimmed, padded, or carry a
  dumper header.
- **Right size, wrong SHA-1.** It is a different region or a modified ROM. Only the US ROM is
  supported; the build defines `VERSION_US`, `LANG_US` and `REFRESH_NTSC`.
- **Missing entirely.** `scripts/extract_baserom.u.sh` has nothing to read, so no asset sources are
  generated, and the build reports `mac assets: 0 built, 0 failed` - see section 7.2.

### 3.5 Generate the asset sources

Run these from inside `vendor/ge-decomp`, in this order. The order matters and was
established by building from a clean checkout.

```bash
cd vendor/ge-decomp
python3 ../../tools/enable_bg_extraction.py
bash scripts/extract_baserom.u.sh
python3 scripts/generate_chr_c.py
python3 scripts/generate_gun_c.py
python3 scripts/generate_prop_model_c.py
python3 ../../tools/gen_obseg_blobs.py
python3 scripts/make/sync_imagelist_with_def.py build/imagelist.csv
bash  scripts/make/combine_images_named.sh build/imagelist.csv assets/images/combined
python3 ../../tools/gen_images_segment.py
python3 ../../tools/fix_asset_switchnodes.py
python3 ../../tools/gen_anim_blobs.py
python3 ../../tools/gen_audio_segment.py
python3 ../../tools/gen_asset_fileview.py
python3 tools/gen_propdef_layout.py
```

Notes on the ones that are easy to get wrong:

- `gen_anim_blobs.py` writes `assets/ge_animation_offsets.h`. Without it `src/game/model.c`
  and everything that includes `initanitable.h` fail with a missing header.
- `gen_asset_fileview.py` writes `src/ge_asset_fileview.h`. Without it `model.c` fails.
- `gen_port_decls.py` is **not** part of this sequence, despite what its name suggests. It
  regenerates `src/ge_port_decls.h` from scratch, and `0001-source.patch` already ships a
  curated version of that header. Running it discards the curated one and reinstates the
  upstream prototype for `sub_GAME_7F0B7F84`, which the patch changed from four arguments
  returning `void` to five returning `s32`. The result is that `src/game/bg.c` no longer
  compiles and the link fails with about thirty undefined `bg*` symbols, none of which
  mention the header. It is a development tool for when new code needs new declarations;
  it is not a build step.
- `gen_images_segment.py` converts `assets/images/combined/combined.bin` into a C array.
  Upstream turns that file into an object with `ld -r -b binary`, which is a GNU extension
  that Mach-O has no equivalent for, so the bytes are emitted as C instead. It must run
  after `combine_images_named.sh`, which is what produces its input.
- `tools/gen_propdef_layout.py` lives in the decomp, not in this repository, and is created
  by `0001-source.patch`. It will not exist until that patch is applied.
- `enable_bg_extraction.py` must run **before** extraction, not after. The decomp ships 25 of the
  34 `assets/obseg/bg/*.bin` rows in `scripts/filelist.u.csv` with their extract flag set to `0`,
  because upstream builds those backgrounds from checked-in `.c` files and never needs the raw
  blobs. This port compiles the blobs, so skipping them produces 25 undefined symbols at link
  time and nothing earlier in the build hints at why. The offsets and sizes in those rows are
  correct; only the flag is wrong. Running it a second time is harmless.

### 3.6 Namespace the asset symbols

Getools emits every asset as plain C with generic global names - `tile_0`, `padlist`, `intro`,
`ai_0`, `footer` - and emits the *same* names in every level's file. Linked together, all 29 stan
files and all 50 setup files define the same symbols, so an internal reference such as

```c
StandFileHeader Tbg_dam_all_p_stanZ = { NULL, &tile_0, ... };
```

binds to whichever object the linker happened to pick, which is alphabetically the first. Before
this pass existed, the Dam's stan header resolved to Tbg_ame's `tile_0` and the Dam's pad list
resolved to a 62-entry list belonging to another level - the Dam has 368. Every level was running
on some other level's data.

`tools/uniquify_asset_symbols.py` fixes that by giving each translation unit its own prefix. Run it
from inside `vendor/ge-decomp`, once per directory:

```bash
python3 ../../tools/uniquify_asset_symbols.py assets/obseg/chr   --recurse
python3 ../../tools/uniquify_asset_symbols.py assets/obseg/gun   --recurse
python3 ../../tools/uniquify_asset_symbols.py assets/obseg/prop  --recurse
python3 ../../tools/uniquify_asset_symbols.py assets/obseg/setup
python3 ../../tools/uniquify_asset_symbols.py assets/obseg/setup/u
python3 ../../tools/uniquify_asset_symbols.py assets/obseg/stan
```

Then, and only then, apply the second patch:

```bash
git apply ../../getv/patches/0002-assets.patch
```

Why it is six invocations rather than one:

- **`chr`, `gun` and `prop` need `--recurse`.** Their models are laid out as
  `<dir>/<name>/Model.c` - 340 props all defining `ModelNode_0x048`, each its own translation
  unit. A flat glob finds only `.inc.c` files, which the tool deliberately skips, so it prints
  nothing at all and still exits 0. With `--recurse` the subdirectory supplies the prefix, giving
  `commguard_Model_` and `desk_lamp2_Model_`.
- **`setup` must not be recursed.** Its 30 level setups sit flat in the directory and take the
  file stem as prefix (`UsetupdamZ_`).
- **`setup/u` is passed directly**, which makes `parent == basename(dir)` and collapses the prefix
  to the bare stem - the same scheme as the flat files, which is what the rest of the tree expects.
- **Never run `assets --recurse`.** It prefixes the symbols in `ge_obseg_blobs.c` as well, so they
  no longer match what `assets/obseg/file_resource_table.inc.c` references, and the link fails on
  undefined character models (`CcommguardZ`, `CdjbondZ` and others).
- **`stan` is easy to leave out and the omission is silent.** Its 29 files ship from the
  decompilation with Getools' generic names, and nothing else renames them, so without this pass
  the archive ends up with 29 definitions of `_tile_0`. That is not a link error: duplicate
  definitions in a static archive are resolved by picking one, so the build succeeds and 28
  levels quietly bind to another level's collision data. This is the exact fault the whole pass
  exists to prevent, and it was missing from this list until a contributor pointed it out.
- **`0002-assets.patch` goes on afterwards.** Run over `assets/font` the tool double-prefixes an
  already-prefixed symbol (`font_fontBankGothic_fontBankGothic_kerning`) while leaving the uses
  alone, which breaks both font translation units. The patch carries the corrected files.

`--dry-run` works as a regression gate - it exits non-zero if anything is still left to rename:

```bash
python3 ../../tools/uniquify_asset_symbols.py assets/obseg/setup --dry-run
```

Four of the five invocations come back clean. `setup/u` is the exception: once
`0002-assets.patch` is applied it always reports `1 file(s) still need namespacing` and exits 1,
naming `UsetuplenZ.c`. That is expected. The patch supplies that file with a `u_UsetuplenZ_`
prefix, while the direct-directory invocation expects the bare `UsetuplenZ_`, so the tool sees a
prefix it did not produce and offers to add its own on top.

**Do not act on it.** Re-running the tool over `setup/u` after applying the patch rewrites those
17 symbols a second time and breaks the file. Run the namespacing pass first, apply the patch
second, and treat that single report as the known steady state.

One caveat worth knowing, because it fails silently. The tool reads a file's globals by compiling
it and running `nm`, so a file that does not compile is left colliding, and reports as one `SKIP`
line among hundreds of `ok` lines. That is how all three `setup/{u,j,e}/UsetuplenZ.c` kept a bare
`propDefs` long after every other setup file had been namespaced, leaving ARCHIVES and CUBA sharing
one propDef stream that ARCHIVES then walked off the end of. Skips are printed again as a block at
the end and the tool exits non-zero; do not ignore that.

#### The PAL and Japanese setup tables are not built

`assets/obseg/setup/e` and `assets/obseg/setup/j` hold the same eight filenames as `setup/u`. The
prefix scheme above is per file stem, so seven of the eight end up defining identical globals in
all three directories - `UsetupcradZ_padlist` and so on. Compiling all three would let the linker
bind Cradle, Silo, Jungle, Train, Statue Park, `UsetupdestZ` and the multiplayer Archives to
whichever copy it saw first, alphabetically `e/`, the PAL data, in a `VERSION_US` build. The PAL
files are not merely a different encoding of the same tables - all seven differ from their US
counterparts and every one of them is larger.

The build therefore excludes both directories (`build_mac.sh`, the `mac assets` find). Nothing
outside them references their symbols and `file_resource_table.inc.c` asks for the bare name, so
excluding them is the whole fix. It also resolves the related case the tool cannot: the top-level
`stagesetup UsetuplenZ`, which the engine looks up by name and which is therefore deliberately left
unprefixed in all three directories.

## 4. Building

Everything below is run from the `getv/` directory.

```bash
cd getv
```

`./build_mac.sh` takes exactly one subcommand. With none, or an unrecognised one, it prints its
usage and exits:

```
usage: ./build_mac.sh {sdl|lib|port|app|all|run|env}
  sdl  = build SDL2 2.30.9 arm64 from deps/ into /Users/you/.n64tvos/sdl2-mac (once)
  lib  = compile game + assets + audio + port layer for arm64 macOS
  port = recompile getv/port/** and the harness only (seconds)
  app  = link /path/to/goldeneye-native/getv/build-mac/goldeneye
  run  = launch it
```

### 4.1 `env` - show the resolved paths

Run this first. It resolves the SDK, the SDL prefix, the target triple and the output paths, and
does nothing else. It is the cheapest way to find out that `xcrun` is broken.

```bash
./build_mac.sh env
```

```
SDK=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.2.sdk
SDL=/Users/you/.n64tvos/sdl2-mac
TARGET=arm64-apple-macos13.0
BUILD=/path/to/goldeneye-native/getv/build-mac
BIN=/path/to/goldeneye-native/getv/build-mac/goldeneye
```

If `SDK=` is empty, stop and fix the Command Line Tools (section 7.3).

**When to use:** first, and any time a path-related failure needs diagnosing.

### 4.2 `sdl` - build SDL2, once

```bash
./build_mac.sh sdl
```

Configures and builds `deps/SDL2-2.30.9` with CMake for `arm64`, `Release`, static only
(`-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF`), in `~/.n64tvos/build-sdl2-mac`, and installs
to `~/.n64tvos/sdl2-mac`. CMake's own chatter is discarded; you get two lines:

```
SDL2 -> /Users/you/.n64tvos/sdl2-mac
Non-fat file: /Users/you/.n64tvos/sdl2-mac/lib/libSDL2.a is architecture: arm64
```

**That second line is the check that matters.** It must say `arm64`. If it says `x86_64`, the link
in section 4.5 will fail and everything after it is wasted time.

**When to use:** once per machine, and again only if you change SDL versions or delete
`~/.n64tvos/`.

### 4.3 `lib` - compile everything

```bash
./build_mac.sh lib
```

Compiles four batches in parallel and reports each. Parallelism is capped by `GETV_JOBS`, which
defaults to 6 - deliberately not `nproc`, so a build does not monopolise the machine:

```bash
GETV_JOBS=10 ./build_mac.sh lib     # if you want it to
```

Expected output:

```
  mac FAILED: src/tlb_manage.c
mac game: 167 built, 1 failed
mac assets: 746 built, 0 failed
mac audio: 40 built, 0 failed
mac port layer: 23 built, 0 failed
```

**`mac game: 167 built, 1 failed` is a correct, successful build.** Read the next subsection before
you conclude anything is broken.

**When to use:** the first build, and after any change to the decompilation, the assets or the
compile flags.

#### The one expected failure

`src/tlb_manage.c` programs the N64's MIPS R4300 translation lookaside buffer. There is no TLB to
program on a Mac and nothing in the linked binary references it, so it is left to fail rather than
being stubbed out or excluded - the failure is the honest record of a file that has no meaning on
this target. The concrete error, if you compile it by hand, is a pair of type redeclaration
conflicts against `src/bondgame.h`:

```
src/tlb_manage.c:108:12: error: redeclaration of '_gameSegmentRomStart' with a different type:
      'u8 *' (aka 'unsigned char *') vs 'u32 *' (aka 'unsigned int *')
```

Seven further N64-hardware and SGI-dev-host files are excluded by name in the build script for the
same reason, and so never appear in the counts at all: `usb.c`, `rmon.c`, `sched.c`, `ramrom.c`,
`init.c`, `indy_comms.c`, `indy_commands.c`. They are excluded explicitly rather than left to fail
because switching `-w` to `-Wno-everything` (needed so `-Werror=return-type` actually takes effect)
also suppressed the diagnostics that used to stop them, and they began compiling - which would
have turned logging stubs into code that writes real RCP and PI registers.

So: **one failure, always the same one, always `src/tlb_manage.c`.** Any second name in a
`mac FAILED:` line is a real problem.

#### Where the counts come from

If you want to confirm the numbers rather than trust them, they are exactly the file counts the
script's own `find` expressions produce:

```bash
cd ../vendor/ge-decomp
{ find src -name '*.c' -not -path 'src/libultra/*' -not -path 'src/libultrare/*' \
      -not -name 'ge_layout_audit.c' -not -name 'ge_asset_fileview_check.c'
  find src/libultra/gu -name '*.c'; } \
  | grep -vE '/(ramromreplay\.c|audi\.c|usb\.c|rmon\.c|sched\.c|ramrom\.c|init\.c|indy_comms\.c|indy_commands\.c)$' \
  | wc -l          # 168 = 167 built + 1 failed
find assets -name '*.c' ! -name '*.inc.c' \
      ! -path 'assets/obseg/setup/e/*' ! -path 'assets/obseg/setup/j/*' \
    | wc -l                                                      # 746
find src/libultra/audio src/libultrare/audio -name '*.c' | wc -l  # 40
cd ../../getv
```

The port layer's 23 is 21 sources under `getv/port/{fast3d,src,audio}/` plus the two harness files,
`getv/Sources/ge_tvos_main.c` and `getv/port/mac/ge_mac_main.c`.

### 4.4 `port` - recompile only the port layer

```bash
./build_mac.sh port
```

```
mac port layer: 23 built, 0 failed
```

Recompiles the 21 port sources and the two harness objects and nothing else. Measured at about 23
seconds on an Apple M1. This is the loop you live in when working on the renderer, input, audio or
configuration - everything under `getv/port/`.

It refuses to run against an empty tree:

```
no objects in /path/to/getv/build-mac/obj -- run './build_mac.sh lib' once first
```

**When to use:** after editing anything under `getv/port/` or `getv/Sources/`. Follow it with
`app`.

### 4.5 `app` - archive and link

```bash
./build_mac.sh app
```

```
mac libge.a:  31M, 974 members
ld: warning: reducing alignment of section __DATA,__common from 0x8000 to 0x4000 because it exceeds segment maximum alignment
mac binary: /path/to/goldeneye-native/getv/build-mac/goldeneye ( 18M, arm64)
```

The `ld: warning` about `__DATA,__common` alignment is expected and benign; it appears on every
successful link. Warnings reading `was built for newer` are filtered out by the script - they come
from SDL2 having been compiled with the SDK's default deployment target rather than this build's,
and the code is arm64 either way.

974 archive members is 976 objects minus the two harness objects, which stay outside the archive
because they carry `main()` and `SDL_main()` and are the roots the link is discovered from.

Two details worth knowing, because both look like mistakes and are not:

- **It archives into `libge.a` rather than linking the objects directly.** That is a correctness
  requirement. A static archive pulls a member only when it resolves an undefined symbol, so the
  objects for `init.c`, `sched.c` and `rmon.c` are never dragged in. Linking `build-mac/obj/*.o`
  directly has no such filter and fails with around 30 undefined N64 linker-script and hardware
  symbols.
- **`-dead_strip` is required, not an optimisation.** Without it the link fails on six
  undefined symbols - `osEepromRead`, `osEepromWrite`, `osViSetMode`, `osPiReadIo` and the two
  `_{e,j}fontchardataSegmentRomStart` linker-script symbols - every one referenced only from a
  function this port never calls. `ld64` dead-strips before it checks for undefined symbols, so a
  reference from a dead atom is not an error. Removing the flag does not link more of the game; it
  just breaks the build.

On failure the last line is `LINK FAILED`, preceded by up to 30 lines of linker diagnostics.

**When to use:** after `lib` or after `port`.

### 4.6 `all` - `lib` then `app`

```bash
./build_mac.sh all
```

The normal full build. Complete expected output:

```
  mac FAILED: src/tlb_manage.c
mac game: 167 built, 1 failed
mac assets: 746 built, 0 failed
mac audio: 40 built, 0 failed
mac port layer: 23 built, 0 failed
mac libge.a:  31M, 974 members
ld: warning: reducing alignment of section __DATA,__common from 0x8000 to 0x4000 because it exceeds segment maximum alignment
mac binary: /path/to/goldeneye-native/getv/build-mac/goldeneye ( 18M, arm64)
```

Measured at 21 seconds wall on an Apple M1 with warm filesystem caches, at the default
`GETV_JOBS=6`. `README.md` says "about five minutes"; that figure appears stale, but a first build
on a cold cache, on a slower machine, or with the ROM extraction still fresh will take longer than
21 seconds. Treat the number as an order of magnitude, not a guarantee.

Note that there is no incremental check: `run_batch` compiles every file every time. `all` is
always a full rebuild, which is why `port` exists.

**When to use:** the first build, and any time you are unsure what state the tree is in.

### 4.7 `run` - launch the binary

```bash
./build_mac.sh run
```

Execs `build-mac/goldeneye`, forwarding any further arguments. A native process just inherits the
environment, so every `GETV_*` variable works exactly as exported:

```bash
GETV_EXIT_FRAME=61 ./build_mac.sh run
./build_mac.sh run --resolution=1920x1440
```

If nothing has been linked:

```
no binary -- run './build_mac.sh all'
```

**When to use:** to play, and for the smoke test in section 8.

### 4.8 One optional build-time switch

```bash
GETV_DEBUGMENU=1 ./build_mac.sh lib
```

Adds `-DDEBUGMENU` and prints:

```
  GETV_DEBUGMENU=1 -- debug menu ENABLED. START is repurposed.
```

Leave it unset unless you are working on the game itself. See `docs/MODDING.md`.

---

## 5. Running

```bash
./build-mac/goldeneye
```

or `./build_mac.sh run`, which is the same thing with arguments forwarded.

### 5.1 What happens on first launch

A resizable 1280x960 window opens. Boot progress and diagnostics go to stdout - the port is
verbose by design, and roughly 400 lines on a short run is normal, not a fault.

Lines worth recognising:

```
[getv][config] first run -- wrote a default config; edit it to taste
[getv][config] file /Users/you/Library/Application Support/GoldenEye/goldeneye.cfg | window=1280x960 fps=60 ss=1 controls=5 filtering=2
[getv] GoldenEye tvOS harness starting
[getv] window: 1280x960 windowed, resizable; fullscreen toggle = F11 / Cmd-F / Alt-Enter
[getv] GL_VENDOR=Apple | GL_RENDERER=Apple M1 | GL_VERSION=2.1 Metal - 90.5
[getv] Fast3D up: 0x0 internal, 0x0 output, supersample 1
[getv] input: N64 ports connected = 2 (bitpattern 0x3)
```

`GL_VERSION=2.1` is intentional. `gfx_opengl.c`'s shader generator emits `#version 120` with
`attribute`/`varying`, which a 3.2 core profile rejects outright, so the build requests macOS's
legacy compatibility profile. Do not read it as a fallback path.

`GoldenEye tvOS harness starting` is also expected: the harness is shared verbatim with the tvOS
app target, which is where it was written. It is plain C and SDL with no UIKit in it.

### 5.2 The configuration file

On first run, with no configuration file present anywhere, the game writes a fully commented
template and immediately reads it back:

```
~/Library/Application Support/GoldenEye/goldeneye.cfg
```

**Note the directory is `GoldenEye`, not `Goldeneye-Native`.** This is verified against
`getv/port/src/ge_config.c`, which calls
`gePortUserDataDir("Goldeneye-Native", "GoldenEye", ...)` - on macOS the second argument is the one
that becomes the directory name. Save data uses a different directory; see 5.3.

The first-run write is not a convenience. Several of the port's tuned defaults exist only in that
template - `invert_look = 1` being the case in point - and a default that lives in a file nobody has
generated is not a default. With no template, retail's own default applies, which omits the
invert-look flag, drives pitch down at full rate on stick-up, and pins the camera at the -90 degree
clamp in about a second and a half. A fresh install then opens staring at the floor. That was
reported as a bug twice before the template existed.

The search order, first match wins:

1. `$GETV_CONFIG`
2. `--config=PATH`
3. `goldeneye.cfg` in the same directory as the binary
4. `~/Library/Application Support/GoldenEye/goldeneye.cfg`

Precedence for values: command line > environment > config file > built-in default.

To regenerate the template at any time, overwriting what is there:

```bash
./build-mac/goldeneye --write-config
# [getv][config] wrote /Users/you/Library/Application Support/GoldenEye/goldeneye.cfg
```

`--write-config=PATH` writes somewhere else instead. Both exit without starting the game.
`--help` prints the built-in usage summary; `--list-cheats` prints every named cheat.

Failure to write the template is deliberately non-fatal and near-silent: a read-only home directory
must still boot on built-in defaults.

Every setting is documented in [`CONFIGURATION.md`](CONFIGURATION.md).

### 5.3 Where saves go

Save data is separate from configuration and lands in:

```
~/Library/Application Support/Goldeneye-Native/eeprom.bin
```

512 bytes - GoldenEye saves to the cartridge's serial EEPROM, not a Controller Pak, and 32 bytes of
`smallSave` plus five 96-byte slots fills a 4 K EEPROM exactly. Writes are atomic via a temporary
file and a rename, so a crash mid-save cannot leave a torn image.

Expected first-run line:

```
[getv][save] no existing save at /Users/you/Library/Application Support/Goldeneye-Native/eeprom.bin -- starting blank (512 bytes)
```

and on later runs:

```
[getv][save] loaded 512/512 bytes from /Users/you/Library/Application Support/Goldeneye-Native/eeprom.bin
```

An absent file is a blank cartridge, not an error, and a short read is not fatal either - the game's
own per-slot CRC rejects damaged slots and resets exactly those.

**Installs predating the rename.** The project was previously called GoldenEyeTV, and an install
from that era has its EEPROM under `~/Library/Application Support/GoldenEyeTV/`. If the new
directory has no save and the old one does, the old path is adopted for that run and you will see:

```
[getv][save] using the pre-rename save directory: /Users/you/Library/Application Support/GoldenEyeTV
```

Nothing is copied and nothing is deleted. The file keeps working where it is. If you want it in the
new location, move it yourself:

```bash
mkdir -p ~/Library/"Application Support"/Goldeneye-Native
mv ~/Library/"Application Support"/GoldenEyeTV/eeprom.bin \
   ~/Library/"Application Support"/Goldeneye-Native/eeprom.bin
```

Two environment gates are relevant here: `GETV_SAVE=0` turns persistence off entirely (the probe
then reports no EEPROM, which is the A/B control), and `GETV_SAVEDIR=<dir>` overrides the directory
- used by the round-trip test so it never touches a real save. `save_dir` in the config file does
the same thing.

### 5.4 Keyboard

The keyboard is bound to controller port 0 by default:

| Keys | Action |
|---|---|
| `W` `A` `S` `D` | Move |
| Arrow keys | Look |
| `Space` or `Left Ctrl` | Fire |
| `E` or `Return` | Use / A |
| `Q` | Aim / B |
| `Z` / `X` | Left / right shoulder |
| `Tab` or keypad `Enter` | Pause / Start |
| `Backspace` | Back |
| `I` `J` `K` `L` | D-pad up / left / down / right |
| `F11`, `Cmd-F`, `Alt-Enter` | Toggle fullscreen |

A connected gamepad works alongside the keyboard. Whichever input is held wins, so plugging in a pad
never degrades the keyboard and unplugging it never leaves you stranded.

One exception: when `GETV_EXIT_FRAME` is set, the keyboard pad is present but reports "nothing held"
for the whole run, because that variable marks an automated measurement rather than a play session.
You will see:

```
[getv] input: keyboard pad is PRESENT but IDLE for this run (measurement run; GETV_KEYBOARD_IDLE=0 to type into it)
```

The pad stays *present* rather than being removed, because dropping the controller count to zero
sends the front end to a terminal `MENU_NO_CONTROLLERS` state with no way out. A plain
`./build_mac.sh run` is unaffected. `GETV_KEYBOARD_IDLE=0` forces live input; `=1` forces idle.

---

## 6. Controllers

Any SDL2-recognised gamepad works, through SDL's game controller database. Nothing needs to be
installed or paired beyond whatever macOS itself requires.

On startup the input layer reports what it found:

```
[getv] input: poll live @513ms -- joysticks=0 gamecontrollers=0 ports=0 [none|none|none|none] forced=0 synth=0
[getv] input: dual-analog ON -- one pad presented as N64 ports 0+1 (port 0 = right stick/look, port 1 = left stick/move). GETV_DUALANALOG=0 to disable.
[getv] input: N64 ports connected = 2 (bitpattern 0x3)
```

### How the defaults were chosen

The two decisions that will surprise you are both deliberate, and both are measurements rather than
preferences.

**One physical pad is presented as two N64 controllers.** GoldenEye has eight of Rare's control
styles, four for one controller and four for two. `2.2 galore` and `2.4 goodhead` are the true
dual-analog layouts, and they are two-controller styles because in 1997 dual analog meant literally
two N64 pads. A modern gamepad already has two sticks, so the port presents it as N64 ports 0 and 1
- left stick moving, right stick looking - and defaults to `2.2`. Rare's own shipped default is
`1.1 honey`. Set `controls = 1.1` for that. With three or four players the game forces everyone back
to `1.1` regardless.

**`invert_look` defaults to `1` in the written template.** Not a taste setting. Retail's
`DEFAULT_OPTIONS` omits `OPTION_INVERTLOOK`, which makes `invertPitch` 1, which drives pitch *down*
at full rate on stick-up and pins at the -90 degree clamp in about 1.5 seconds with nothing to
recentre it. Comment the line out if you want retail behaviour; note that unset is not the same as
`0`, because unset lets the save file's own Look Up/Down option decide.

**Button names are positional, not label-based.** `a` means the physically bottom face button on
whatever pad you have - SDL maps the bottom face button to its `A` slot on every controller it
knows, including Nintendo's, where that same button is printed `B`. Defaults: `fire = rt`,
`aim = lt`, `use = b`, `weapon_next = a`, `weapon_prev = none`, `pause = start`.
`fire = rt` / `aim = lt` is the modern-shooter convention rather than a settled fact; GoldenEye's
retail scheme has neither, and swapping them is one line.

`weapon_prev` defaults to `none` on purpose: GoldenEye has no back-cycle button. The retail gesture
is hold-inventory plus tap-fire. A synthesised single-button version exists and is faithful to that
gesture, but it has not been verified against real hardware, so it stays opt-in.

The `gamepad` setting (`auto`, `xbox`, `playstation`, `switch`, `generic`) changes **which glyphs
are printed for on-screen prompts and nothing else.** It cannot make `a` refer to a different
physical button. Set it only when SDL misidentifies a third-party pad.

Full detail, including `deadzone` (default 20 in the template, roughly 9.8% built in) and every
other key: [`CONFIGURATION.md`](CONFIGURATION.md).

---

## 7. Troubleshooting

Symptoms are grouped by what you actually see. All messages quoted here are from the scripts and
sources named beside them.

### 7.1 Third-party port sources are missing

**Symptom** - any of `lib`, `port` or `all` stops immediately:

```
error: third-party port sources are missing.
       run tools/fetch-thirdparty.sh fetch   (see docs/THIRD_PARTY.md)
```

**Cause** - the preflight in `require_thirdparty()` (`getv/build_mac.sh`) found no
`getv/port/fast3d/gfx_pc.c`. You have not run the fetch script, or you ran `clean` afterwards.

**Fix**

```bash
cd /path/to/goldeneye-native
tools/fetch-thirdparty.sh fetch
tools/fetch-thirdparty.sh verify
```

**Caveat worth knowing:** the preflight checks for **one** file. A partial fetch that happens to
include `gfx_pc.c` but is missing, say, `ge_mixer.c` will pass the preflight and fail later with
ordinary compile or link errors. `verify` is the check that catches that; see 7.6.

### 7.2 The decompilation is missing, or the assets were never generated

**Symptom** - the build reports zero of everything, preceded by a `cd` failure:

```
./build_mac.sh: line NNN: cd: /path/to/goldeneye-native/vendor/ge-decomp: No such file or directory
mac game: 0 built, 0 failed
mac assets: 0 built, 0 failed
mac audio: 0 built, 0 failed
```

**Cause** - `vendor/ge-decomp` does not exist. `cmd_lib` runs each `find` inside a subshell that
`cd`s to the decomp root; the `cd` fails, the subshell produces no file list, `xargs` gets nothing
to do, and the counters come out zero. The build script does not use `set -e`, so it carries on and
you get four zero lines rather than one clear error.

**Fix** - sections 2.4 and 3.

**Related symptom** - the decomp is present but `mac assets: 0 built, 0 failed` while `mac game`
reports 167. The decompilation was cloned but the ROM extraction and asset generation in 3.5 were
never run, so `assets/**/*.c` does not exist yet.

**Related symptom** - `mac assets:` reports a large number of failures rather than
`746 built, 0 failed`, and the named files are under `assets/obseg/{chr,prop,gun}/`. That is
`tools/fix_asset_switchnodes.py` not having been run: the script's own header records **156 asset
translation units** (68 chr, 61 prop, 27 gun) dying on `(u32)&ModelNode_...` initialisers, which are
not compile-time constant expressions on a 64-bit target. See 3.5. (The exact failing count was
not reproduced here - the tree used to write this guide was already converted.)

**Related symptom** - every compile stage succeeds, the counters look healthy, and the build dies
only at the link with around 25 undefined symbols, all of them background blobs:

```
Undefined symbols for architecture arm64:
  "_bg_sev_all_p", referenced from: ...
  "_bg_silo_all_p", referenced from: ...
```

**Cause** - `tools/enable_bg_extraction.py` was not run before `scripts/extract_baserom.u.sh`.
The decomp ships 25 of the 34 `assets/obseg/bg/*.bin` rows in `scripts/filelist.u.csv` with the
extract flag set to `0`, so the extractor skips them and those `.bin` files are never written.
Nothing earlier in the build complains, because nothing earlier needs them.

**Fix** - run it and re-extract:

```bash
cd vendor/ge-decomp
python3 ../../tools/enable_bg_extraction.py
tools/extractor/extractor baserom.u.z64 scripts/filelist.u.csv
```

Re-extracting is safe and idempotent: every row is a byte range copied out of the ROM, so files
that already exist are rewritten with identical contents.

**Related symptom** - `mac game: 166 built, 2 failed`, the second failure being
`src/game/bg.c`, and a link failure listing around thirty undefined symbols that all begin
`bg`:

```
mac FAILED: src/game/bg.c
mac FAILED: src/tlb_manage.c
Undefined symbols for architecture arm64:
  "_bgCopyVisibleRoomsToList", referenced from: ...
```

**Cause** - `gen_port_decls.py` was run as part of section 3.5. It regenerates
`src/ge_port_decls.h`, discarding the curated copy that `0001-source.patch` installs, and
reinstates the upstream four-argument `void sub_GAME_7F0B7F84(...)` prototype where the patch
uses five arguments returning `s32`. `bg.c` then fails on a conflicting declaration, and every
symbol it defines goes undefined. Nothing in the error mentions the header.

**Fix** - restore the patched header and rebuild:

```bash
cd vendor/ge-decomp
rm -f src/ge_port_decls.h
git apply --include='src/ge_port_decls.h' ../../getv/patches/0001-source.patch
```

**Related symptom** - `mac assets: 744 built, 0 failed` instead of 746, with undefined
`gePortObsegSize` or `ge_images_segment`. Two generated sources are missing:
`assets/obseg/ge_obseg_sizes.c`, written by `gen_obseg_blobs.py`, and
`assets/images/ge_images_segment.c`, written by `gen_images_segment.py`. Both are in the
section 3.5 sequence; re-run whichever was skipped.

### 7.3 SDL2 problems

**Symptom A - no SDL2 source.**

```
no SDL2 source at /path/to/goldeneye-native/deps/SDL2-2.30.9
```

`deps/` is gitignored and untracked, so a fresh clone never has it. **Fix:** section 2.3. The
directory name must be exactly `SDL2-2.30.9`.

**Symptom B - CMake refuses the project.** SDL 2.30.9 declares `cmake_minimum_required(3.0)`,
which CMake 4 rejects. The script already passes `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, which is the
sanctioned opt-back-in, so this should not happen. If it does, you are not running the script's own
CMake invocation. **Fix:** use `./build_mac.sh sdl` rather than invoking CMake by hand.

**Symptom C - SDL2 built, wrong architecture.** `./build_mac.sh sdl` finishes but the second line
reads:

```
Non-fat file: /Users/you/.n64tvos/sdl2-mac/lib/libSDL2.a is architecture: x86_64
```

**Cause** - the whole toolchain is running under Rosetta. This is the specific trap the build's SDL
arrangement exists to avoid, and it is why `brew install sdl2` is not used: brew under Rosetta
produces an Intel SDL2 that cannot link into an arm64 binary, and it does so without complaining.

**Fix** - check what you are actually running:

```bash
uname -m                    # must be arm64, not x86_64
arch                        # same
which cmake && file $(which cmake)
```

If your terminal is launched under Rosetta, turn that off (Finder, Get Info on the terminal app,
uncheck "Open using Rosetta"), open a fresh terminal, delete `~/.n64tvos/build-sdl2-mac` and
`~/.n64tvos/sdl2-mac`, and run `./build_mac.sh sdl` again.

**Symptom D - SDL2 headers not found.** Compiles of the port layer fail on `SDL.h`. The port
flags include `-I "$SDL/include" -I "$SDL/include/SDL2"`, so this means the install never happened.
Run `./build_mac.sh env`, check the `SDL=` path, and confirm `ls "$SDL/include/SDL2/SDL.h"`.

### 7.4 Wrong architecture, or Command Line Tools broken

**Symptom** - `./build_mac.sh env` prints an empty SDK:

```
SDK=
```

and every subsequent compile fails. **Cause** - `xcrun -sdk macosx --show-sdk-path` produced
nothing: the Command Line Tools are absent, or `xcode-select` points at a directory that no longer
exists (common after deleting or moving Xcode).

**Fix**

```bash
xcode-select -p                       # what it currently points at
xcode-select --install                # install the tools
sudo xcode-select --reset             # or reset a stale pointer
xcrun -sdk macosx --show-sdk-path     # must print a real path
```

**Symptom** - the link fails with `building for macOS-arm64 but attempting to link with file
built for macOS-x86_64`, naming `libSDL2.a`. That is 7.3 symptom C.

**Symptom** - you are on an Intel Mac. The build produces arm64 objects that will not run. There is
no supported configuration here; the target triple is a literal in `build_mac.sh` and changing it is
a porting task, not a setting. See `docs/PORTING.md`.

### 7.5 Stale or partial build directory

**Symptom A**

```
no objects in /path/to/getv/build-mac/obj -- run './build_mac.sh lib' once first
```

You ran `port` before ever running `lib`. `cmd_port` refuses when `build-mac/obj` is absent or
empty.

**Symptom B**

```
nothing built -- run './build_mac.sh lib'
```

You ran `app` with no `build-mac/obj` directory at all.

**Symptom C - orphaned objects**

```
mac excluded 16 orphaned object(s) whose source left the build
```

Not an error. Objects accumulate in `build-mac/obj`, and one whose source has since left the
build - excluded by name, deleted, renamed - would otherwise stay in `libge.a` and keep
satisfying the link forever. `run_batch` records what it compiled in `build-mac/objects.txt`;
anything on disk that is neither in that manifest nor part of the port layer is left out of the
archive. The files are not deleted, so a wrong exclusion costs a rebuild rather than your build
directory.

The failure this prevents is invisible, which is why it is worth understanding. When
`assets/obseg/setup/{e,j}` were excluded from the build, their sixteen objects stayed in the
archive and the binary went on resolving seven levels to PAL setup data through them - a build
reporting the correct new counts while linking the old data. A large count here, or any count on
a build where you changed nothing, means `obj` has drifted; deleting `build-mac/` and running
`lib` again is always safe.

```
mac note: no object manifest; archiving every object found -- run './build_mac.sh lib'
```

Also not an error. `build-mac/objects.txt` is absent, which happens on an `app` run against a
build directory created before manifests existed. Every object found is archived for that run
only, exactly as the build behaved previously.

**Symptom C**

```
missing harness object /path/to/getv/build-mac/obj/port_ge_mac_main.o -- run './build_mac.sh port'
```

The object directory exists but one of the two link roots is not in it. The harness objects carry
`main()` and `SDL_main()` and stay outside the archive, so `app` checks for them explicitly.

**Symptom D - a build that used to work now fails oddly, or a file you deleted still seems to be
linked in.** There is no dependency tracking and no incremental check: `run_batch` recompiles
everything every time, but it never removes objects for sources that no longer exist. A stale `.o`
outliving its source is exactly the case that turns a broken tree into a green-looking build. (For
the same reason, a compile that fails always deletes its output rather than leaving the previous
object in place.)

**Fix - the reset that always works:**

```bash
cd getv
rm -rf build-mac
./build_mac.sh all
```

`build-mac/` is gitignored in full and holds nothing you cannot regenerate. Deleting it costs one
full build.

To reset SDL2 as well:

```bash
rm -rf ~/.n64tvos/sdl2-mac ~/.n64tvos/build-sdl2-mac
cd getv && ./build_mac.sh sdl && ./build_mac.sh all
```

### 7.6 Partial or failed third-party fetch

**Symptom A - refusal to overwrite.**

```
fetch-thirdparty: getv/port/fast3d/gfx_cc.c already exists; run 'clean' first, or pass --force
```

By design: `fetch` will not clobber files that are already there, in case you have edited them.

**Fix** - if you have made no local edits:

```bash
tools/fetch-thirdparty.sh clean
tools/fetch-thirdparty.sh fetch
```

or `tools/fetch-thirdparty.sh fetch --force`. **If you have edited any of the fifteen files, run
`tools/fetch-thirdparty.sh regen` first** - the patch is the only place such an edit is recorded,
and `clean` or `--force` will otherwise discard it.

**Symptom B - cannot reach upstream.**

```
fetch-thirdparty: could not obtain sm64ex at d7ca2c04364a6dd0dac58b47151e04e26887e6f0 (network?)
```

The script tries, in order: `$GETV_SM64EX_REPO` if set; `vendor/sm64ex` if it already contains the
pinned commit; the bare cache at `vendor/sm64ex-cache.git`; then a network fetch of the bare SHA.

**Fix** - check connectivity to `github.com`. On an offline machine, point at an existing clone or
mirror that contains the commit:

```bash
GETV_SM64EX_REPO=/path/to/an/sm64ex/clone tools/fetch-thirdparty.sh fetch
```

If that clone does not have the commit you get a precise error:

```
fetch-thirdparty: GETV_SM64EX_REPO=/path does not contain d7ca2c04364a6dd0dac58b47151e04e26887e6f0
```

`GETV_SM64EX_CACHE` relocates the bare cache directory.

**Symptom C - the patch does not apply.**

```
fetch-thirdparty: patch did not apply -- upstream pin and patch disagree
```

The patch is zero-context and applies to exactly one commit, so this means the upstream tree you
resolved is not `d7ca2c04`. Most likely `$GETV_SM64EX_REPO` or `vendor/sm64ex` is pointing at a
different revision. Unset the override, delete `vendor/sm64ex-cache.git`, and let the script fetch
the pin itself.

**Symptom D - a file is missing upstream.**

```
fetch-thirdparty: upstream src/pc/gfx/gfx_cc.c missing at d7ca2c04364a6dd0dac58b47151e04e26887e6f0
```

The resolved repository is not sm64ex, or the manifest has been edited.

**Symptom E - an incomplete set on disk.** `status` shows one or more `ABSENT` lines, or `verify`
reports:

```
MISSING  getv/port/audio/ge_mixer.c
DIFFERS  getv/port/fast3d/gfx_pc.c
```

`MISSING` means the fetch did not complete - run `clean` then `fetch`. `DIFFERS` means the file on
disk is not pristine-plus-patch, which is either a local edit you have not run `regen` for, or a
corrupted fetch.

### 7.7 No `$HOME`

**Symptom** - one or both of:

```
[getv][config] no $HOME
[getv][save] no $HOME and no GETV_SAVEDIR -- persistence OFF
```

**Cause** - `HOME` is unset or empty. `gePortUserDataDir()`'s macOS branch is a literal
`getenv("HOME")` plus `/Library/Application Support/<app>` and has no fallback, which is deliberate:
`SDL_GetPrefPath` would return the same directory on macOS, but going through it would make the Mac
path depend on tvOS's choice, and on tvOS that choice lands in `Library/Caches`, which the OS
purges.

This usually happens under `launchd`, `cron`, a CI runner, or `env -i`.

**Consequence** - the game still runs. Configuration falls back to built-in defaults, and saves are
off for that run. Nothing crashes.

**Fix**

```bash
HOME="$HOME" ./build_mac.sh run          # from an interactive shell
GETV_SAVEDIR=/some/writable/dir ./build_mac.sh run
```

`GETV_SAVEDIR` bypasses the home directory entirely and is the right answer for automated runs, so
they never touch a real save.

### 7.8 Read-only or unwritable home directory

**Symptom A - saves.**

```
[getv][save] cannot create /Users/you/Library/Application Support/Goldeneye-Native: Read-only file system -- persistence OFF
```

or, if the directory exists but the file cannot be written:

```
[getv][save] cannot write /Users/you/Library/Application Support/Goldeneye-Native/eeprom.bin.tmp: Permission denied
```

**Cause** - the save directory's leaf is created with `mkdir`, not `mkdir -p`. That is intentional:
`~/Library/Application Support` always exists on a real macOS account, so a missing *parent* is a
symptom of a wrong `$HOME` and must stay visible rather than being papered over by creating it.

**Symptom B - configuration.**

```
[getv][config] mkdir failed: /Users/you/Library/Application Support/GoldenEye
[getv][config] cannot write /Users/you/Library/Application Support/GoldenEye/goldeneye.cfg
```

The config directory *is* created recursively, because `--write-config` is explicitly a "set this
machine up" command. Failure here is non-fatal and near-silent by design - a read-only home must
still boot on built-in defaults.

**Fix**

```bash
ls -ld ~/Library/"Application Support"
mkdir -p ~/Library/"Application Support"/Goldeneye-Native
```

or redirect both away from home:

```bash
GETV_SAVEDIR=/tmp/ge-save ./build-mac/goldeneye --config=/tmp/ge/goldeneye.cfg
```

Note that `--config=PATH` reads a file; it does not create one. Use `--write-config=/tmp/ge.cfg`
first if you need a template there.

### 7.9 Runtime messages that are not faults

Do not chase these:

| Message | Why |
|---|---|
| `mac FAILED: src/tlb_manage.c` | Expected. See 4.3. |
| `ld: warning: reducing alignment of section __DATA,__common ...` | Expected on every link. |
| `[getv] STUB: crashInit`, `STUB: romCreateMesgQueue`, `STUB: indycommInit`, `STUB: rmonGetToken` | N64 hardware and dev-host entry points with nothing to do here. |
| `[getv] TLB: not present on tvOS; heap = 0x... (32 MB)` | The 32 MB game arena is a `static u8[]` in `__bss`, zero-filled by the kernel at every launch. |
| `[getv] GoldenEye tvOS harness starting` | The harness is shared verbatim with the tvOS target. |
| `GL_VERSION=2.1 Metal - ...` | The legacy compatibility profile is requested deliberately. |
| `[getv][config] ... controls=2.2 galore is a TWO-CONTROLLER style ...` | Informational; the port presents one pad as ports 0+1. |
| `[getv][ob] index=670 name='LgunE' ... -> NATIVE (early-out)` | Asset loader taking the native path. |
| `[getv][far] ...`, `[getv][zcmp] ...`, `[getv][texfmt] ...` | Renderer instrumentation, on by default. |

Genuine open problems - untextured level props and characters, a scene that is too dark, menu
polish, no framerate above 60, multiplayer edge cases - are listed in `README.md` and detailed in
`docs/ROADMAP.md`. They are not setup faults and no amount of rebuilding will change them.

---

## 8. Verifying it works

The smoke test is a fixed-frame run. Setting `GETV_EXIT_FRAME=N` ends the run after N *rendered
frames* and calls `_exit(0)`, rather than after N seconds. That distinction is what makes the
result reproducible: the renderer prints a counter checkpoint every 60 frames, and a wall-clock
timeout samples whichever checkpoint happened to fall before the deadline, which depends entirely
on host load. Under a frame budget, the last checkpoint is the same frame on every launch and every
machine.

Pick N as `k*60 + 1` so the run ends just after a checkpoint. 61 is the conventional value.

A window will open briefly - there is no true headless video path - and the process will exit on its
own. Nothing needs to be killed and no timeout wrapper is needed.

```bash
cd getv
GETV_EXIT_FRAME=61 ./build_mac.sh run > /tmp/ge-smoke.log 2>&1; echo "rc=$?"
```

**Expected: `rc=0`.**

Then check the log:

```bash
tail -3 /tmp/ge-smoke.log
```

```
[getv][texfmt]   I    8b  : 14421
[getv][fp] 4bit_texture_loads=0 sky_tris=0
[getv][fp] exit_frame reached: frames=61
```

The two assertions that constitute a pass:

```bash
grep -q '^\[getv\]\[fp\] exit_frame reached: frames=61$' /tmp/ge-smoke.log && echo PASS || echo FAIL
```

- exit status `0`
- a final line reading exactly `[getv][fp] exit_frame reached: frames=61`

Both together mean the binary linked correctly, SDL2 and OpenGL came up, the game booted through
its whole init path, the asset loader found its data, and Fast3D rendered 61 frames and shut down
cleanly.

For a fuller picture, these lines should all be present:

```bash
grep -E 'GL_RENDERER|Fast3D up|window:|frame 61:|exit_frame reached' /tmp/ge-smoke.log
```

Observed on an Apple M1, macOS 26.2 SDK, at the shipped defaults:

```
[getv] window: 1280x960 windowed, resizable; fullscreen toggle = F11 / Cmd-F / Alt-Enter
[getv] GL_VENDOR=Apple | GL_RENDERER=Apple M1 | GL_VERSION=2.1 Metal - 90.5
[getv] Fast3D up: 0x0 internal, 0x0 output, supersample 1
[getv] frame 61: tris submitted=520 drawn=520 fog=0 fog_prim_a=0 shadea_cc=0 | tlut ia16=0 rgba16=0 none=0 tiledim=0
[getv][fp] exit_frame reached: frames=61
```

The run produces about 400 lines and takes a few seconds. `GL_RENDERER` will name your own GPU.

**Do not treat `submitted=520 drawn=520` as a required value.** It was measured on the machine above
at frame 61 of the title sequence with the shipped configuration. It is stable across runs on one
machine and at one configuration - that is the whole point of the fixed-frame budget - but it is not
a cross-machine constant and it changes with resolution, supersampling and anything else that
alters what is on screen. Use it as a baseline you establish yourself, not as a pass criterion.

If `rc` is nonzero, or the `exit_frame reached` line is absent, the last few hundred lines of the
log show how far the boot got. Compare against section 5.1 and work back through section 7.

---

## Related documents

- [`CONFIGURATION.md`](CONFIGURATION.md) - every setting, on the command line and in the file.
- [`THIRD_PARTY.md`](THIRD_PARTY.md) - the fifteen fetched files, in full.
- [`CHEATS.md`](CHEATS.md) - the named cheat system.
- [`MODDING.md`](MODDING.md) - changing the game rather than playing it.
- [`PORTING.md`](PORTING.md) - what a Windows, Linux or tvOS target would take.
- [`ROADMAP.md`](ROADMAP.md) - the detailed open-problem list.
- [`LICENSING.md`](LICENSING.md) - provenance for the repository as a whole.
- [`../getv/port/PROVENANCE.md`](../getv/port/PROVENANCE.md) - file-level origin record for the
  port layer. **Read this before redistributing anything.**
