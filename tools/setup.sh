#!/usr/bin/env bash
# One command to get everything except the ROM.
#
# tools/install.sh supersedes this for most people: it does everything here AND the asset
# pipeline, the namespacing pass and the build, which is the part that was left as four manual
# steps out of docs/SETUP.md and is really twenty-odd commands in a fixed order. This script
# stays because "fetch the dependencies and stop" is still the right thing when you intend to
# drive the rest by hand.
#
# Fetches the third-party port-layer sources, the decompilation, Lua and Dear ImGui, applies
# the source patch, and builds SDL2 on macOS. Stops before the ROM, which you have to supply
# yourself and which nothing here will ever download.
#
# Everything fetched is permissively licensed and comes from its own upstream:
#   Lua              lua.org, MIT
#   Dear ImGui       github.com/ocornut/imgui, MIT
#   SDL2             libsdl-org, zlib
#   sm64ex           for the Fast3D renderer -- cloned and patched, never redistributed
#   n64decomp/007    the decompilation itself
#
# Re-running is safe. Each step checks for its own output and skips if it is already there.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

case "$(uname -s)" in
    Darwin) PLATFORM=mac ;;
    Linux)  PLATFORM=linux ;;
    *)      echo "error: unsupported platform $(uname -s). Windows uses tools\\fetch_deps_windows.ps1"; exit 1 ;;
esac

say() { printf '\n== %s\n' "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }

say "checking what is already here"
missing=""
for t in git python3 cmake; do have "$t" || missing="$missing $t"; done
have cc || have gcc || have clang || missing="$missing a C compiler"
if [ -n "$missing" ]; then
    echo "missing:$missing"
    if [ "$PLATFORM" = linux ]; then
        echo "  sudo apt install build-essential pkg-config cmake python3 git libsdl2-dev libgl1-mesa-dev"
    else
        echo "  xcode-select --install   # then: brew install cmake"
    fi
    exit 1
fi
echo "ok"

say "third-party port-layer sources"
if [ -f getv/port/fast3d/gfx_pc.c ]; then echo "already present"; else bash tools/fetch-thirdparty.sh fetch; fi

say "the decompilation"
if [ -d vendor/ge-decomp/.git ]; then
    echo "already cloned"
else
    git clone --depth 1 https://github.com/n64decomp/007 vendor/ge-decomp
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0001-source.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0006-fov-live-setter.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0007-load-trace.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0008-crosshair-color.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0009-freerun-divider.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0010-state-dump-player-position.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0011-netplay-tick-integration.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0012-real-font-overlay.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0013-lockstep-pinned-sim-step.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0014-lockstep-cull-on-the-tick.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0015-aim-toggle.patch" )
    ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0016-freecam.patch" )
    echo "cloned and patched"
fi

say "Lua and Dear ImGui (optional: mods, launcher, dev overlay)"
bash tools/fetch_lua.sh   || echo "  Lua fetch failed; mods will be unavailable"
bash tools/fetch_imgui.sh || echo "  ImGui fetch failed; the launcher will be unavailable"

if [ "$PLATFORM" = mac ]; then
    say "SDL2"
    if [ -d "$HOME/.n64tvos/sdl2-mac" ]; then echo "already built"; else bash getv/build_mac.sh sdl; fi
fi

cat <<'NEXT'

== done, except the part only you can do

Supply your own copy of the game. Nothing here downloads it and nothing here ships it.

  1. put your ROM at  roms/ge007.u.z64
  2. verify it:       shasum -a 1 roms/ge007.u.z64
                      expect abe01e4aeb033b6c0836819f549c791b26cfde83
  3. generate assets: docs/SETUP.md section 3.5
  4. build:           ./getv/build_mac.sh all     (or build_linux.sh)

docs/SETUP.md walks all four with the expected output of each.
NEXT
