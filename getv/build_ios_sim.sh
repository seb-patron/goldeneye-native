#!/usr/bin/env bash
# Build and run Goldeneye-Native on the iOS SIMULATOR. Mirrors build_sim.sh (tvOS
# simulator) almost exactly -- see that file's header comment for why a simulator target
# exists alongside the device one (screenshotable, no physical hardware needed). The one
# iPhone/iPad-specific addition is virtual on-screen controls (GCVirtualController),
# wired up in ge_launcher.cpp / the game's input layer, not in this build script.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$HERE/../vendor/ge-decomp"
SLOT="${GETV_SLOT:-}"
BUILD="$HERE/build-ios-sim${SLOT:+-$SLOT}"
DD="$BUILD-dd"
PROJ_NAME="GoldeneyeNativeIOSSim${SLOT:+$SLOT}"
SPEC="$HERE/project-ios-sim${SLOT:+-$SLOT}.yml"
SDK="$(xcrun -sdk iphonesimulator --show-sdk-path)"
IMGUI="$HOME/.n64tvos/imgui-iossim"
IMGUIFLAGS=(); IMGUILIBS=()
if [ -f "$IMGUI/lib/libimgui.a" ] && [ -f "$IMGUI/include/imgui.h" ]; then
  IMGUIFLAGS=( -DGE_WITH_IMGUI -I "$IMGUI/include" )
  IMGUILIBS=( "$IMGUI/lib/libimgui.a" )
fi
SDL="${N64TVOS_PREFIX:-$HOME/.n64tvos}/sdl2-iossim"
TARGET="arm64-apple-ios15.0-simulator"
BUNDLE_ID="org.goldeneyenative.getv"
SIM_NAME="${GETV_SIM:-iPhone 17}"
RENDERER="${GETV_RENDERER:-metal}"
case "$RENDERER" in
  gl|metal) ;;
  *) echo "unknown GETV_RENDERER: $RENDERER (want gl or metal)" >&2; exit 1 ;;
esac

sim_udid() {
  xcrun simctl list devices available \
    | awk -v n="$SIM_NAME" '
        /^-- iOS/   { ok = 1; next }
        /^-- /      { ok = 0; next }
        ok && index($0, n) && match($0, /[0-9A-F-]{36}/) { u = substr($0, RSTART, RLENGTH) }
        END { print u }'
}

cmd_boot() {
  local u; u="$(sim_udid)"
  [ -n "$u" ] || { echo "no simulator matching '$SIM_NAME' -- run 'xcrun simctl list devicetypes' / create one, or set GETV_SIM"; return 1; }
  echo "simulator: $SIM_NAME ($u)"
  xcrun simctl boot "$u" 2>/dev/null || true
  xcrun simctl bootstatus "$u" -b 2>/dev/null || true
  echo "booted."
}

cmd_shot() {
  local u out; u="$(sim_udid)"
  out="${1:-$HERE/build-ios-sim/shot.png}"
  mkdir -p "$(dirname "$out")"
  xcrun simctl io "$u" screenshot --type=png "$out" >/dev/null 2>&1 \
    && echo "screenshot -> $out" || echo "screenshot FAILED"
}

