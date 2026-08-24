#!/usr/bin/env bash
# Fetch and build Lua for the mod scripting host (getv/port/src/ge_lua.c).
#
# Lua is not vendored into this repository. deps/ is gitignored (see .gitignore), which is
# the same arrangement SDL2 already uses: upstream source is fetched on demand and built
# into a prefix outside the tree, so the repository never carries third-party code it did
# not write. Run this once; the build scripts detect the result and enable scripting.
#
# Without it, everything still builds. ge_lua.c compiles its hook functions to empty
# bodies, the build omits -DGE_WITH_LUA, and mods/ is ignored. Scripting is an addition,
# never a prerequisite.
#
# Licence: Lua is MIT (deps/lua-5.4.7/doc/readme.html). That is compatible with this
# project's position in docs/LICENSING.md and, unlike the GPL projects listed in
# docs/THIRD_PARTY.md, imposes nothing on the rest of the tree. Attribution belongs in
# docs/THIRD_PARTY.md, not in a comment here.
set -uo pipefail

VERSION="5.4.7"
# Published by lua.org for lua-5.4.7.tar.gz. Checked rather than trusted, because this
# script downloads a tarball and then compiles it into the game binary.
SHA256="9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="$HERE/../deps"
SRC="$DEPS/lua-$VERSION"

case "$(uname -s)" in
  Darwin) PREFIX="$HOME/.n64tvos/lua-mac";   PLATDEF="-DLUA_USE_MACOSX" ;;
  Linux)  PREFIX="$HOME/.n64tvos/lua-linux"; PLATDEF="-DLUA_USE_LINUX"  ;;
  *) echo "unsupported host: $(uname -s)" >&2; exit 1 ;;
esac

CC="${CC:-cc}"

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
  echo "fetching lua $VERSION"
  TARBALL="$DEPS/lua-$VERSION.tar.gz"
  curl -sSL --max-time 120 -o "$TARBALL" "https://www.lua.org/ftp/lua-$VERSION.tar.gz" || {
    echo "download failed" >&2; exit 1; }

  if command -v shasum >/dev/null 2>&1; then GOT="$(shasum -a 256 "$TARBALL" | cut -d' ' -f1)"
  else                                       GOT="$(sha256sum   "$TARBALL" | cut -d' ' -f1)"; fi
  if [ "$GOT" != "$SHA256" ]; then
    echo "checksum mismatch for lua-$VERSION.tar.gz" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $GOT" >&2
    rm -f "$TARBALL"
    exit 1
  fi
  tar xzf "$TARBALL" -C "$DEPS" && rm -f "$TARBALL"
fi

mkdir -p "$PREFIX/lib" "$PREFIX/include"
OBJDIR="$(mktemp -d)"
trap 'rm -rf "$OBJDIR"' EXIT

# lua.c and luac.c are the standalone interpreter and bytecode compiler; both define main()
# and would collide with the game's. Everything else is the library.
ok=0; fail=0
for f in "$SRC"/src/*.c; do
  b="$(basename "$f" .c)"
  case "$b" in lua|luac) continue ;; esac
  if "$CC" -c "$f" -o "$OBJDIR/$b.o" ${TARGETFLAGS[@]+"${TARGETFLAGS[@]}"} -O2 "$PLATDEF" -w; then
    ok=$((ok+1))
  else
    fail=$((fail+1)); echo "  FAILED: $b.c"
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "lua: $ok built, $fail failed -- not installing a partial library" >&2
  exit 1
fi

rm -f "$PREFIX/lib/liblua.a"
ar rcs "$PREFIX/lib/liblua.a" "$OBJDIR"/*.o || exit 1
cp "$SRC"/src/lua.h "$SRC"/src/luaconf.h "$SRC"/src/lualib.h "$SRC"/src/lauxlib.h "$PREFIX/include/"

echo "lua $VERSION: $ok objects -> $PREFIX/lib/liblua.a"
echo "rebuild the game to pick it up: ./getv/build_mac.sh all   (or build_linux.sh all)"
