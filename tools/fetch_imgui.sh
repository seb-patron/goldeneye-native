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

# The build script to name in the closing hint. Printing the macOS one on Linux is a
# small thing that costs a real minute to anyone following the message on the host that
# most needs it.
case "$(uname -s)" in
  Darwin) BUILDSCRIPT=build_mac.sh; PREFIX="$HOME/.n64tvos/imgui-mac";   SDL="$HOME/.n64tvos/sdl2-mac"   ;;
  Linux)  BUILDSCRIPT=build_linux.sh;  PREFIX="$HOME/.n64tvos/imgui-linux"; SDL="$HOME/.n64tvos/sdl2-linux" ;;
  *) echo "unsupported host: $(uname -s)" >&2; exit 1 ;;
esac

CXX="${CXX:-c++}"

# ImGui's SDL2 backend includes <SDL.h>. The port links a private static SDL2 built by
# `./getv/build_mac.sh sdl` into a space-free prefix; a system/Homebrew SDL2 is not a
# substitute here (brew's is x86_64-under-Rosetta on this machine and its headers could
# disagree with the library we actually link).
SDLINC=()
if   [ -d "$SDL/include/SDL2" ]; then SDLINC=( -I "$SDL/include" -I "$SDL/include/SDL2" )
elif command -v sdl2-config >/dev/null 2>&1; then
  # Space-safe: one flag per word is fine, sdl2-config never emits paths with spaces on a
  # sane install, and this is only the fallback for a host that has no private SDL2 yet.
  SDLINC=( $(sdl2-config --cflags) )
else
  echo "no SDL2 headers found." >&2
  echo "  expected $SDL/include/SDL2 -- run './getv/build_mac.sh sdl' first" >&2
  exit 1
fi

# Host arch, asked of the kernel rather than of uname, for the reason documented at the top
# of getv/build_mac.sh: under an x86_64 Homebrew bash in Rosetta, uname -m reports the
# process architecture and the library would be built for the wrong slice.
TARGETFLAGS=()
if [ "$(uname -s)" = "Darwin" ]; then
  ARCH="$(uname -m)"
  if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = "1" ]; then ARCH="arm64"; fi
  TARGETFLAGS=( -target "${ARCH}-apple-macos13.0" -isysroot "$(xcrun -sdk macosx --show-sdk-path)" )
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
  "$SRC/backends/imgui_impl_opengl2.cpp"
)
# Metal renderer backend, Darwin only: getv/port/fast3d/gfx_metal.mm #includes
# imgui_impl_metal.h under GE_WITH_IMGUI unconditionally (it is the same renderer file
# on every Apple platform), so this prefix has to carry it even though build_mac.sh's
# own launcher still renders through the OpenGL2 backend above -- see that file's header
# comment for why GL2, not GL3, is the right choice for the launcher specifically. This
# is a second, independent backend alongside it, not a replacement.
if [ "$(uname -s)" = "Darwin" ]; then
  SOURCES+=( "$SRC/backends/imgui_impl_metal.mm" )
fi

# -fobjc-arc is Objective-C ARC and only means anything for the .mm Metal backend, which is
# added to SOURCES on Darwin alone. Passing it unconditionally was harmless on macOS, where
# clang ignores it for a .cpp, and fatal on Linux, where g++ rejects the flag outright:
# "unrecognized command-line option '-fobjc-arc'". All seven translation units failed, and
# because a failed ImGui fetch is non-fatal the installer printed one line about the launcher
# being unavailable and carried on. So Linux has silently never had the launcher, and nothing
# in the build counts or the self-test would ever have said so.
ARCFLAG=()
if [ "$(uname -s)" = "Darwin" ]; then ARCFLAG=(-fobjc-arc); fi

ok=0; fail=0
for f in "${SOURCES[@]}"; do
  b="$(basename "$f")"; b="${b%.*}"
  [ -f "$f" ] || { fail=$((fail+1)); echo "  MISSING: $f"; continue; }
  if "$CXX" -c "$f" -o "$OBJDIR/$b.o" \
       ${TARGETFLAGS[@]+"${TARGETFLAGS[@]}"} \
       -std=c++17 -O2 -fPIC -fno-exceptions -fno-rtti ${ARCFLAG[@]+"${ARCFLAG[@]}"} \
       -I "$SRC" -I "$SRC/backends" ${SDLINC[@]+"${SDLINC[@]}"} \
       -DGL_SILENCE_DEPRECATION -w
  then
    ok=$((ok+1))
  else
    fail=$((fail+1)); echo "  FAILED: $b.cpp"
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
   "$SRC/backends/imgui_impl_sdl2.h" "$SRC/backends/imgui_impl_opengl2.h" \
   "$PREFIX/include/" || exit 1
if [ "$(uname -s)" = "Darwin" ]; then
  cp "$SRC/backends/imgui_impl_metal.h" "$PREFIX/include/" || exit 1
fi
cp "$SRC/LICENSE.txt" "$PREFIX/IMGUI-LICENSE.txt" 2>/dev/null

echo "dear imgui $VERSION: $ok objects -> $PREFIX/lib/libimgui.a"
echo "rebuild the game to pick it up: ./getv/$BUILDSCRIPT all"
echo "then run it with: GETV_IMGUI=1 ./getv/$BUILDSCRIPT run"
