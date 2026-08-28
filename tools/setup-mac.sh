#!/usr/bin/env bash
# One-shot setup for a fresh macOS checkout: fetch-thirdparty, clone the decomp, apply
# 0001, generate assets from your ROM, namespace them, apply 0002, then build.
#
# This automates docs/SETUP.md sections 2-4 exactly, in order. It does not replace that
# document, read it if any step here fails, since it explains why each one exists.
#
# What it cannot do: SDL2 source and the ROM are yours to supply (README's "bring your
# own" rules). If either is missing this exits with the same instructions SETUP.md gives.
#
# usage: tools/setup-mac.sh [--skip-sdl]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DECOMP="$HERE/vendor/ge-decomp"
SDLSRC="$HERE/deps/SDL2-2.30.9"
ROM="$HERE/roms/ge007.u.z64"
SKIP_SDL=0
[ "${1:-}" = "--skip-sdl" ] && SKIP_SDL=1

die() { echo "setup-mac: $*" >&2; exit 1; }
step() { echo; echo "== $* =="; }

# ---------------------------------------------------------------------- 1. third-party
step "third-party port sources"
if [ -f "$HERE/getv/port/fast3d/gfx_pc.c" ]; then
  echo "already present"
else
  "$HERE/tools/fetch-thirdparty.sh" fetch || die "fetch-thirdparty failed"
fi

# ---------------------------------------------------------------------- 2. SDL2 source
step "SDL2 2.30.9 source"
if [ -f "$SDLSRC/CMakeLists.txt" ]; then
  echo "already present at $SDLSRC"
elif [ "$SKIP_SDL" = "1" ]; then
  echo "skipped (--skip-sdl)"
else
  mkdir -p "$HERE/deps"
  URL="https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz"
  echo "downloading $URL"
  curl -fL "$URL" -o "$HERE/deps/SDL2-2.30.9.tar.gz" \
    || die "download failed -- fetch it yourself per docs/SETUP.md section 2.3"
  tar -xzf "$HERE/deps/SDL2-2.30.9.tar.gz" -C "$HERE/deps" \
    || die "extract failed"
  rm -f "$HERE/deps/SDL2-2.30.9.tar.gz"
  [ -f "$SDLSRC/CMakeLists.txt" ] || die "extracted tree missing $SDLSRC/CMakeLists.txt"
fi

# ---------------------------------------------------------------------- 3. the decomp
step "decompiled game source"
if [ -d "$DECOMP/assets" ]; then
  echo "already cloned at $DECOMP"
else
  git clone https://github.com/n64decomp/007 "$DECOMP" || die "clone failed"
fi

if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/0001-source.patch" ) 2>/dev/null; then
  echo "0001-source.patch: already applied"
else
  ( cd "$DECOMP" && git apply "$HERE/getv/patches/0001-source.patch" ) \
    || die "0001-source.patch failed to apply -- checkout may not be pristine"
fi




if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/0006-fov-live-setter.patch" ) 2>/dev/null; then
  echo "0006-fov-live-setter.patch: already applied"
else
  ( cd "$DECOMP" && git apply "$HERE/getv/patches/0006-fov-live-setter.patch" ) \
    || die "0006-fov-live-setter.patch failed to apply"
fi

if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/0007-load-trace.patch" ) 2>/dev/null; then
  echo "0007-load-trace.patch: already applied"
else
  ( cd "$DECOMP" && git apply "$HERE/getv/patches/0007-load-trace.patch" ) \
    || die "0007-load-trace.patch failed to apply"
fi

if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/0008-crosshair-color.patch" ) 2>/dev/null; then
  echo "0008-crosshair-color.patch: already applied"
else
  ( cd "$DECOMP" && git apply "$HERE/getv/patches/0008-crosshair-color.patch" ) \
    || die "0008-crosshair-color.patch failed to apply"
fi

if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/0009-freerun-divider.patch" ) 2>/dev/null; then
  echo "0009-freerun-divider.patch: already applied"
else
  ( cd "$DECOMP" && git apply "$HERE/getv/patches/0009-freerun-divider.patch" ) \
    || die "0009-freerun-divider.patch failed to apply"
fi

if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/0010-state-dump-player-position.patch" ) 2>/dev/null; then
  echo "0010-state-dump-player-position.patch: already applied"
