#!/usr/bin/env bash
# GoldenEye -> iOS DEVICE harness. Mirrors build.sh (tvOS device) exactly except for the
# target triple, the SDK, the SDL/ImGui prefixes and the deploy mechanism -- see that
# file's header comment for the overall rationale (native decomp port, not emulation).
#
# No physical iPhone/iPad is paired yet, unlike the tvOS Apple TV on the LAN. `lib` and
# `app` are fully usable today; `deploy` needs DEV_DEVICECTL_IOS set to a real device UDID
# once one exists (`xcrun devicectl list devices` after pairing one over USB/Wi-Fi). Until
# then, use build_ios_sim.sh -- it needs no physical hardware and can be screenshotted.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$HERE/../vendor/ge-decomp"
BUILD="$HERE/build-ios"
SDK="$(xcrun -sdk iphoneos --show-sdk-path)"
TARGET="arm64-apple-ios15.0"
IMGUI="$HOME/.n64tvos/imgui-ios"
IMGUIFLAGS=(); IMGUILIBS=()
if [ -f "$IMGUI/lib/libimgui.a" ] && [ -f "$IMGUI/include/imgui.h" ]; then
  IMGUIFLAGS=( -DGE_WITH_IMGUI -I "$IMGUI/include" )
  IMGUILIBS=( "$IMGUI/lib/libimgui.a" )
fi
# GETV_RENDERER=gl|metal (default metal -- unlike tvOS's build.sh, which defaults to gl
# for historical reasons; iOS bring-up started after Metal was already the proven
# renderer, so there is no reason to default to the GLES path here).
RENDERER="${GETV_RENDERER:-metal}"
case "$RENDERER" in
  gl|metal) ;;
  *) echo "unknown GETV_RENDERER: $RENDERER (want gl or metal)" >&2; exit 1 ;;
esac

CFLAGS=(
  -target "$TARGET" -isysroot "$SDK"
  -fms-extensions
  -include src/ge_port_decls.h
  -I . -I include -I include/PR -I src -I src/game -I src/inflate
  -DVERSION_US -DLANG_US -DREFRESH_NTSC -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM
  -DBUGFIX_R0 -DTARGET_N64 -DGE_PORT_NATIVE
  -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C=1
  -Wno-everything -Werror=return-type -ferror-limit=0 -fno-strict-aliasing -O1
)

GE_HELD_BACK_RE='ramromreplay\.c|audi\.c|usb\.c|rmon\.c|sched\.c|ramrom\.c|init\.c|indy_comms\.c|indy_commands\.c|tlb_manage\.c'

sources() {
  (cd "$DECOMP" && { find src -name '*.c' \
      -not -path 'src/libultra/*' -not -path 'src/libultrare/*' \
      -not -name 'ge_layout_audit.c' -not -name 'ge_asset_fileview_check.c'
    find src/libultra/gu -name '*.c'
  } | grep -vE "/($GE_HELD_BACK_RE)$" | sort)
}

audio_sources() {
  (cd "$DECOMP" && for f in \
      auxbus bnkf cents2ratio copy cseq cspgetstate csplayer cspplay cspsetseq \
      cspsetvol cspstop event filter heapalloc heapinit load mainbus resample save \
      seq seqplayer seqpsetbank sl synaddplayer synallocfx synallocvoice syndelete \
      synfreevoice synsetfxmix synsetpan synsetpitch synsetpriority synsetvol \
      synstartvoice synstartvoiceparam synstopvoice synthesizer; do
    echo "src/libultra/audio/$f.c"
  done
  for f in drvrNew env reverb; do echo "src/libultrare/audio/$f.c"; done)
}

asset_sources() {
  (cd "$DECOMP" && find assets -name '*.c' ! -name '*.inc.c' | sort)
}

