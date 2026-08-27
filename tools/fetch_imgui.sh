#!/usr/bin/env bash
# Fetch and build Dear ImGui for the dev overlay / launcher UI (getv/port/src/ge_imgui.cpp).
#
# Same arrangement as tools/fetch_lua.sh, and for the same reason: deps/ is gitignored (see
# .gitignore), so third-party source is fetched on demand and built into a prefix outside
# the tree. This repository never carries code it did not write. Run this once; the build
# scripts detect the result and enable the overlay.
#
# Without it, everything still builds. ge_imgui.cpp compiles its entry points to empty
# bodies, the build omits -DGE_WITH_IMGUI, and GETV_IMGUI=1 prints a line saying the binary
# was built without ImGui. The overlay is an addition, never a prerequisite.
#
# Why a static library rather than compiling ImGui inside build_mac.sh
# -------------------------------------------------------------------
# Same call fetch_lua.sh made, for stronger reasons here. ImGui's seven translation units
# want -std=c++17, their own include paths, and warning settings that have nothing to do
# with the game's PORTFLAGS; folding them into build_mac.sh's port loop would mean either
# polluting those flags or growing a second special-cased loop that recompiles ~40 s of
# third-party C++ on every `./build_mac.sh port`. Building once into a prefix keeps
# build_mac.sh's only C++ compile to the single file this project actually wrote
# (getv/port/src/ge_imgui.cpp) and keeps `port` a seconds-long command.
#
# Which backends, and why OpenGL2 and not OpenGL3
# -----------------------------------------------
# imgui_impl_sdl2.cpp is the obvious platform backend: the port already creates its window
# and pumps its events through SDL2.
#
# The renderer backend is imgui_impl_opengl2.cpp, which is NOT the usual choice and is not
# an accident. build_mac.sh requests no GL profile, so macOS hands back a legacy 2.1
# context -- deliberately, because gfx_opengl.c's shader generator emits `#version 120`
# with attribute/varying, which a 3.2 core profile rejects outright (see the header comment
# in build_mac.sh; do not "modernise" the context request). imgui_impl_opengl3.cpp defines
# IMGUI_IMPL_OPENGL_USE_VERTEX_ARRAY unconditionally for every desktop GL target and calls
# glGenVertexArrays/glBindVertexArray, which are GL 3.0 entry points that a 2.1 context does
# not have. imgui_impl_opengl2.cpp is the fixed-function path and is exactly what a 2.1
# context wants.
#
# The cost of the GL2 backend is that it cannot reset state it has no API for -- its own
# source says so. Fast3D leaves a shader program bound and a VBO bound, and fixed-function
# client arrays would be read as offsets into that VBO. ge_imgui.cpp saves and clears both
# around the draw; that is the whole reason that wrapper exists rather than calling
# ImGui_ImplOpenGL2_RenderDrawData() straight from gfx_sdl2.c.
#
# Licence: Dear ImGui is MIT (LICENSE.txt in the tarball). That is compatible with this
# project's position in docs/LICENSING.md and, unlike the GPL projects listed in
# docs/THIRD_PARTY.md, imposes nothing on the rest of the tree. Attribution belongs in
# docs/THIRD_PARTY.md, not in a comment here.
set -uo pipefail

VERSION="1.91.9b"
TAG="v$VERSION"
# github.com/ocornut/imgui, tag v1.91.9b, commit 5c1d6d4 (release of 2025-04-11).
# Checked rather than trusted, because this script downloads an archive and then compiles
# it into the game binary.
#
# Caveat worth knowing: this is the sha256 of GitHub's *generated* tag archive, not of an
# artefact upstream uploaded. GitHub has kept those byte-stable since 2023 but has changed
# its gzip once before. If this check ever fails, do not "fix" it by pasting in the new
# hash -- diff the extracted tree against a known-good copy first, then update this line
# with the reason in the commit message.
SHA256="8e1bbc76c71d74fef2fb85db7e7ca8eba13d6a86623c54992b60162db554ffdb"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="$HERE/../deps"
SRC="$DEPS/imgui-$VERSION"

# TARGET selects which platform's library this run produces -- "host" (the default,
# original behaviour, unchanged) builds for whatever this Mac/Linux box is; "tvos" and
# "tvsim" cross-compile for the Apple TV device and Simulator respectively, with a Metal
# renderer backend instead of OpenGL2 (tvOS has no desktop GL at all, GLES or not -- see
# gfx_metal.mm's header comment for why the game renderer went the same way). Each target
# gets its own prefix and its own SOURCES list below; nothing about the host path changes.
#
#   ./fetch_imgui.sh          host build (as before)
#   ./fetch_imgui.sh tvos     device, arm64-apple-tvos17.0
#   ./fetch_imgui.sh tvsim    simulator, arm64-apple-tvos17.0-simulator
TARGET="${1:-host}"
CXX="${CXX:-c++}"
TARGETFLAGS=()
SDLINC=()
BACKEND_RENDERER=opengl2   # or metal, set below

