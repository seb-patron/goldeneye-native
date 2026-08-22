#!/usr/bin/env bash
# Build and run GoldenEye natively on macOS (arm64), on the EXISTING Fast3D + GL path.
#
# WHY THIS EXISTS
# ---------------
# tvOS cannot be observed. The OS blocks devicectl from reading the app container, the
# device cannot be screenshotted, and the tvOS SIMULATOR has no GPU-backed GLES at all
# (GL_RENDERER = "Apple Software Renderer", 600 ms - 2.4 s per frame). Every visual claim
# on this project so far comes from software-rendered simulator frames plus counter
# instrumentation. A Mac window on a real GPU removes that blindfold: real frame rates,
# real screenshots, a real crash report, and a build you can actually PLAY.
#
# 🔴 THIS SCRIPT IS ADDITIVE. It does not read, write or source build.sh or build_sim.sh.
# tvOS is ON HOLD, NOT ABANDONED -- `./build.sh` (device) and `./build_sim.sh` (simulator)
# must keep working, and this file exists precisely so they are never edited for Mac.
#
# 🔴 NO METAL BACKEND. Deliberately the same renderer the tvOS build uses, so a
# regression seen here is a regression there. Only the platform bindings differ:
#
#            tvOS device / sim            macOS
#   GL       OpenGL ES 3.0 (EAGL)         OpenGL 2.1 compat (NSOpenGL)
#   defines  -DUSE_GLES -DTVOS_SUPERSAMPLE  (neither) -DGE_PLATFORM_MAC
#   shaders  #version 100 ESSL            #version 120 GLSL   (already in gfx_opengl.c)
#   FBO      SDL's own FBO, blit-resolved framebuffer 0, no supersample
#   entry    libSDL2main -> UIApplicationMain -> SDL_main   main() -> SDL_main
#   window   full-screen UIWindow          resizable NSWindow
#
# ⚠️ macOS's legacy GL profile is what we want and NOT an accident: gfx_opengl.c's shader
# generator emits `#version 120` with attribute/varying, which a 3.2 CORE profile
# rejects outright. Do not "modernise" the context request.
#
# ⚠️ Supersampling is OFF here (TVOS_SUPERSAMPLE undefined), so gfx_supersample stays at
# its declared default of 1 and Fast3D renders straight to framebuffer 0. Any number
# measured here is therefore an ss=1 number -- never diff it against a tvOS ss=2 run.
#
# SDL2: this Mac's Homebrew is x86_64 under Rosetta, so brew's SDL2 is Intel-only and
# cannot be linked into an arm64 binary. `./build_mac.sh sdl` builds SDL2 2.30.9 from the
# source already in deps/ for arm64-apple-macos and installs it OUTSIDE the repo, into a
# space-free path (the repo lives under ".../Code Projects/...", and a space in a header
# search path has already cost this project a day).
#
# usage: ./build_mac.sh {sdl|lib|port|app|all|run|env}
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$HERE/../vendor/ge-decomp"
BUILD="$HERE/build-mac-final"
SDL="$HOME/.n64tvos/sdl2-mac"
SDLSRC="$HERE/../deps/SDL2-2.30.9"
SDK="$(xcrun -sdk macosx --show-sdk-path)"
TARGET="arm64-apple-macos13.0"
BIN="$BUILD/goldeneye"

# --------------------------------------------------------------------------- SDL2
cmd_sdl() {
  [ -d "$SDLSRC" ] || { echo "no SDL2 source at $SDLSRC"; return 1; }
  local b="$HOME/.n64tvos/build-sdl2-mac"
  # CMAKE_POLICY_VERSION_MINIMUM: SDL 2.30.9 declares cmake_minimum_required(3.0),
  # which CMake 4 refuses outright. This is the sanctioned opt-back-in.
  cmake -S "$SDLSRC" -B "$b" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$SDL" >/dev/null || return 1
  cmake --build "$b" -j8 >/dev/null || return 1
  cmake --install "$b" >/dev/null || return 1
  echo "SDL2 -> $SDL"
  lipo -info "$SDL/lib/libSDL2.a" 2>/dev/null | tail -1
}

