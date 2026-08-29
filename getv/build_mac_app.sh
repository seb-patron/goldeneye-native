#!/usr/bin/env bash
# Build and run the native macOS .app (project-mac.yml) -- the SwiftUI launcher + game in a
# real, double-clickable, code-signed bundle, unlike build_mac.sh's own `app` command, which
# links a bare command-line binary with no bundle, no Info.plist, no signing at all. That
# script still owns everything BELOW the launcher (compiling the game/assets/audio/port
# layer into libge.a) -- this one only adds the Xcode wrapper on top, mirroring exactly how
# build_ios_sim.sh's `app` command relates to its own game-object compile step. Never reads,
# writes or sources build_mac.sh; the two are additive, same rule build_mac.sh states for
# itself relative to build.sh/build_sim.sh.
#
# usage: GETV_RENDERER=gl|metal ./build_mac_app.sh {lib|app|run|all|env}
#   lib = build_mac.sh lib && build_mac.sh port for this renderer (produces libge.a)
#   app = xcodegen generate + xcodebuild the .app (needs `lib` to have run at least once)
#   run = launch the built .app directly (not `open`, so its stdout/stderr stay attached
#         to this terminal -- `open` detaches them, which is exactly the tail you need to
#         see a launcher crash or the boot log GETV_LOGFLUSH=1 asks for)
#   all = lib && app
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RENDERER="${GETV_RENDERER:-gl}"
case "$RENDERER" in
  gl|metal) ;;
  *) echo "unknown GETV_RENDERER: $RENDERER (want gl or metal)" >&2; exit 1 ;;
esac
# Matches build_mac.sh's own per-renderer BUILD dir exactly -- has to, since this script
# links against whatever libge.a that one produced.
if [ "$RENDERER" = "metal" ]; then
  MAC_BUILD_DIR="build-mac-metal"
else
  MAC_BUILD_DIR="build-mac"
fi
N64TVOS_PREFIX="${N64TVOS_PREFIX:-$HOME/.n64tvos}"
DD="$HERE/build-mac-app-dd"
APP="$DD/Build/Products/Release/Goldeneye-Native-Mac.app"

cmd_lib() {
  ( cd "$HERE" && GETV_RENDERER="$RENDERER" ./build_mac.sh lib )
}

cmd_app() {
  [ -f "$HERE/$MAC_BUILD_DIR/libge.a" ] || {
    echo "no $MAC_BUILD_DIR/libge.a -- run '$0 lib' first"; return 1; }

  IMGUI="$N64TVOS_PREFIX/imgui-mac"
  export GETV_IMGUI_LIB=""
  [ -f "$IMGUI/lib/libimgui.a" ] && GETV_IMGUI_LIB="$IMGUI/lib/libimgui.a"

  LUA="$N64TVOS_PREFIX/lua-mac"
  export GETV_LUA_LIB=""
  [ -f "$LUA/lib/liblua.a" ] && GETV_LUA_LIB="$LUA/lib/liblua.a"

  export GETV_MAC_BUILD_DIR="$MAC_BUILD_DIR"
  export N64TVOS_PREFIX
  export GETV_RAPI_DEFINE
  GETV_RAPI_DEFINE="$([ "$RENDERER" = "metal" ] && echo RAPI_METAL || echo RAPI_GL)"

  # Same delete-then-regenerate rationale as build.sh/build_ios_sim.sh's identical rm: an
  # in-place regen over an existing xcodeproj can leave the scheme's buildable "supported
  # platforms" empty, which surfaces later as an opaque "Found no destinations" failure.
  rm -rf "$HERE/Goldeneye-Native-Mac.xcodeproj"
  ( cd "$HERE" && xcodegen generate --spec project-mac.yml ) || { echo "xcodegen failed"; return 1; }

  local host_arch
  host_arch="$(sysctl -n hw.optional.arm64 2>/dev/null)"
  local arch; arch="$([ "$host_arch" = "1" ] && echo arm64 || echo x86_64)"

  xcodebuild -project "$HERE/Goldeneye-Native-Mac.xcodeproj" -scheme Goldeneye-Native-Mac \
    -configuration Release -destination "generic/platform=macOS" \
    CODE_SIGNING_ALLOWED=YES ARCHS="$arch" ONLY_ACTIVE_ARCH=YES VALID_ARCHS="$arch" \
    -derivedDataPath "$DD" build 2>&1 | grep -E "error:|warning: .*Swift|BUILD SUCCEEDED|BUILD FAILED"
  [ -d "$APP" ] || { echo "no .app produced at $APP"; return 1; }
  echo "built: $APP"
}

cmd_run() {
  [ -d "$APP" ] || { echo "no app at $APP -- run '$0 app' first"; return 1; }
  local bin="$APP/Contents/MacOS/Goldeneye-Native-Mac"
  [ -x "$bin" ] || { echo "no executable at $bin"; return 1; }
  "$bin" "$@"
}

case "${1:-}" in
  lib)  cmd_lib ;;
  app)  cmd_app ;;
  run)  shift; cmd_run "$@" ;;
  all)  cmd_lib && cmd_app ;;
  env)  echo "RENDERER=$RENDERER"; echo "MAC_BUILD_DIR=$MAC_BUILD_DIR"; echo "APP=$APP" ;;
  *)    echo "usage: GETV_RENDERER=gl|metal $0 {lib|app|run|all|env}" ;;
esac