case "$TARGET" in
  host)
    # The build script to name in the closing hint. Printing the macOS one on Linux is a
    # small thing that costs a real minute to anyone following the message on the host
    # that most needs it.
    case "$(uname -s)" in
      Darwin) BUILDSCRIPT=build_mac.sh; PREFIX="$HOME/.n64tvos/imgui-mac";   SDL="$HOME/.n64tvos/sdl2-mac"   ;;
      Linux)  BUILDSCRIPT=build_linux.sh;  PREFIX="$HOME/.n64tvos/imgui-linux"; SDL="$HOME/.n64tvos/sdl2-linux" ;;
      *) echo "unsupported host: $(uname -s)" >&2; exit 1 ;;
    esac
    # Host arch, asked of the kernel rather than of uname, for the reason documented at
    # the top of getv/build_mac.sh: under an x86_64 Homebrew bash in Rosetta, uname -m
    # reports the process architecture and the library would be built for the wrong slice.
    if [ "$(uname -s)" = "Darwin" ]; then
      ARCH="$(uname -m)"
      if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = "1" ]; then ARCH="arm64"; fi
      TARGETFLAGS=( -target "${ARCH}-apple-macos13.0" -isysroot "$(xcrun -sdk macosx --show-sdk-path)" )
    fi
    ;;
  tvos)
    BUILDSCRIPT=build.sh
    PREFIX="$HOME/.n64tvos/imgui-tvos"
    SDL="$HOME/.n64tvos/sdl2-tvos"
    BACKEND_RENDERER=metal
    TARGETFLAGS=( -target arm64-apple-tvos17.0 -isysroot "$(xcrun -sdk appletvos --show-sdk-path)" )
    ;;
  tvsim)
    BUILDSCRIPT=build_sim.sh
    PREFIX="$HOME/.n64tvos/imgui-tvsim"
    SDL="$HOME/.n64tvos/sdl2-tvsim"
    BACKEND_RENDERER=metal
    TARGETFLAGS=( -target arm64-apple-tvos17.0-simulator -isysroot "$(xcrun -sdk appletvsimulator --show-sdk-path)" )
    ;;
  *) echo "unknown target: $TARGET (want host, tvos or tvsim)" >&2; exit 1 ;;
esac

# ImGui's SDL2 backend includes <SDL.h>. The port links a private static SDL2 built by
# `./getv/build_mac.sh sdl` (or build.sh/build_sim.sh for tvOS) into a space-free prefix;
# a system/Homebrew SDL2 is not a substitute for the host build (brew's is
# x86_64-under-Rosetta on this machine and its headers could disagree with the library we
# actually link), and there is no system SDL2 at all to fall back to when cross-compiling.
if [ -d "$SDL/include/SDL2" ]; then
  SDLINC=( -I "$SDL/include" -I "$SDL/include/SDL2" )
elif [ "$TARGET" = "host" ] && command -v sdl2-config >/dev/null 2>&1; then
  # Space-safe: one flag per word is fine, sdl2-config never emits paths with spaces on a
  # sane install, and this is only the fallback for a host that has no private SDL2 yet.
  SDLINC=( $(sdl2-config --cflags) )
else
  echo "no SDL2 headers found." >&2
  echo "  expected $SDL/include/SDL2" >&2
  case "$TARGET" in
    host)  echo "  run './getv/build_mac.sh sdl' first" >&2 ;;
    tvos)  echo "  build getv/deps/sdl2-tvos and symlink it into ~/.n64tvos/sdl2-tvos first" >&2 ;;
    tvsim) echo "  build getv/deps/sdl2-tvsim and symlink it into ~/.n64tvos/sdl2-tvsim first" >&2 ;;
  esac
  exit 1
fi

mkdir -p "$DEPS"

if [ ! -d "$SRC" ]; then
  echo "fetching dear imgui $TAG"
  TARBALL="$DEPS/imgui-$VERSION.tar.gz"
  curl -sSL --max-time 180 -o "$TARBALL" \
    "https://github.com/ocornut/imgui/archive/refs/tags/$TAG.tar.gz" || {
    echo "download failed" >&2; exit 1; }

  if command -v shasum >/dev/null 2>&1; then GOT="$(shasum -a 256 "$TARBALL" | cut -d' ' -f1)"
  else                                       GOT="$(sha256sum   "$TARBALL" | cut -d' ' -f1)"; fi
  if [ "$GOT" != "$SHA256" ]; then
    echo "checksum mismatch for imgui-$VERSION.tar.gz" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $GOT" >&2
    rm -f "$TARBALL"
    exit 1
  fi
  # The archive's top directory is imgui-1.91.9b, which is already $SRC's basename.
  tar xzf "$TARBALL" -C "$DEPS" && rm -f "$TARBALL"
  [ -d "$SRC" ] || { echo "unexpected archive layout: no $SRC" >&2; exit 1; }