else
  ( cd "$DECOMP" && git apply "$HERE/getv/patches/0010-state-dump-player-position.patch" ) \
    || die "0010-state-dump-player-position.patch failed to apply"
fi

if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/0011-netplay-tick-integration.patch" ) 2>/dev/null; then
  echo "0011-netplay-tick-integration.patch: already applied"
else
  ( cd "$DECOMP" && git apply "$HERE/getv/patches/0011-netplay-tick-integration.patch" ) \
    || die "0011-netplay-tick-integration.patch failed to apply"
fi

if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/0012-real-font-overlay.patch" ) 2>/dev/null; then
  echo "0012-real-font-overlay.patch: already applied"
else
  ( cd "$DECOMP" && git apply "$HERE/getv/patches/0012-real-font-overlay.patch" ) \
    || die "0012-real-font-overlay.patch failed to apply"
fi

# ---------------------------------------------------------------------- 4. the ROM
step "ROM"
[ -f "$ROM" ] || die "no ROM at $ROM -- see README.md 'Bring your own ROM'. Not something this script can fetch for you."
SHA="$(shasum -a1 "$ROM" | awk '{print $1}')"
WANT="abe01e4aeb033b6c0836819f549c791b26cfde83"
[ "$SHA" = "$WANT" ] || die "ROM SHA-1 $SHA does not match $WANT -- see docs/SETUP.md 3.4"
ln -sf "../../roms/ge007.u.z64" "$DECOMP/baserom.u.z64"
echo "ROM ok, symlinked into $DECOMP/baserom.u.z64"

# ---------------------------------------------------------------------- 5. asset pipeline
step "asset generation (docs/SETUP.md 3.5)"
if [ -f "$DECOMP/assets/ge_animation_offsets.h" ]; then
  echo "already generated"
else
  (
    cd "$DECOMP"
    python3 "$HERE/tools/enable_bg_extraction.py"
    bash scripts/extract_baserom.u.sh
    python3 scripts/generate_chr_c.py
    python3 scripts/generate_gun_c.py
    python3 scripts/generate_prop_model_c.py
    python3 "$HERE/tools/gen_obseg_blobs.py"
    python3 scripts/make/sync_imagelist_with_def.py build/imagelist.csv
    bash scripts/make/combine_images_named.sh build/imagelist.csv assets/images/combined
    python3 "$HERE/tools/gen_images_segment.py"
    python3 "$HERE/tools/fix_asset_switchnodes.py"
    python3 "$HERE/tools/gen_anim_blobs.py"
    python3 "$HERE/tools/gen_audio_segment.py"
    python3 "$HERE/tools/gen_asset_fileview.py"
    python3 tools/gen_propdef_layout.py
  ) || die "asset generation failed -- rerun by hand per docs/SETUP.md 3.5 to see which step"
fi

# ---------------------------------------------------------------------- 6. namespacing + 0002
step "symbol namespacing (docs/SETUP.md 3.6)"
if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/0002-assets.patch" ) 2>/dev/null; then
  echo "already namespaced and patched"
else
  (
    cd "$DECOMP"
    python3 "$HERE/tools/uniquify_asset_symbols.py" assets/obseg/chr   --recurse
    python3 "$HERE/tools/uniquify_asset_symbols.py" assets/obseg/gun   --recurse
    python3 "$HERE/tools/uniquify_asset_symbols.py" assets/obseg/prop  --recurse
    python3 "$HERE/tools/uniquify_asset_symbols.py" assets/obseg/setup
    python3 "$HERE/tools/uniquify_asset_symbols.py" assets/obseg/setup/u
    python3 "$HERE/tools/uniquify_asset_symbols.py" assets/obseg/stan
  ) || die "namespacing failed"
  ( cd "$DECOMP" && git apply "$HERE/getv/patches/0002-assets.patch" ) \
    || die "0002-assets.patch failed to apply"
fi

# ---------------------------------------------------------------------- 7. build
step "build"
cd "$HERE/getv"
[ "$SKIP_SDL" = "1" ] || ./build_mac.sh sdl || die "SDL2 build failed"
./build_mac.sh all || die "build failed"

echo
echo "setup-mac: done -- run ./getv/build_mac.sh run"
