#!/usr/bin/env bash
# Build and run Goldeneye-Native on the tvOS SIMULATOR.
#
# why this exists
# ---------------
# tvOS blocks devicectl from screenshotting a real Apple TV, so every visual question
# this port has hit -- "is it centred?", "is the font legible?" -- has had to go through
# a human looking at the TV. The SIMULATOR can be screenshotted with `simctl io screenshot`,
# which turns those into things we can measure directly.
#
# It is a DIFFERENT PLATFORM from the device, not a flag on it:
#   device    -> -target arm64-apple-tvos17.0            -sdk appletvos        (platform 3)
#   simulator -> -target arm64-apple-tvos17.0-simulator  -sdk appletvsimulator (platform 8)
# Linking a device SDL2 into a simulator binary fails at link time with a platform
# mismatch, which is why deps/sdl2-tvsim exists separately from the device build.
#
# Kept OUT of build.sh deliberately: several agents share that script, and the device
# path is the one that ships. This is an additive diagnostic tool.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$HERE/../vendor/ge-decomp"
# per-SLOT build isolation. Parallel builds used to share build-sim/obj and libge.a, so
# two concurrent `build_sim.sh lib` runs silently clobbered each other's objects and
# archive -- which shows up as an inexplicably changed "N built" count or a link error in
# a file nobody touched. Set GETV_SLOT=<name> to get a private object dir, archive,
# derived-data dir and generated xcodeproj. Unset behaves exactly as before.
SLOT="${GETV_SLOT:-}"
BUILD="$HERE/build-sim${SLOT:+-$SLOT}"
DD="$BUILD-dd"
PROJ_NAME="GoldeneyeNativeSim${SLOT:+$SLOT}"
SPEC="$HERE/project-sim${SLOT:+-$SLOT}.yml"
SDK="$(xcrun -sdk appletvsimulator --show-sdk-path)"
# A SPACE-FREE PATH. The repo lives under ".../Code Projects/...", and an
# unquoted HEADER_SEARCH_PATHS entry containing a space is silently split by
# xcodebuild -- which presents as "SDL.h file not found" even though the path is
# right there in project.yml. The device build never hit this because
# The SDL2 prefix must not contain spaces. Symlink into the same
# space-free directory and use that.
SDL="${N64TVOS_PREFIX:-$HOME/.n64tvos}/sdl2-tvsim"
TARGET="arm64-apple-tvos17.0-simulator"
BUNDLE_ID="org.goldeneyenative.getv"
SIM_NAME="${GETV_SIM:-Apple TV 4K (3rd generation)}"

# Pick the NEWEST runtime that hosts this device name. The list is grouped by
# "-- tvOS X.Y --" headers in ascending order, and the first match would be tvOS 16.1 --
# below our 17.0 deployment target, so the app would refuse to install.
sim_udid() {
  xcrun simctl list devices available \
    | awk -v n="$SIM_NAME" '
        /^-- tvOS/ { ok = 1; next }
        /^-- /     { ok = 0; next }
        ok && index($0, n) && match($0, /[0-9A-F-]{36}/) { u = substr($0, RSTART, RLENGTH) }
        END { print u }'
}

cmd_boot() {
  local u; u="$(sim_udid)"
  [ -n "$u" ] || { echo "no simulator matching '$SIM_NAME'"; return 1; }
  echo "simulator: $SIM_NAME ($u)"
  xcrun simctl boot "$u" 2>/dev/null || true
  xcrun simctl bootstatus "$u" -b 2>/dev/null || true
  echo "booted."
}

cmd_shot() {
  local u out; u="$(sim_udid)"
  out="${1:-$HERE/build-sim/shot.png}"
  mkdir -p "$(dirname "$out")"
  xcrun simctl io "$u" screenshot --type=png "$out" >/dev/null 2>&1 \
    && echo "screenshot -> $out" || echo "screenshot FAILED"
}