# --------------------------------------------------------------------- game objects
# 🔑 IDENTICAL to build_sim.sh's CFLAGS except for the target triple and sysroot.
# Every one of these is load-bearing and is documented at length in build_sim.sh:
#   -fms-extensions            anonymous struct/union members the decomp relies on
#   -include ge_port_decls.h   prototypes; IDO allowed implicit declarations
#   -Wno-everything -Werror=return-type   NOT -w. `-w` defeats -Werror in BOTH orders
#                              (measured 2026-08-21). The accidental-$v0-return family
#                              is caught only by this flag actually taking effect.
#   -ferror-limit=0            the default 20 truncates and flatters the count
#   -fno-strict-aliasing       the decomp punts types through pointers constantly
mac_cflags() {
  CFLAGS=(
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
    echo "  ⚠️  GETV_DEBUGMENU=1 -- debug menu ENABLED. START is repurposed."
  fi
}

build_port_layer() {
  local pok=0 pfail=0
  # 🔑 The ONLY intentional divergence from build_sim.sh's PORTFLAGS: USE_GLES and
  # TVOS_SUPERSAMPLE are absent and GE_PLATFORM_MAC is present. Everything else must
  # match, or this build stops being a valid proxy for the tvOS one.
  local PORTFLAGS=(
    -target "$TARGET" -isysroot "$SDK"
    -I "$HERE/port" -I "$HERE/port/include" -I "$HERE/port/fast3d" -I "$HERE/port/src"
    -I "$SDL/include" -I "$SDL/include/SDL2"
    -DTARGET_N64 -DGE_PORT_NATIVE -D_LANGUAGE_C=1 -DRAPI_GL -DWAPI_SDL2
    -DGE_PLATFORM_MAC
    # Desktop GL on macOS is deprecated-but-present; silence only that, so a real
    # diagnostic still shows.
    -DGL_SILENCE_DEPRECATION
    -Wno-everything -Werror=return-type -ferror-limit=0 -O1
  )
  mkdir -p "$BUILD/obj"
  for f in "$HERE"/port/fast3d/*.c "$HERE"/port/src/*.c "$HERE"/port/audio/*.c; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.c}").o"
    if clang "${PORTFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"; echo "  mac port FAILED: $(basename "$f")"; fi
  done
  # The harness. Shared verbatim with the tvOS app target (getv/Sources) -- it is plain
  # C + SDL with no UIKit in it, so there is nothing to fork. It defines SDL_main();
  # ge_mac_main.c below supplies the real main().
  for f in "$HERE"/Sources/ge_tvos_main.c "$HERE"/port/mac/ge_mac_main.c; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.c}").o"
    if clang "${PORTFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"
         echo "  mac port FAILED: $(basename "$f")"; fi
  done
  echo "mac port layer: $pok built, $pfail failed"
}

# 🔑 PARALLEL, unlike build_sim.sh. That script compiles all ~823 objects SEQUENTIALLY
# (~20-45 min depending on host load, which has ranged 15 to 878 on this Mac in a day).
# Nothing in the decomp generates a shared intermediate and every object path is unique,
# so the work is embarrassingly parallel; -P is the only reason a full Mac rebuild is
# minutes instead of the better part of an hour.
# ⚠️ GETV_JOBS caps it. Four other agents build in this repo concurrently, so the default
# is 6, not nproc -- pinning all 8 cores makes everyone else's build slower.
GE_JOBS="${GETV_JOBS:-6}"

# Compile one file and report a single parseable line. Counting is done by grepping these
# rather than by incrementing a variable, because a variable incremented inside an xargs
# child is lost when that subshell exits -- which would report "0 built" on a perfect build.
#
# 🔴 THE FLAGS TRAVEL THROUGH A FILE, NOT AN EXPORTED VARIABLE. Bash cannot export an
# ARRAY -- `export CFLAGS` on an array is silently a no-op -- and flattening the array to
# a string breaks on the spaces in this repo's own path (".../Code Projects/..."), which
# is exactly the class of bug that already cost this project a day on HEADER_SEARCH_PATHS.
# One flag per line, read back with mapfile, is space-safe and array-safe.
run_batch() {
  local label="$1"; shift
  local res
  printf '%s\n' "$@" > "$BUILD/flags.txt"
  res="$(GE_FLAGFILE="$BUILD/flags.txt" GE_BUILD="$BUILD" GE_DECOMP="$DECOMP" \
         xargs -P "$GE_JOBS" -I{} bash -c '
            mapfile -t F < "$GE_FLAGFILE"
            o="$GE_BUILD/obj/$(echo "${1%.c}" | tr "/" "_").o"
            if ( cd "$GE_DECOMP" && clang "${F[@]}" -c "$1" -o "$o" 2>/dev/null ); then
              echo "OK $1"
            else
              # 🔴 Never leave the PREVIOUS object behind: a stale .o outlives the source
              # that produced it and turns a broken file into a green-looking build.
              rm -f "$o"; echo "FAIL $1"
            fi' _ {})"
  echo "$res" | grep '^FAIL ' | sed 's/^FAIL /  mac FAILED: /'
  echo "$label: $(echo "$res" | grep -c '^OK ') built, $(echo "$res" | grep -c '^FAIL ') failed"
}

cmd_lib() {
  mkdir -p "$BUILD/obj"
  local CFLAGS; mac_cflags

  (cd "$DECOMP" && { find src -name '*.c' \
             -not -path 'src/libultra/*' -not -path 'src/libultrare/*' \
             -not -name 'ge_layout_audit.c' -not -name 'ge_asset_fileview_check.c'
           # 🔴 HELD BACK ON PURPOSE -- MIRRORS build_sim.sh / build.sh EXACTLY.
           # usb.c/rmon.c/sched.c/ramrom.c/init.c/indy_* are N64 HARDWARE and dev-host
           # files: compiling them turns logging stubs into code that writes real RCP/PI
           # registers or talks to an SGI host. They used to FAIL to build, and that
           # failure was the guard. Switching -w -> -Wno-everything (needed because -w
           # defeats -Werror=return-type) also suppresses clang's default-ERROR
           # diagnostics, so all seven started compiling: the count moved 165/10 -> 172/3,
           # which was a WEAKER guard, not progress. Excluding them by name is explicit.
           # ⚠️ This list MUST stay in step with the two tvOS scripts or the Mac build
           # stops being a valid proxy for them -- which is the whole point of this target.
           find src/libultra/gu -name '*.c'; } | grep -vE '/(ramromreplay\.c|audi\.c|usb\.c|rmon\.c|sched\.c|ramrom\.c|init\.c|indy_comms\.c|indy_commands\.c)$' | sort) \
    | run_batch "mac game" "${CFLAGS[@]}"

  (cd "$DECOMP" && find assets -name '*.c' ! -name '*.inc.c' | sort) | run_batch "mac assets" "${CFLAGS[@]}"

  # ⚠️ -DNDEBUG is passed to the MIXER ONLY, exactly as the tvOS builds do. Do not widen
  # it: SUPPORT_CHECK in gfx_pc.c is an assert() and is deliberately ARMED.
  (cd "$DECOMP" && find src/libultra/audio src/libultrare/audio -name '*.c' 2>/dev/null | sort) \
    | run_batch "mac audio" "${CFLAGS[@]}" -DGE_AUDIO_MIXER -DNDEBUG \
        -I src/libultra -I src/libultrare -I "$HERE/port/audio"

  build_port_layer
}

cmd_port() {
  if [ ! -d "$BUILD/obj" ] || [ -z "$(ls -A "$BUILD/obj" 2>/dev/null)" ]; then
    echo "no objects in $BUILD/obj -- run './build_mac.sh lib' once first"; return 1
  fi
  build_port_layer
}

# --------------------------------------------------------------------------- link
cmd_app() {
  [ -d "$BUILD/obj" ] || { echo "nothing built -- run './build_mac.sh lib'"; return 1; }

  # 🔴 ARCHIVE, NOT A PILE OF OBJECTS -- and this is the whole point, not a packaging
  # preference. MEASURED: linking build-mac/obj/*.o directly fails with ~30 undefined
  # symbols (`__codeSegmentRomStart`, `___osGetFpcCsr`, `_crashRenderFrame`,
  # `__libm_qnan_f`, ...). Every one of them is an N64 LINKER-SCRIPT or hardware symbol
  # referenced only by src/init.c, src/sched.c and src/rmon.c -- files this port compiles
  # but DELIBERATELY NEVER LINKS. A static archive pulls a member only when it resolves
  # an undefined symbol, so those three objects are simply never dragged in; a direct
  # object link has no such filter and drags in everything.
  #
  # This is exactly the arrangement build.sh/build_sim.sh use, and the reason project.yml
  # says "No -force_load yet: the harness deliberately pulls in only what it calls."
  # 🔴 If you ever add -force_load here, note that assets/obseg/bg/{u,e,j}/ contain
  # COLLIDING BARE SYMBOLS (`header`, `room_data_table`) which are harmless today only
  # because those objects are never pulled.
  #
  # The two harness objects stay OUTSIDE the archive: they carry main()/SDL_main() and
  # are the roots the whole link is discovered from.
  local -a roots=("$BUILD/obj/port_ge_tvos_main.o" "$BUILD/obj/port_ge_mac_main.o")
  local f
  for f in "${roots[@]}"; do
    [ -e "$f" ] || { echo "missing harness object $f -- run './build_mac.sh port'"; return 1; }
  done

  rm -f "$BUILD/libge.a"
  local -a members=()
  for f in "$BUILD"/obj/*.o; do
    case "$f" in
      *"/port_ge_tvos_main.o"|*"/port_ge_mac_main.o") continue ;;
    esac
    members+=("$f")
  done
  ar rcs "$BUILD/libge.a" "${members[@]}" || return 1
  echo "mac libge.a: $(du -h "$BUILD/libge.a" | cut -f1), ${#members[@]} members"

  # 🔑 -lc++ is MANDATORY: Fast3D is C++ and every source here is C, so clang links
  # without the C++ runtime and the link dies on C++ ABI symbols.
  # ⚠️ "built for newer macOS version" warnings come from SDL2 having been compiled with
  # the SDK's default deployment target rather than ours. They are advisory -- the code
  # is arm64 either way -- so they are filtered rather than chased.
  # 🔴 -dead_strip IS LOAD-BEARING, NOT AN OPTIMISATION. MEASURED: without it the link
  # fails on six undefined symbols -- `osEepromRead`/`osEepromWrite` (joy.c's GamePak
  # save path), `osViSetMode` (fr.c's viVsyncRelated, deliberately not carried over),
  # `osPiReadIo` (token.c) and the `_{e,j}fontchardataSegmentRomStart` linker-script
  # symbols (language.c's Japanese font). Every one is referenced ONLY from a function
  # this port never calls.
  # ld64 dead-strips BEFORE it checks for undefined symbols, so a reference from a dead
  # atom is not an error. Xcode's Release configuration turns DEAD_CODE_STRIPPING on by
  # default, which is exactly why the tvOS build has never seen these -- and the sim
  # binary's dSYM confirms it: `_joyPoll` is present, `_joyGamePakRead` is not.
  # ⚠️ Removing this flag does not "link more of the game"; it just breaks the build.
  clang -target "$TARGET" -isysroot "$SDK" -o "$BIN" \
    -dead_strip \
    "${roots[@]}" "$BUILD/libge.a" \
    "$SDL/lib/libSDL2.a" \
    -lc++ -lz -liconv \
    -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo \
    -framework CoreAudio -framework AudioToolbox -framework AVFoundation \
    -framework Carbon -framework ForceFeedback -framework Metal -framework QuartzCore \
    -framework CoreFoundation -framework CoreServices -framework CoreHaptics \
    -framework GameController -framework CoreMedia -framework UniformTypeIdentifiers \
    2>&1 | grep -v "was built for newer" | head -30
  if [ -x "$BIN" ]; then
    echo "mac binary: $BIN ($(du -h "$BIN" | cut -f1), $(lipo -archs "$BIN"))"
  else
    echo "LINK FAILED"; return 1
  fi
}

cmd_run() {
  [ -x "$BIN" ] || { echo "no binary -- run './build_mac.sh all'"; return 1; }
  # Nothing to forward: unlike simctl, a native process just inherits the environment,
  # so every GETV_* knob works exactly as exported.
  "$BIN" "$@"
}

case "${1:-}" in
  sdl)  cmd_sdl ;;
  lib)  cmd_lib ;;
  port) cmd_port ;;
  app)  cmd_app ;;
  all)  cmd_lib && cmd_app ;;
  run)  shift; cmd_run "$@" ;;
  env)  echo "SDK=$SDK"; echo "SDL=$SDL"; echo "TARGET=$TARGET"; echo "BUILD=$BUILD"
        echo "BIN=$BIN" ;;
  *)    echo "usage: $0 {sdl|lib|port|app|all|run|env}"
        echo "  sdl  = build SDL2 2.30.9 arm64 from deps/ into $SDL (once)"
        echo "  lib  = compile game + assets + audio + port layer for arm64 macOS"
        echo "  port = recompile getv/port/** and the harness only (seconds)"
        echo "  app  = link $BIN"
        echo "  run  = launch it" ;;
esac