fi

mkdir -p "$PREFIX/lib" "$PREFIX/include"
OBJDIR="$(mktemp -d)"
trap 'rm -rf "$OBJDIR"' EXIT

# imgui_demo.cpp is included on purpose. It is the reference for every widget in the
# library and ImGui::ShowDemoWindow() is the fastest way to sanity-check that the overlay's
# GL state handling is correct against a busy draw list rather than one small window. It
# costs archive size only: ar members are pulled on demand, so a binary that never calls
# ShowDemoWindow() does not link it.
SOURCES=(
  "$SRC/imgui.cpp"
  "$SRC/imgui_draw.cpp"
  "$SRC/imgui_tables.cpp"
  "$SRC/imgui_widgets.cpp"
  "$SRC/imgui_demo.cpp"
  "$SRC/backends/imgui_impl_sdl2.cpp"
)
if [ "$BACKEND_RENDERER" = "metal" ]; then
  # tvOS has no desktop GL (imgui_impl_opengl2.cpp's fixed-function calls don't exist on
  # GLES either) -- Metal is the only renderer backend that makes sense there, same
  # reasoning as gfx_metal.mm. imgui_impl_metal.mm is Objective-C++ and needs -fobjc-arc;
  # the .cpp members below do not, and passing it to them is harmless but pointless.
  SOURCES+=( "$SRC/backends/imgui_impl_metal.mm" )
else
  SOURCES+=( "$SRC/backends/imgui_impl_opengl2.cpp" )
fi

ok=0; fail=0
for f in "${SOURCES[@]}"; do
  ext="${f##*.}"
  b="$(basename "$f" ".$ext")"
  [ -f "$f" ] || { fail=$((fail+1)); echo "  MISSING: $f"; continue; }
  extraflags=()
  [ "$ext" = "mm" ] && extraflags=( -fobjc-arc )
  if "$CXX" -c "$f" -o "$OBJDIR/$b.o" \
       ${TARGETFLAGS[@]+"${TARGETFLAGS[@]}"} \
       -std=c++17 -O2 -fno-exceptions -fno-rtti \
       -I "$SRC" -I "$SRC/backends" ${SDLINC[@]+"${SDLINC[@]}"} \
       ${extraflags[@]+"${extraflags[@]}"} \
       -DGL_SILENCE_DEPRECATION -w
  then
    ok=$((ok+1))
  else
    fail=$((fail+1)); echo "  FAILED: $b.$ext"
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "imgui: $ok built, $fail failed -- not installing a partial library" >&2
  exit 1
fi

rm -f "$PREFIX/lib/libimgui.a"
ar rcs "$PREFIX/lib/libimgui.a" "$OBJDIR"/*.o || exit 1

# imgui_internal.h and the imstb_* headers are not optional extras: imgui.h does not need
# them, but any non-trivial overlay eventually does, and shipping half the headers turns a
# later #include into a confusing "file not found" against a prefix that looks installed.
cp "$SRC/imgui.h" "$SRC/imconfig.h" "$SRC/imgui_internal.h" \
   "$SRC/imstb_rectpack.h" "$SRC/imstb_textedit.h" "$SRC/imstb_truetype.h" \
   "$SRC/backends/imgui_impl_sdl2.h" \
   "$PREFIX/include/" || exit 1
if [ "$BACKEND_RENDERER" = "metal" ]; then
  cp "$SRC/backends/imgui_impl_metal.h" "$PREFIX/include/" || exit 1
else
  cp "$SRC/backends/imgui_impl_opengl2.h" "$PREFIX/include/" || exit 1
fi
cp "$SRC/LICENSE.txt" "$PREFIX/IMGUI-LICENSE.txt" 2>/dev/null

echo "dear imgui $VERSION ($TARGET, $BACKEND_RENDERER): $ok objects -> $PREFIX/lib/libimgui.a"
case "$TARGET" in
  host)
    echo "rebuild the game to pick it up: ./getv/$BUILDSCRIPT all"
    echo "then run it with: GETV_IMGUI=1 ./getv/$BUILDSCRIPT run"
    ;;
  tvos|tvsim)
    echo "rebuild the game to pick it up: GETV_RENDERER=metal ./getv/$BUILDSCRIPT all"
    ;;
esac
