#!/usr/bin/env bash
# One-shot setup for a fresh Windows checkout, run from git-bash: fetch-thirdparty, fetch
# the mingw toolchain + libraries, clone the decomp, apply patches, generate assets from
# your ROM, namespace them, apply 0002, then build.
#
# This automates docs/SETUP.md sections 2-4 for Windows, mirroring tools/setup-mac.sh. Read
# that document if any step here fails, since it explains why each one exists.
#
# Two things differ from the Mac script, both because tools/fetch_deps_windows.ps1 covers a
# different slice of section 2 than build_mac.sh's SDL2-from-source step does:
#   - no SDL2-source step -- fetch_deps_windows.ps1 installs the official prebuilt mingw
#     package instead, into the same C:\mingw64 that -Mingw below points builds at.
#   - the ROM is copied into the decomp checkout, not symlinked -- creating a symlink on
#     Windows needs Developer Mode or an elevated prompt, and most machines this script
#     runs on will have neither.
#
# What it cannot do: the ROM is yours to supply (README's "bring your own" rules). If it is
# missing this exits with the same instructions SETUP.md gives.
#
# usage (from git-bash): tools/setup-windows.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DECOMP="$HERE/vendor/ge-decomp"
ROM="$HERE/roms/ge007.u.z64"
MINGW='C:\mingw64'

die() { echo "setup-windows: $*" >&2; exit 1; }
step() { echo; echo "== $* =="; }

# ---------------------------------------------------------------------- 0. tools on PATH
step "checking for python3"
command -v python3 >/dev/null 2>&1 \
  || die "python3 not found on PATH -- install it from python.org (check \"Add to PATH\" in the installer) and re-run"

# ---------------------------------------------------------------------- 1. third-party
step "third-party port sources"
if [ -f "$HERE/getv/port/fast3d/gfx_pc.c" ]; then
  echo "already present"
else
  "$HERE/tools/fetch-thirdparty.sh" fetch || die "fetch-thirdparty failed"
fi

# ---------------------------------------------------------------------- 2. toolchain + libraries
step "mingw toolchain, SDL2, GLEW, Lua, Dear ImGui, Tracy"
if [ -f "$MINGW/bin/gcc.exe" ] && [ -f "$MINGW/include/SDL2/SDL.h" ] && [ -f "$MINGW/lib/libglew32.a" ]; then
  echo "already present at $MINGW"
else
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$HERE/tools/fetch_deps_windows.ps1" \
    || die "fetch_deps_windows.ps1 failed"
fi
export PATH="$MINGW/bin:$PATH"

# ---------------------------------------------------------------------- 3. the decomp
step "decompiled game source"
if [ -d "$DECOMP/assets" ]; then
  echo "already cloned at $DECOMP"
else
  git clone https://github.com/n64decomp/007 "$DECOMP" || die "clone failed"
fi

# 0003, 0004 and 0005 are deliberately absent: they were folded into 0001 and applying them
# on top of it fails with `patch does not apply`. See getv/patches/README.md.
for p in 0001-source 0006-fov-live-setter 0007-load-trace \
         0008-crosshair-color 0009-freerun-divider 0010-state-dump-player-position \
         0011-netplay-tick-integration 0012-real-font-overlay \
         0013-lockstep-pinned-sim-step; do
  if ( cd "$DECOMP" && git apply --reverse --check "$HERE/getv/patches/$p.patch" ) 2>/dev/null; then
    echo "$p.patch: already applied"
  else
    ( cd "$DECOMP" && git apply "$HERE/getv/patches/$p.patch" ) \
      || die "$p.patch failed to apply -- checkout may not be pristine"
  fi
done

# ---------------------------------------------------------------------- 4. the ROM
step "ROM"
[ -f "$ROM" ] || die "no ROM at $ROM -- see README.md 'Bring your own ROM'. Not something this script can fetch for you."
SHA="$(sha1sum "$ROM" | awk '{print $1}')"
WANT="abe01e4aeb033b6c0836819f549c791b26cfde83"
[ "$SHA" = "$WANT" ] || die "ROM SHA-1 $SHA does not match $WANT -- see docs/SETUP.md 3.4"
cp -f "$ROM" "$DECOMP/baserom.u.z64"
echo "ROM ok, copied into $DECOMP/baserom.u.z64"

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
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$HERE/getv/build_windows.ps1" -Target all -Mingw "$MINGW" \
  || die "build failed"

echo
echo "setup-windows: done -- run getv\\build-windows\\goldeneye.exe"