cmd_run() {
  local u; u="$(sim_udid)"
  local app="$DD/Build/Products/Release-appletvsimulator/Goldeneye-Native.app"
  [ -d "$app" ] || { echo "no app at $app - run '$0 app' first"; return 1; }
  xcrun simctl uninstall "$u" "$BUNDLE_ID" >/dev/null 2>&1 || true
  xcrun simctl install "$u" "$app" || return 1
  # simctl launch --console streams stdout, and unlike devicectl it does not wedge.
  #
  # Forward EVERY GETV_* knob, not just GETV_STAGE. simctl passes the child's
  # environment only through SIMCTL_CHILD_<NAME>, so a var that is merely exported in
  # this shell is silently dropped -- which looks exactly like the feature it gates
  # being broken. Currently: GETV_STAGE, GETV_INPUT_DEBUG.
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
    -DTARGET_N64 -DGE_PORT_NATIVE -D_LANGUAGE_C=1 -DRAPI_GL -DWAPI_SDL2
    # Without these, gfx_opengl.c never includes an OpenGLES header and every
    # GLuint/GLint is an unknown type. Must match build.sh or the sim build is
    # not a valid proxy for the device build.
    -DUSE_GLES -DTVOS_SUPERSAMPLE
    -Wno-everything -Werror=return-type -ferror-limit=0 -O1
    ${GETV_AUTOVARINIT:+-ftrivial-auto-var-init=$GETV_AUTOVARINIT}
  )
  for f in "$HERE"/port/fast3d/*.c "$HERE"/port/src/*.c "$HERE"/port/audio/*.c; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.c}").o"
    if clang "${PORTFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"; echo "  sim port FAILED: $(basename "$f")"; fi
  done
  echo "sim port layer: $pok built, $pfail failed"
}

cmd_lib() {
  # Mirrors build.sh's flags exactly, changing ONLY the target triple, the sysroot and
  # the SDL path. Everything else -- the -D defines, -fms-extensions, the forced include,
  # -ferror-limit=0 -- has to match or the simulator build diverges from the device build
  # and stops being a valid proxy for it.
  mkdir -p "$BUILD/obj"
  local CFLAGS=(
    -target "$TARGET" -isysroot "$SDK"
    -fms-extensions -include src/ge_port_decls.h
    -I . -I include -I include/PR -I src -I src/game -I src/inflate
    -DVERSION_US -DLANG_US -DREFRESH_NTSC -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM
    -DBUGFIX_R0 -DTARGET_N64 -DGE_PORT_NATIVE
    -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C=1
    # -Wno-everything, NOT -w. -w defeats -Werror=return-type in
    # BOTH orders (rc=0, no diagnostic); -Wno-everything lets it through. The old comment
    # here claimed order mattered -- it does not, and the guard was a no-op all along.
    # A non-void function that falls off the end "worked" on MIPS/IDO: the callee's result
    # was already in $v0, so it returned by accident and the decomp matched. clang leaves x0
    # undefined. This silently zeroed the music volume (mp_music.c) and fed garbage
    # display-list cursors to three Gfx* builders. `-w` hid all ten instances completely.
    # As an ERROR a regression shows up as a changed build count instead of a mystery.
    -Wno-everything -Werror=return-type -ferror-limit=0 -fno-strict-aliasing -O1
  )
  # boot lane (bt_): opt-in automatic-variable initialisation. MSan does not exist on
  # Darwin; -ftrivial-auto-var-init is the compiler-side substitute and it makes every
  # uninitialised automatic variable DETERMINISTIC instead of stack-residue-dependent.
  if [ -n "${GETV_AUTOVARINIT:-}" ]; then
    CFLAGS+=(-ftrivial-auto-var-init="$GETV_AUTOVARINIT")
    echo "  bt: -ftrivial-auto-var-init=$GETV_AUTOVARINIT"
  fi

  # opt-IN debug menu. `-DLEFTOVERDEBUG` above already compiles the whole ~1,100-line
  # debug menu into the binary, but `boss.c:565` gates the C-Up + C-Down trigger on
  # `DEBUGMENU`, which is defined NOWHERE in the tree -- so nothing can ever open it.
  # Defining it buys: free camera, background/props toggles, portal-cull toggle, position
  # display, all-levels unlock, and `obj load`/`weapon load` (an asset-presence test that
  # is inert on N64 but genuinely useful to us).
  #
  # opt-IN, not default, and this is deliberate. With debugmenu defined the else-chain in
  # boss.c routes **START** into debug_menu_processor, which is disruptive during normal
  # play and would silently change every lane's measurements. Turn it on per-run:
  #
  #     GETV_DEBUGMENU=1 ./build_sim.sh lib
  #
  # It changes code generation, so a slot must REBUILD when toggling it, and any number
  # measured under it is not comparable to one measured without it.
  # There is NO working level select in there -- DEB_LEVEL/REGION/SCALE are gutted no-ops
  # at debugmenu_handler.c:511-521. `GETV_STAGE` remains the only stage selector.
  # Cheaper for a headless harness: `set_debug_testingmanpos_flag(1)` is an exported
  # one-liner giving room number + XYZ + facing with no menu at all.
  if [ "${GETV_DEBUGMENU:-0}" = "1" ]; then
    CFLAGS+=(-DDEBUGMENU)
    echo "   GETV_DEBUGMENU=1 -- debug menu ENABLED (C-Up+C-Down). START is repurposed."
  fi
  local ok=0 fail=0
  while read -r f; do
    local o="$BUILD/obj/$(echo "${f%.c}" | tr '/' '_').o"
    if ( cd "$DECOMP" && clang "${CFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null ); then
      ok=$((ok+1)); else fail=$((fail+1)); rm -f "$o"; fi
  done < <(cd "$DECOMP" && { find src -name '*.c' \
             -not -path 'src/libultra/*' -not -path 'src/libultrare/*' \
             -not -name 'ge_layout_audit.c' -not -name 'ge_asset_fileview_check.c'
           find src/libultra/gu -name '*.c'; } | grep -vE '/(ramromreplay\.c|audi\.c)$' | sort)
  echo "sim game: $ok built, $fail failed"

  # assets/ holds the extracted ROM data as C source (obseg blobs, images, stan, setup).
  # Omitting it links an app with no game data at all.
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

  # A failed compile must not leave its previous object behind.
  # `ar rcs .../obj/*.o` never prunes, so a stale .o silently outlived the source that
  # produced it: one slot shipped a binary whose UsetuparchZ.o was absent from the archive
  # entirely while the build still reported "762 built, 0 failed". The counts were telling
  # the truth about THIS run and lying about the archive. Each compile loop above now
  # `rm -f "$o"` on failure, so a broken file shows up as a missing symbol at link time
  # instead of as a stale binary that looks green.
  rm -f "$BUILD/libge.a"
  ar rcs "$BUILD/libge.a" "$BUILD"/obj/*.o && echo "sim libge.a: $(du -h "$BUILD/libge.a" | cut -f1)"
}

cmd_port() {
  # Recompile ONLY getv/port/** and re-archive. `lib` rebuilds all ~823 game/asset
  # objects every run (~20 min) even when nothing outside port/ changed, which made the
  # edit->run loop for renderer/audio/input work absurdly slow. Those objects are
  # untouched, so re-archiving them is equivalent and takes seconds.
  # It reuses build_port_layer(), so the flags CANNOT drift from what `lib` uses --
  # a divergent copy of PORTFLAGS would silently stop being a valid proxy for the
  # device build.
  if [ ! -d "$BUILD/obj" ] || [ -z "$(ls -A "$BUILD/obj" 2>/dev/null)" ]; then
    echo "no objects in $BUILD/obj -- run './build_sim.sh lib' once for this slot first"
    return 1
  fi
  build_port_layer
  rm -f "$BUILD/libge.a"
  ar rcs "$BUILD/libge.a" "$BUILD"/obj/*.o && echo "sim libge.a: $(du -h "$BUILD/libge.a" | cut -f1)"
}

cmd_app() {
  # A simulator-flavoured project.yml. Same sources and Info.plist, different SDL and
  # no device signing -- the simulator does not need a provisioning profile.
  # Generate into a SEPARATE directory rather than swapping project.yml in place -- the
  # device project is shared and a failed run must not leave it half-substituted.
  # Generate IN $HERE under a different project name, so $(SRCROOT) still resolves to
  # the real source tree (Sources/, port/ are all SRCROOT-relative in project.yml).
  sed -e "s|${N64TVOS_PREFIX:-$HOME/.n64tvos}/sdl2-tvos|$SDL|g" \
      -e "s|\$(SRCROOT)/build/libge.a|\$(SRCROOT)/$(basename "$BUILD")/libge.a|" \
      -e "s|^name: Goldeneye-Native$|name: $PROJ_NAME|" \
      "$HERE/project.yml" > "$SPEC"
  grep -q "sdl2-tvsim" "$SPEC" || { echo "SDL substitution FAILED"; return 1; }
  grep -q "$(basename "$BUILD")/libge.a" "$SPEC" || { echo "libge.a path substitution FAILED"; return 1; }
  ( cd "$HERE" && xcodegen generate --spec "$(basename "$SPEC")" >/dev/null 2>&1 ) || {
      echo "xcodegen failed"; return 1; }
  xcodebuild -project "$HERE/$PROJ_NAME.xcodeproj" -scheme Goldeneye-Native \
    -configuration Release -sdk appletvsimulator \
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
  env)  echo "SDK=$SDK"; echo "SDL=$SDL"; echo "TARGET=$TARGET"; echo "SLOT=${SLOT:-<none>}"; echo "BUILD=$BUILD"; echo "SIM=$(sim_udid)" ;;
  *)    echo "usage: $0 {lib|port|app|boot|run|shot [path]|env}"
        echo "  port = recompile getv/port/** only and re-archive (seconds, not ~20 min)" ;;
esac
