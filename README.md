# Goldeneye-Native

A native port of GoldenEye 007. The game's C source comes from the
[`n64decomp/007`](https://github.com/n64decomp/007) decompilation and is compiled directly for
the host with clang; a platform layer supplies windowing, input, audio and a Fast3D display-list
renderer on top of SDL2 and OpenGL. There is no MIPS interpreter, no dynamic recompiler and no
static recompilation step — the binary is the game, built as ordinary C.

Today it builds and plays on **macOS on Apple silicon (arm64)**. Windows and Linux are not
supported; nothing in the build scripts targets them yet. A tvOS target exists in the tree
(`getv/build.sh`, `getv/build_sim.sh`) but is on hold and has bit-rotted, so treat it as
unbuildable until stated otherwise.

## Bring your own ROM

**No ROM, no extracted assets and no game data are distributed with this repository, and none
ever will be.** You need your own legal copy of the NTSC (US) cartridge, dumped to a file.

The build reads the ROM through the decompilation's own extraction scripts, which expect it at
the root of the decomp checkout under a fixed name:

```
vendor/ge-decomp/baserom.u.z64
```

The convention used here is to keep dumps in `roms/` at the repository root and symlink one into
place. `.gitignore` blocks `roms/`, every `*.z64` / `*.n64` / `*.v64` / `*.elf`, all `getv/build-*`
directories (they contain object files compiled from extracted ROM data), `*.bmp` frame captures,
and `vendor/` itself. Do not defeat those rules.

What is verified: the expected US ROM is 12,582,912 bytes, big-endian z64 (magic `80371240`,
internal name `GOLDENEYE`), SHA-1 `abe01e4aeb033b6c0836819f549c791b26cfde83`. That value matches
`ge007.u.sha1` in the decompilation, so a ROM with that hash is byte-identical to what a correct
US build must produce. If your dump has a `.n64` or `.v64` extension, or a header of `37804012`,
it is byte-swapped and must be converted to native big-endian first.

## Prerequisites

- macOS 13 or later on Apple silicon. The build targets `arm64-apple-macos13.0`.
- Xcode Command Line Tools (`xcode-select --install`) — supplies `clang`, `xcrun` and the macOS SDK.
- **bash 4 or newer on `PATH`.** The parallel compile step uses `mapfile`, which the `/bin/bash`
  3.2 that ships with macOS does not have. `brew install bash` and make sure `/usr/local/bin` or
  `/opt/homebrew/bin` precedes `/bin` on your `PATH`.
- CMake, for building SDL2.
- Python 3, for the asset generation scripts.
- SDL2 2.30.9 source in `deps/SDL2-2.30.9`. It is built from source rather than taken from
  Homebrew because a Homebrew running under Rosetta produces an x86_64 SDL2 that cannot link into
  an arm64 binary. `build_mac.sh sdl` compiles it for arm64 and installs it to
  `~/.n64tvos/sdl2-mac` — deliberately outside the repository, because the repository path
  contains a space and that has broken header search paths here before.

## Preparing the source tree

`vendor/` is not tracked. Clone the decompilation into place, apply this port's changes, and
extract the assets from your ROM.

```bash
git clone https://github.com/n64decomp/007 vendor/ge-decomp
cd vendor/ge-decomp
git apply ../../getv/patches/0001-getv-port.patch
ln -sf ../../roms/ge007.u.z64 baserom.u.z64
```

Then generate the asset sources. This needs neither Docker nor the IDO toolchain:

```bash
cd vendor/ge-decomp
bash scripts/extract_baserom.u.sh
python3 scripts/generate_chr_c.py
python3 scripts/generate_gun_c.py
python3 scripts/generate_prop_model_c.py
python3 ../../tools/gen_obseg_blobs.py
python3 scripts/make/sync_imagelist_with_def.py build/imagelist.csv
bash scripts/make/combine_images_named.sh build/imagelist.csv assets/images/combined
```

Everything these produce is derived from your ROM. It stays untracked.

## Build

`getv/build_mac.sh` takes one of `sdl`, `lib`, `port`, `app`, `all`, `run` or `env`.

```bash
cd getv
./build_mac.sh sdl     # once: build SDL2 2.30.9 for arm64 into ~/.n64tvos/sdl2-mac
./build_mac.sh all     # compile everything, then link
```

- `lib` compiles the game, assets, audio and platform layer into `build-mac/obj`.
- `port` recompiles only `getv/port/**` and the two harness objects. Takes seconds.
- `app` archives the objects into `build-mac/libge.a` and links `build-mac/goldeneye`.
- `all` is `lib` followed by `app`.
- `run` launches the linked binary, forwarding any arguments.
- `env` prints the resolved SDK, SDL prefix, target triple and output paths.

A full build takes about five minutes; incremental `port` builds take seconds. Compilation is
parallel; `GETV_JOBS` caps the job count and defaults to 6.

Expected output from `./build_mac.sh all`:

```
mac game 167 built, 1 failed
mac assets: 762 built, 0 failed
mac audio: 40 built, 0 failed
mac port layer: 22 built, 0 failed
```

**The one failure is expected.** `src/tlb_manage.c` programs the N64's MIPS 4300 translation
lookaside buffer. There is no TLB to program here and nothing links against it, so it is left to
fail rather than being papered over. Seven further N64-hardware and SGI-dev-host files
(`usb.c`, `rmon.c`, `sched.c`, `ramrom.c`, `init.c`, `indy_comms.c`, `indy_commands.c`) are
excluded by name in the build script for the same reason.

## Run

```bash
./build-mac/goldeneye
```

Keyboard is bound to controller port 0 by default: WASD to move, arrow keys to look, Space or
Left Ctrl to fire, E or Return to use, Q to aim, Z and X for the shoulder buttons, Tab to pause,
IJKL for the d-pad. A connected gamepad works alongside it — whichever input is held wins, so
plugging in a pad never degrades the keyboard and vice versa.

On first run, with no configuration file present anywhere, the game writes a commented template
to `~/Library/Application Support/GoldenEye/goldeneye.cfg` and immediately reads it back. This is
not a convenience: several of the port's tuned defaults (notably `invert_look`) only exist in
that template, and a default that lives in a file nobody has generated is not a default. Edit
that file to taste.

Save data is separate, and lands in `~/Library/Application Support/Goldeneye-Native/eeprom.bin`.

Every setting is documented in [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md). The named cheat
system is in [`docs/CHEATS.md`](docs/CHEATS.md). If you want to change the game rather than play
it, start at [`docs/MODDING.md`](docs/MODDING.md).

## Known issues

All 26 real stages load, reach first-person and render. Eleven further stage ids have no data in
the ROM at all — nine were cut during development, and Citadel has a background file but no setup
file, so it can never load. That is data absence, not a port defect.

Open problems, in rough order of how much they will bother you:

- **Level props and characters are untextured.** The pass that expands the game's `G_NOOP`
  texture placeholders reaches menu and title models but not level-loaded ones. This is the most
  visible remaining defect.
- **The scene is too dark.** Rock surfaces measure a mean around 17 against retail's mid-grey.
- **Menu polish.** Teal film strips, a split Brosnan portrait, unselected names that are hard to
  read, and an empty primary-objectives list.
- **No framerate above 60.** The game integrates fire rates, physics, animation and the mission
  clock once per rendered frame, so raising the render rate raises the game speed with it. Real
  high-refresh support needs the simulation decoupled from presentation, which this build does not
  do. Values above 60 are rejected by the configuration layer for that reason.
- **Multiplayer edge cases.** Score caps are not enforced on the headless path, and `num_shots`
  disagrees with the fire path.
- **Windows, Linux and tvOS.** Not built, not tested.

`docs/ROADMAP.md` carries the detailed open list.

## Provenance and licensing

Read [`getv/port/PROVENANCE.md`](getv/port/PROVENANCE.md) before redistributing anything. It
records, per directory, where the platform layer's code came from — in particular that the
license status of `getv/port/fast3d/`, which descends from sm64ex's copy of
`Emill/n64-fast3d-engine`, is **unresolved**. The notice sm64ex ships is the pre-2021 form, whose
second condition bans binary redistribution outright. Do not assume it is MIT.

The decompilation itself has no license file, and its libultra sources carry SGI proprietary
headers. That is upstream's situation, but it is a fact about the base this port is built on.

The ROM, extracted assets, and anything derived from them are never distributable under any
license.