cmd_lib() {
  mkdir -p "$BUILD/obj"
  local ok=0 fail=0 failed=()
  cd "$DECOMP"
  while read -r f; do
    local o="$BUILD/obj/$(echo "${f%.c}" | tr '/' '_').o"
    if clang "${CFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null; then
      ok=$((ok+1))
    else
      fail=$((fail+1)); failed+=("$f")
    fi
  done < <(sources)

  local uok=0 ufail=0
  while read -r f; do
    local o="$BUILD/obj/$(echo "${f%.c}" | tr '/' '_').o"
    if clang "${CFLAGS[@]}" -DGE_AUDIO_MIXER -DNDEBUG \
         -I src/libultra -I src/libultrare -I "$HERE/port/audio" -c "$f" -o "$o" 2>/dev/null; then
      uok=$((uok+1))
    else
      ufail=$((ufail+1)); echo "  libultra audio FAILED: $f"
    fi
  done < <(audio_sources)
  printf 'libultra audio: %d built, %d failed\n' "$uok" "$ufail"

  if clang -target "$TARGET" -isysroot "$SDK" \
       -I "$HERE/port/include" -I "$HERE/port/audio" \
       -D_LANGUAGE_C=1 -DTARGET_N64 -DGE_PORT_NATIVE \
       -Wno-everything -Werror=return-type -ferror-limit=0 -O2 -c "$HERE/port/audio/ge_mixer.c" \
       -o "$BUILD/obj/port_ge_mixer.o" 2>/dev/null; then
    echo "software RSP: ge_mixer.o built"
  else
    echo "  port FAILED: ge_mixer.c"
  fi

  local aok=0 afail=0
  while read -r f; do
    local o="$BUILD/obj/asset_$(echo "${f%.c}" | tr '/' '_').o"
    if clang "${CFLAGS[@]}" -I assets -c "$f" -o "$o" 2>/dev/null; then
      aok=$((aok+1))
    else
      afail=$((afail+1))
    fi
  done < <(asset_sources)
  printf 'assets: %d built, %d failed\n' "$aok" "$afail"

  printf '\ncompiled %d / %d  (%d still failing)\n' "$ok" "$((ok+fail))" "$fail"
  if [ "$fail" -gt 0 ]; then
    printf '%s\n' "${failed[@]}" > "$BUILD/failing.txt"
    echo "failing list -> $BUILD/failing.txt"
  fi

  local PORTFLAGS=(
    -target "$TARGET" -isysroot "$SDK"
    -I "$HERE/port" -I "$HERE/port/include" -I "$HERE/port/fast3d" -I "$HERE/port/src"
    -I ${N64TVOS_PREFIX:-$HOME/.n64tvos}/sdl2-ios/include
    -I ${N64TVOS_PREFIX:-$HOME/.n64tvos}/sdl2-ios/include/SDL2
    -DTARGET_N64 -DGE_PORT_NATIVE -D_LANGUAGE_C=1 -DWAPI_SDL2
    $([ "$RENDERER" = "metal" ] && echo -DRAPI_METAL || echo -DRAPI_GL)
    $([ "$RENDERER" = "gl" ] && echo "-DUSE_GLES")
    -Wno-everything -Werror=return-type -ferror-limit=0 -O1
    ${IMGUIFLAGS[@]+"${IMGUIFLAGS[@]}"}
  )
  local pok=0 pfail=0
  for f in "$HERE"/port/fast3d/*.c "$HERE"/port/src/*.c; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.c}").o"
    if clang "${PORTFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null; then
      pok=$((pok+1))
    else
      pfail=$((pfail+1)); echo "  port FAILED: $(basename "$f")"
    fi
  done
  for f in "$HERE"/port/fast3d/*.mm "$HERE"/port/src/*.mm; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.mm}").o"
    if clang++ "${PORTFLAGS[@]}" -std=c++17 -fno-exceptions -fno-rtti -fobjc-arc -c "$f" -o "$o" 2>/dev/null; then
      pok=$((pok+1))
    else
      pfail=$((pfail+1)); echo "  port FAILED: $(basename "$f")"
    fi
  done
  for f in "$HERE"/port/src/*.cpp; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.cpp}").o"
    if clang++ "${PORTFLAGS[@]}" -std=c++17 -fno-exceptions -fno-rtti -c "$f" -o "$o" 2>/dev/null; then
      pok=$((pok+1))
    else
      pfail=$((pfail+1)); echo "  port FAILED: $(basename "$f")"
    fi
  done
  printf 'port layer: %d built, %d failed\n' "$pok" "$pfail"

  if [ "$ok" -gt 0 ]; then
    rm -f "$BUILD/libge.a"
    ar rcs "$BUILD/libge.a" "$BUILD"/obj/*.o
    echo "archived -> $BUILD/libge.a ($(du -h "$BUILD/libge.a" | cut -f1))"
    local platforms
    platforms="$(otool -l "$BUILD/libge.a" 2>/dev/null | grep -A2 LC_BUILD_VERSION | grep -c 'platform 2')"
    if [ "${platforms:-0}" -gt 0 ]; then
      echo "verified: platform 2 (iOS), arm64"
    else
      echo "WARNING: archive is not iOS platform 2 -- check the SDK and -target"
    fi
  fi
}

BUNDLE_ID="org.goldeneyenative.getv"

cmd_app() {
  export GETV_RAPI_DEFINE
  GETV_RAPI_DEFINE="$([ "$RENDERER" = "metal" ] && echo RAPI_METAL || echo RAPI_GL)"
  export N64TVOS_PREFIX="${N64TVOS_PREFIX:-$HOME/.n64tvos}"
  export GETV_IMGUI_LIB="${IMGUILIBS[0]:-}"
  rm -rf "$HERE/Goldeneye-Native-iOS.xcodeproj"
  ( cd "$HERE" && xcodegen generate --spec project_ios.yml ) || return 1
  xcodebuild -project "$HERE/Goldeneye-Native-iOS.xcodeproj" -scheme Goldeneye-Native-iOS \
    -configuration Release -destination "generic/platform=iOS" \
    -derivedDataPath "$HERE/build-ios-device" -allowProvisioningUpdates build
}

cmd_deploy() {
  local app="$HERE/build-ios-device/Build/Products/Release-iphoneos/Goldeneye-Native-iOS.app"
  [ -d "$app" ] || { echo "no app at $app - run '$0 app' first"; return 1; }
  # "iPhone18ProS", an iPhone 14 Pro -- found paired via `xcrun devicectl list devices`
  # on 2026-08-26. Override with DEV_DEVICECTL_IOS if a different device is the target.
  local DEV_DEVICECTL_IOS="${DEV_DEVICECTL_IOS:-3AA1F63F-53C0-5C57-A19F-06BC22CBB1B6}"
  xcrun devicectl device uninstall app --device "$DEV_DEVICECTL_IOS" "$BUNDLE_ID" || true
  xcrun devicectl device install app --device "$DEV_DEVICECTL_IOS" "$app"
  xcrun devicectl device process launch --device "$DEV_DEVICECTL_IOS" --console "$BUNDLE_ID"
}

cmd_clean() { rm -rf "$BUILD" "$HERE/build-ios-device"; echo "cleaned"; }

case "${1:-lib}" in
  lib)    cmd_lib ;;
  app)    cmd_app ;;
  deploy) cmd_deploy ;;
  all)    cmd_lib && cmd_app && cmd_deploy ;;
  clean)  cmd_clean ;;
  *) echo "usage: $0 {lib|app|deploy|all|clean}"; exit 2 ;;
esac