cmd_run() {
  local u; u="$(sim_udid)"
  local app="$DD/Build/Products/Release-iphonesimulator/Goldeneye-Native-iOS.app"
  [ -d "$app" ] || { echo "no app at $app - run '$0 app' first"; return 1; }
  xcrun simctl uninstall "$u" "$BUNDLE_ID" >/dev/null 2>&1 || true
  xcrun simctl install "$u" "$app" || return 1
  local -a envargs=()
  local name val
  while IFS='=' read -r name val; do
    case "$name" in
      GETV_*) envargs+=("SIMCTL_CHILD_$name=$val") ;;
    esac
  done < <(env)
  if [ ${#envargs[@]} -gt 0 ]; then
    echo "forwarding: ${envargs[*]}"
    env "${envargs[@]}" xcrun simctl launch --console-pty "$u" "$BUNDLE_ID"
  else
    xcrun simctl launch --console-pty "$u" "$BUNDLE_ID"
  fi
}

build_port_layer() {
  local pok=0 pfail=0
  local PORTFLAGS=(
    -target "$TARGET" -isysroot "$SDK"
    -I "$HERE/port" -I "$HERE/port/include" -I "$HERE/port/fast3d" -I "$HERE/port/src"
    -I "$SDL/include" -I "$SDL/include/SDL2"
    -DTARGET_N64 -DGE_PORT_NATIVE -D_LANGUAGE_C=1 -DWAPI_SDL2
    $([ "$RENDERER" = "metal" ] && echo -DRAPI_METAL || echo -DRAPI_GL)
    $([ "$RENDERER" = "gl" ] && echo "-DUSE_GLES")
    -Wno-everything -Werror=return-type -ferror-limit=0 -O1
    ${IMGUIFLAGS[@]+"${IMGUIFLAGS[@]}"}
  )
  for f in "$HERE"/port/fast3d/*.c "$HERE"/port/src/*.c "$HERE"/port/audio/*.c; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.c}").o"
    if clang "${PORTFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"; echo "  sim port FAILED: $(basename "$f")"; fi
  done
  for f in "$HERE"/port/fast3d/*.mm "$HERE"/port/src/*.mm; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.mm}").o"
    if clang++ "${PORTFLAGS[@]}" -std=c++17 -fno-exceptions -fno-rtti -fobjc-arc -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"; echo "  sim port FAILED: $(basename "$f")"; fi
  done
  for f in "$HERE"/port/src/*.cpp; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.cpp}").o"
    if clang++ "${PORTFLAGS[@]}" -std=c++17 -fno-exceptions -fno-rtti -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"; echo "  sim port FAILED: $(basename "$f")"; fi
  done
  echo "sim port layer: $pok built, $pfail failed"
}

cmd_lib() {
  mkdir -p "$BUILD/obj"
  local CFLAGS=(
    -target "$TARGET" -isysroot "$SDK"
    -fms-extensions -include src/ge_port_decls.h
    -I . -I include -I include/PR -I src -I src/game -I src/inflate
    -DVERSION_US -DLANG_US -DREFRESH_NTSC -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM
    -DBUGFIX_R0 -DTARGET_N64 -DGE_PORT_NATIVE
    -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C=1
    -Wno-everything -Werror=return-type -ferror-limit=0 -fno-strict-aliasing -O1
  )
  if [ "${GETV_DEBUGMENU:-0}" = "1" ]; then
    CFLAGS+=(-DDEBUGMENU)
    echo "  GETV_DEBUGMENU=1 -- debug menu ENABLED (C-Up+C-Down). START is repurposed."
  fi
  local ok=0 fail=0
  while read -r f; do
    local o="$BUILD/obj/$(echo "${f%.c}" | tr '/' '_').o"
    if ( cd "$DECOMP" && clang "${CFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null ); then
      ok=$((ok+1)); else fail=$((fail+1)); rm -f "$o"; fi
  done < <(cd "$DECOMP" && { find src -name '*.c' \
             -not -path 'src/libultra/*' -not -path 'src/libultrare/*' \
             -not -name 'ge_layout_audit.c' -not -name 'ge_asset_fileview_check.c'
           find src/libultra/gu -name '*.c'; } | grep -vE '/(ramromreplay\.c|audi\.c|usb\.c|rmon\.c|sched\.c|ramrom\.c|init\.c|indy_comms\.c|indy_commands\.c|tlb_manage\.c)$' | sort)
  echo "sim game: $ok built, $fail failed"

  local sok=0 sfail=0
  while read -r f; do
    local o="$BUILD/obj/$(echo "${f%.c}" | tr '/' '_').o"
    if ( cd "$DECOMP" && clang "${CFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null ); then
      sok=$((sok+1)); else sfail=$((sfail+1)); rm -f "$o"; fi
  done < <(cd "$DECOMP" && find assets -name '*.c' ! -name '*.inc.c' | sort)
  echo "sim assets: $sok built, $sfail failed"

  local aok=0 afail=0
  while read -r f; do
    local o="$BUILD/obj/$(echo "${f%.c}" | tr '/' '_').o"
    if ( cd "$DECOMP" && clang "${CFLAGS[@]}" -DGE_AUDIO_MIXER -DNDEBUG \
           -I src/libultra -I src/libultrare -I "$HERE/port/audio" -c "$f" -o "$o" 2>/dev/null ); then
      aok=$((aok+1)); else afail=$((afail+1)); rm -f "$o"; fi
  done < <(cd "$DECOMP" && find src/libultra/audio src/libultrare/audio -name '*.c' 2>/dev/null | sort)
  echo "sim audio: $aok built, $afail failed"

  build_port_layer

  rm -f "$BUILD/libge.a"
  ar rcs "$BUILD/libge.a" "$BUILD"/obj/*.o && echo "sim libge.a: $(du -h "$BUILD/libge.a" | cut -f1)"
}

cmd_port() {
  if [ ! -d "$BUILD/obj" ] || [ -z "$(ls -A "$BUILD/obj" 2>/dev/null)" ]; then
    echo "no objects in $BUILD/obj -- run './build_ios_sim.sh lib' once for this slot first"
    return 1
  fi
  build_port_layer
  rm -f "$BUILD/libge.a"
  ar rcs "$BUILD/libge.a" "$BUILD"/obj/*.o && echo "sim libge.a: $(du -h "$BUILD/libge.a" | cut -f1)"
}

cmd_app() {
  # Same sed-derivation trick as build_sim.sh, from project_ios.yml instead of
  # project.yml -- see that file's identical comment for why a full regen (not an
  # in-place edit) and why the substitution is checked rather than trusted.
  sed -e "s|\${N64TVOS_PREFIX}/sdl2-ios|$SDL|g" \
      -e "s|\$(SRCROOT)/build-ios/libge.a|\$(SRCROOT)/$(basename "$BUILD")/libge.a|" \
      -e "s|^name: Goldeneye-Native-iOS$|name: $PROJ_NAME|" \
      "$HERE/project_ios.yml" > "$SPEC"
  grep -q "sdl2-iossim" "$SPEC" || { echo "SDL substitution FAILED"; return 1; }
  grep -q "$(basename "$BUILD")/libge.a" "$SPEC" || { echo "libge.a path substitution FAILED"; return 1; }
  export GETV_RAPI_DEFINE
  GETV_RAPI_DEFINE="$([ "$RENDERER" = "metal" ] && echo RAPI_METAL || echo RAPI_GL)"
  export GETV_IMGUI_LIB="${IMGUILIBS[0]:-}"
  rm -rf "$HERE/$PROJ_NAME.xcodeproj"
  ( cd "$HERE" && xcodegen generate --spec "$(basename "$SPEC")" >/dev/null 2>&1 ) || {
      echo "xcodegen failed"; return 1; }
  xcodebuild -project "$HERE/$PROJ_NAME.xcodeproj" -scheme Goldeneye-Native-iOS \
    -configuration Release -sdk iphonesimulator \
    CODE_SIGNING_ALLOWED=NO ARCHS=arm64 ONLY_ACTIVE_ARCH=NO VALID_ARCHS=arm64 \
    -derivedDataPath "$DD" build 2>&1 | grep -E "error:|BUILD SUCCEEDED|BUILD FAILED" | head -5
}

case "${1:-}" in
  lib)  cmd_lib ;;
  port) cmd_port ;;
  app)  cmd_app ;;
  boot) cmd_boot ;;
  shot) cmd_shot "${2:-}" ;;
  run)  cmd_run ;;
  env)  echo "SDK=$SDK"; echo "SDL=$SDL"; echo "TARGET=$TARGET"; echo "SLOT=${SLOT:-<none>}"; echo "BUILD=$BUILD"; echo "SIM=$(sim_udid)"; echo "RENDERER=$RENDERER" ;;
  *) echo "usage: $0 {lib|port|app|boot|run|shot [path]|env}"
        echo "  port = recompile getv/port/** only and re-archive (seconds, not ~20 min)" ;;
esac
