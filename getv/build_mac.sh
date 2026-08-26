#!/usr/bin/env bash
# Build and run GoldenEye natively on macOS (arm64), on the EXISTING Fast3D + GL path.
#
# Why this target exists
# ----------------------
# tvOS cannot be observed directly. The OS blocks devicectl from reading the app
# container, the device cannot be screenshotted, and the tvOS simulator has no
# GPU-backed GLES at all (GL_RENDERER = "Apple Software Renderer", 600 ms - 2.4 s per
# frame), so visual claims made from it rest on software-rendered frames plus counter
# instrumentation. A Mac window on a real GPU gives real frame rates, real screenshots,
# a real crash report, and a build that can be played.
#
# This script is additive. It does not read, write or source build.sh or build_sim.sh.
# tvOS is on hold, not abandoned: `./build.sh` (device) and `./build_sim.sh` (simulator)
# must keep working, and this file exists precisely so they are never edited for Mac.
#
# No Metal backend. This uses the same renderer the tvOS build uses, so a regression
# seen here is a regression there. Only the platform bindings differ:
#
#            tvOS device / sim            macOS
#   GL       OpenGL ES 3.0 (EAGL)         OpenGL 2.1 compat (NSOpenGL)
#   defines  -DUSE_GLES -DTVOS_SUPERSAMPLE  (neither) -DGE_PLATFORM_MAC
#   shaders  #version 100 ESSL            #version 120 GLSL   (already in gfx_opengl.c)
#   FBO      SDL's own FBO, blit-resolved framebuffer 0, no supersample
#   entry    libSDL2main -> UIApplicationMain -> SDL_main   main() -> SDL_main
#   window   full-screen UIWindow          resizable NSWindow
#
# macOS's legacy GL profile is intentional: gfx_opengl.c's shader generator emits
# `#version 120` with attribute/varying, which a 3.2 core profile rejects outright. Do
# not "modernise" the context request.
#
# Supersampling is off here (TVOS_SUPERSAMPLE undefined), so gfx_supersample stays at
# its declared default of 1 and Fast3D renders straight to framebuffer 0. Any number
# measured here is therefore an ss=1 number; do not diff it against a tvOS ss=2 run.
#
# SDL2: this Mac's Homebrew is x86_64 under Rosetta, so brew's SDL2 is Intel-only and
# cannot be linked into an arm64 binary. `./build_mac.sh sdl` builds SDL2 2.30.9 from the
# source already in deps/ for arm64-apple-macos and installs it outside the repo, into a
# space-free path. The repo lives under ".../Code Projects/...", and a space in a header
# search path breaks the build in ways that are hard to trace.
#
# usage: ./build_mac.sh {sdl|lib|port|app|all|run|env}
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$HERE/../vendor/ge-decomp"
BUILD="$HERE/build-mac"
SDL="$HOME/.n64tvos/sdl2-mac"
# Optional Lua mod scripting. Built by tools/fetch_lua.sh into the same out-of-repo prefix
# SDL2 uses, because deps/ is not tracked. Absent is the normal case: without it the build
# omits -DGE_WITH_LUA and ge_lua.c compiles to empty hook functions, so nothing else in the
# tree needs to know whether scripting is present.
LUA="$HOME/.n64tvos/lua-mac"
LUAFLAGS=(); LUALIBS=()
if [ -f "$LUA/lib/liblua.a" ] && [ -f "$LUA/include/lua.h" ]; then
  LUAFLAGS=( -DGE_WITH_LUA -I "$LUA/include" )
  LUALIBS=( "$LUA/lib/liblua.a" )
fi
# Optional Dear ImGui dev overlay / launcher UI. Built by tools/fetch_imgui.sh into the same
# out-of-repo prefix, on the same terms as Lua and SDL2. Absent is the normal case: without
# it the build omits -DGE_WITH_IMGUI and getv/port/src/ge_imgui.cpp compiles to empty entry
# points, so gfx_sdl2.c needs no #ifdef and nothing else in the tree knows.
#
# The overlay is additionally OFF at runtime unless GETV_IMGUI=1, so having the library
# installed does not change how the game behaves.
IMGUI="$HOME/.n64tvos/imgui-mac"
IMGUIFLAGS=(); IMGUILIBS=()
if [ -f "$IMGUI/lib/libimgui.a" ] && [ -f "$IMGUI/include/imgui.h" ]; then
  IMGUIFLAGS=( -DGE_WITH_IMGUI -I "$IMGUI/include" )
  IMGUILIBS=( "$IMGUI/lib/libimgui.a" )
fi
SDLSRC="$HERE/../deps/SDL2-2.30.9"
SDK="$(xcrun -sdk macosx --show-sdk-path)"
# Host-arch detection: the game/renderer layer has no arm64-specific code (see
# docs/PORTING.md), so x86_64 Macs build too. The only real arch dependency is the
# crash handler's register dump in Sources/ge_tvos_main.c, which branches on it.
# uname -m answers for the *process*, not the machine. An x86_64 Homebrew bash under
# Rosetta reports x86_64 on an Apple silicon Mac, and the build then targets the Intel
# slice, ignores an arm64-only libSDL2.a with ~30 "required architecture" warnings, and
# either fails to link or silently leaves the previous binary in place. hw.optional.arm64
# is a property of the hardware and is not rewritten by Rosetta, so it is asked first;
# uname remains the fallback for real Intel Macs, where the sysctl does not exist.
# Set MACARCH in the environment to override (cross-building the other slice).
HOSTARCH="$(uname -m)"
if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = "1" ]; then
  HOSTARCH="arm64"
fi
case "${MACARCH:-$HOSTARCH}" in
  arm64)   MACARCH="arm64" ;;
  x86_64)  MACARCH="x86_64" ;;
  *) echo "unsupported host arch: ${MACARCH:-$HOSTARCH}" >&2; exit 1 ;;
esac
TARGET="${MACARCH}-apple-macos13.0"
BIN="$BUILD/goldeneye"

# --------------------------------------------------------------------------- SDL2
cmd_sdl() {
  [ -d "$SDLSRC" ] || { echo "no SDL2 source at $SDLSRC"; return 1; }
  local b="$HOME/.n64tvos/build-sdl2-mac"
  # CMAKE_POLICY_VERSION_MINIMUM: SDL 2.30.9 declares cmake_minimum_required(3.0),
  # which CMake 4 refuses outright. This is the sanctioned opt-back-in.
  cmake -S "$SDLSRC" -B "$b" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_OSX_ARCHITECTURES="$MACARCH" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$SDL" >/dev/null || return 1
  cmake --build "$b" -j8 >/dev/null || return 1
  cmake --install "$b" >/dev/null || return 1
  echo "SDL2 -> $SDL"
  lipo -info "$SDL/lib/libSDL2.a" 2>/dev/null | tail -1
}

# --------------------------------------------------------------------- game objects
# Identical to build_sim.sh's CFLAGS except for the target triple and sysroot. Every
# one of these matters and is documented at length in build_sim.sh:
#   -fms-extensions            anonymous struct/union members the decomp relies on
#   -include ge_port_decls.h   prototypes; IDO allowed implicit declarations
#   -Wno-everything -Werror=return-type   not -w. `-w` defeats -Werror in both orders,
#                              and the accidental-$v0-return family is caught only when
#                              this flag actually takes effect.
#   -ferror-limit=0            the default 20 truncates and flatters the count
#   -fno-strict-aliasing       the decomp punts types through pointers constantly
# The port layer's third-party sources (Fast3D, the audio mixer) are not vendored in this
# repository -- see docs/THIRD_PARTY.md. Without them the compile fails with a long list
# of missing headers and nothing that points at the cause, so check once and say so.
require_decomp() {
  if [ ! -d "$DECOMP/src" ]; then
    echo "error: the decompiled game source is not present." >&2
    echo "       expected: $( cd "$HERE/.." >/dev/null 2>&1 && pwd )/vendor/ge-decomp" >&2
    echo "" >&2
    echo "       This repository does not include the decompilation. Clone it into" >&2
    echo "       vendor/ge-decomp, then run the asset pipeline. See docs/SETUP.md." >&2
    exit 1
  fi
  # A decomp that is present but has never had its assets generated fails later as
  # hundreds of missing includes; name it here instead.
  if [ ! -d "$DECOMP/assets" ]; then
    echo "error: $DECOMP exists but has no assets/ directory." >&2
    echo "       The asset pipeline has not been run. See docs/SETUP.md section 3." >&2
    exit 1
  fi
}

require_thirdparty() {
  # Check every file the fetch is responsible for, not just one. A partial or
  # interrupted fetch otherwise passes this gate and fails later as a confusing
  # pile of missing-header errors.
  local missing=0 f
  for f in \
    port/fast3d/gfx_cc.c port/fast3d/gfx_cc.h \
    port/fast3d/gfx_opengl.c port/fast3d/gfx_opengl.h \
    port/fast3d/gfx_pc.c port/fast3d/gfx_pc.h \
    port/fast3d/gfx_rendering_api.h port/fast3d/gfx_screen_config.h \
    port/fast3d/gfx_sdl.h port/fast3d/gfx_sdl2.c \
    port/fast3d/gfx_window_manager_api.h \
    port/audio/ge_mixer.c port/audio/ge_mixer.h \
    port/configfile.h port/fs/fs.h
  do
    [ -f "$HERE/$f" ] || { echo "  missing: $f" >&2; missing=$((missing+1)); }
  done
  if [ "$missing" -gt 0 ]; then
    echo "error: $missing third-party port source(s) missing." >&2
    echo "       run tools/fetch-thirdparty.sh fetch   (see docs/THIRD_PARTY.md)" >&2
    exit 1
  fi
}

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
    echo "  GETV_DEBUGMENU=1 -- debug menu ENABLED. START is repurposed."
  fi
}

build_port_layer() {
  require_thirdparty
  local pok=0 pfail=0
  # The only intentional divergence from build_sim.sh's PORTFLAGS: USE_GLES and
  # TVOS_SUPERSAMPLE are absent and GE_PLATFORM_MAC is present. Everything else must
  # match, or this build stops being a valid proxy for the tvOS one.
  local PORTFLAGS=(
    -target "$TARGET" -isysroot "$SDK"
    -I "$HERE/port" -I "$HERE/port/include" -I "$HERE/port/fast3d" -I "$HERE/port/src"
    -I "$SDL/include" -I "$SDL/include/SDL2"
    -DTARGET_N64 -DGE_PORT_NATIVE -D_LANGUAGE_C=1 -DRAPI_GL -DWAPI_SDL2
    -DGE_PLATFORM_MAC
    -DGE_PLATFORM_DESKTOP
    # Desktop GL on macOS is deprecated-but-present; silence only that, so a real
    # diagnostic still shows.
    -DGL_SILENCE_DEPRECATION
    -Wno-everything -Werror=return-type -ferror-limit=0 -O1
    ${LUAFLAGS[@]+"${LUAFLAGS[@]}"}
    ${IMGUIFLAGS[@]+"${IMGUIFLAGS[@]}"}
  )
  mkdir -p "$BUILD/obj"
  for f in "$HERE"/port/fast3d/*.c "$HERE"/port/src/*.c "$HERE"/port/audio/*.c; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.c}").o"
    if clang "${PORTFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"; echo "  mac port FAILED: $(basename "$f")"; fi
  done
  # C++ in the port layer. Exactly one file today (ge_imgui.cpp) and that is the intent:
  # ImGui is C++, everything that calls it is C, and the boundary is kept to one translation
  # unit with a plain-C header. The flags are PORTFLAGS with -std=c++17 added and nothing
  # removed, so a define that reaches the C files reaches this one too -- an -I or a -D that
  # applied to only half the port layer is a bug waiting to happen.
  #
  # This loop runs whether or not ImGui is installed. Without -DGE_WITH_IMGUI the file
  # compiles to empty entry points, which is what lets gfx_sdl2.c call them unconditionally.
  for f in "$HERE"/port/src/*.cpp; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.cpp}").o"
    if clang++ "${PORTFLAGS[@]}" -std=c++17 -fno-exceptions -fno-rtti -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
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

# Parallel, unlike build_sim.sh, which compiles all ~823 objects sequentially (~20-45 min
# depending on host load). Nothing in the decomp generates a shared intermediate and every
# object path is unique, so the work is embarrassingly parallel; -P is the only reason a
# full Mac rebuild is minutes instead of the better part of an hour.
# GETV_JOBS caps it. The default is 6 rather than nproc so a build does not monopolise the
# machine.
GE_JOBS="${GETV_JOBS:-6}"

# Compile one file and report a single parseable line. Counting is done by grepping these
# rather than by incrementing a variable, because a variable incremented inside an xargs
# child is lost when that subshell exits -- which would report "0 built" on a perfect build.
#
# The flags travel through a file, not an exported variable. Bash cannot export an array
# (`export CFLAGS` on an array is silently a no-op), and flattening the array to a string
# breaks on the spaces in this repo's own path (".../Code Projects/..."). One flag per
# line, read back into an array, is space-safe and array-safe.
#
# The read loop is deliberately not `mapfile`. macOS ships bash 3.2 as /bin/bash and
# mapfile arrived in bash 4, so on a stock machine every compile in the batch would fail
# and the build would report "0 built" with no other diagnostic. `while IFS= read -r`
# is portable to 3.2 and behaves identically here.
run_batch() {
  local label="$1"; shift
  local res
  printf '%s\n' "$@" > "$BUILD/flags.txt"
  res="$(GE_FLAGFILE="$BUILD/flags.txt" GE_BUILD="$BUILD" GE_DECOMP="$DECOMP" \
         xargs -P "$GE_JOBS" -I{} bash -c '
            F=(); while IFS= read -r _l; do F+=("$_l"); done < "$GE_FLAGFILE"
            o="$GE_BUILD/obj/$(echo "${1%.c}" | tr "/" "_").o"
            if ( cd "$GE_DECOMP" && clang "${F[@]}" -c "$1" -o "$o" 2>/dev/null ); then
              echo "OK $1"
            else
              # Never leave the previous object behind: a stale .o outlives the source
              # that produced it and turns a broken file into a green-looking build.
              rm -f "$o"; echo "FAIL $1"
            fi' _ {})"
  echo "$res" | grep '^FAIL ' | sed 's/^FAIL /  mac FAILED: /'
  echo "$label: $(echo "$res" | grep -c '^OK ') built, $(echo "$res" | grep -c '^FAIL ') failed"
  # Record what this batch is entitled to put in the archive. cmd_app archives every .o it
  # finds on disk, so an object whose source has left the build -- excluded by name, deleted,
  # renamed -- would otherwise be linked forever. That is not hypothetical: excluding
  # assets/obseg/setup/{e,j} left sixteen PAL and Japanese setup objects in the archive, and
  # the binary went on resolving seven levels to PAL data through them.
  echo "$res" | sed -n 's|^OK ||p' \
    | sed -e 's|\.c$||' -e 'y|/|_|' -e "s|^|$BUILD/obj/|" -e 's|$|.o|' >> "$BUILD/objects.txt"
}

cmd_lib() {
  require_decomp
  require_thirdparty
  mkdir -p "$BUILD/obj"
  # Fresh manifest per full compile; run_batch appends to it.
  : > "$BUILD/objects.txt"
  local CFLAGS; mac_cflags

  (cd "$DECOMP" && { find src -name '*.c' \
             -not -path 'src/libultra/*' -not -path 'src/libultrare/*' \
             -not -name 'ge_layout_audit.c' -not -name 'ge_asset_fileview_check.c'
           # Held back on purpose; mirrors build_sim.sh and build.sh exactly.
           # usb.c/rmon.c/sched.c/ramrom.c/init.c/indy_* are N64 hardware and dev-host
           # files: compiling them turns logging stubs into code that writes real RCP/PI
           # registers or talks to an SGI host. They used to fail to build, and that
           # failure was the guard. Switching -w -> -Wno-everything (needed because -w
           # defeats -Werror=return-type) also suppresses clang's default-error
           # diagnostics, so all seven started compiling and the count moved 165/10 ->
           # 172/3 -- a weaker guard, not progress. Excluding them by name is explicit.
           # tlb_manage.c manages the N64's TLB, which a native build does not have. It has never
           # compiled here -- three type conflicts against the native headers -- so it was already
           # absent from the binary, and excluding it by name only stops the build reporting a
           # failure on every run. A build that always fails once teaches people to ignore failures.
           # This list must stay in step with the two tvOS scripts or the Mac build stops
           # being a valid proxy for them, which is the whole point of this target.
           find src/libultra/gu -name '*.c'; } | grep -vE '/(ramromreplay\.c|audi\.c|usb\.c|rmon\.c|sched\.c|ramrom\.c|init\.c|indy_comms\.c|indy_commands\.c|tlb_manage\.c)$' | sort) \
    | run_batch "mac game" "${CFLAGS[@]}"

  # setup/e and setup/j are the PAL and Japanese setup tables. They hold the same eight
  # filenames as setup/u, and uniquify_asset_symbols.py namespaces by file stem, so seven of
  # the eight end up defining identical globals in all three directories -- UsetupcradZ_padlist
  # And so on. Compiling all three lets the linker bind Cradle, Silo, destruction, Jungle,
  # Train, Statue and the multiplayer Archives to whichever copy it saw first, which is
  # alphabetically e/, the PAL data, in a VERSION_US build. The same applies to the top-level
  # `stagesetup UsetuplenZ`, which the engine looks up by name and so is deliberately left
  # unprefixed. Nothing outside those two directories references their symbols and
  # file_resource_table.inc.c asks for the bare name, so a US build simply must not compile
  # them. Namespacing them instead would keep seven dead translation units in the binary.
  (cd "$DECOMP" && find assets -name '*.c' ! -name '*.inc.c' \
      ! -path 'assets/obseg/setup/e/*' ! -path 'assets/obseg/setup/j/*' | sort) \
    | run_batch "mac assets" "${CFLAGS[@]}"

  # -DNDEBUG is passed to the mixer only, exactly as the tvOS builds do. Do not widen it:
  # SUPPORT_CHECK in gfx_pc.c is an assert() and is deliberately armed.
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

  # An archive, not a pile of objects, and that is a correctness requirement rather than
  # a packaging preference. Linking build-mac/obj/*.o directly fails with ~30 undefined
  # symbols (`__codeSegmentRomStart`, `___osGetFpcCsr`, `_crashRenderFrame`,
  # `__libm_qnan_f`, ...). Every one of them is an N64 linker-script or hardware symbol
  # referenced only by src/init.c, src/sched.c and src/rmon.c -- files this port compiles
  # but deliberately never links. A static archive pulls a member only when it resolves
  # an undefined symbol, so those three objects are never dragged in; a direct object
  # link has no such filter and drags in everything.
  #
  # This is the arrangement build.sh and build_sim.sh use, and the reason project.yml
  # says "No -force_load yet: the harness deliberately pulls in only what it calls."
  # Before adding -force_load here, note that assets/obseg/bg/{u,e,j}/ contain colliding
  # bare symbols (`header`, `room_data_table`) which are harmless today only because
  # those objects are never pulled.
  #
  # The two harness objects stay outside the archive: they carry main()/SDL_main() and
  # are the roots the whole link is discovered from.
  local -a roots=("$BUILD/obj/port_ge_tvos_main.o" "$BUILD/obj/port_ge_mac_main.o")
  local f
  for f in "${roots[@]}"; do
    [ -e "$f" ] || { echo "missing harness object $f -- run './build_mac.sh port'"; return 1; }
  done

  rm -f "$BUILD/libge.a"
  # Choose members rather than sweeping the directory. Objects accumulate: one whose source
  # has left the build -- excluded by name, deleted, renamed -- stays on disk and would keep
  # satisfying the link forever. That is not hypothetical. Excluding assets/obseg/setup/{e,j}
  # left sixteen PAL and Japanese setup objects behind, and the binary went on resolving
  # seven levels to PAL data through them while reporting the correct new counts.
  #
  # run_batch records what it compiled in objects.txt. The port layer is built outside it and
  # is recognised by its port_ prefix. Anything else on disk is an orphan and is left out of
  # the archive -- not deleted, because a wrong exclusion here should cost a rebuild, not
  # someone's build directory.
  local -a members=()
  local f orphans=0 manifest="$BUILD/objects.txt"
  if [ -s "$manifest" ]; then
    # Match in one pass. The obvious per-file form,
    #     printf '%s\n' "$keep" | grep -qxF "$f"
    # is wrong under the `set -o pipefail` above: grep -q exits at the first hit, printf
    # dies of SIGPIPE, and the pipeline reports 141 -- a failure -- for exactly the files
    # that DID match. It silently dropped about a third of the archive.
    local disklist="$BUILD/.objects.disk" keeplist="$BUILD/.objects.keep"
    : > "$disklist"
    for f in "$BUILD"/obj/*.o; do
      case "$f" in
        *"/port_ge_tvos_main.o"|*"/port_ge_mac_main.o") continue ;;
        *"/port_"*) members+=("$f"); continue ;;
      esac
      printf '%s\n' "$f" >> "$disklist"
    done
    sort -u "$manifest" > "$keeplist"
    local total matched=0
    total=$(wc -l < "$disklist" | tr -d ' ')
    while IFS= read -r f; do
      members+=("$f"); matched=$((matched + 1))
    done < <(grep -xFf "$keeplist" "$disklist" || true)
    orphans=$((total - matched))
    rm -f "$disklist" "$keeplist"
    if [ "$orphans" -gt 0 ]; then
      echo "mac excluded $orphans orphaned object(s) whose source left the build"
    fi
  else
    echo "mac note: no object manifest; archiving every object found -- run './build_mac.sh lib'"
    for f in "$BUILD"/obj/*.o; do
      case "$f" in
        *"/port_ge_tvos_main.o"|*"/port_ge_mac_main.o") continue ;;
      esac
      members+=("$f")
    done
  fi
  ar rcs "$BUILD/libge.a" "${members[@]}" || return 1
  echo "mac libge.a: $(du -h "$BUILD/libge.a" | cut -f1), ${#members[@]} members"

  # -lc++ is mandatory: Fast3D is C++ and every source here is C, so clang links without
  # the C++ runtime and the link dies on C++ ABI symbols.
  # "built for newer macOS version" warnings come from SDL2 having been compiled with the
  # SDK's default deployment target rather than ours. They are advisory -- the code is
  # arm64 either way -- so they are filtered rather than chased.
  # -dead_strip is REMOVED. It was documented here as a pure size optimisation; it
  # wasn't. The obseg asset generator (chr/gun/prop/stan) emits each ROM-derived record
  # as its own named global, and the decompiled game walks between them with raw
  # `tile + stride` pointer arithmetic, referencing everything past the first record in
  # a chain only by computed offset. ld64 saw no name reference to `_tile_1..N` and
  # discarded them, which broke room/collision resolution on every level -- see
  # github.com/SegfaultEvan/goldeneye-native/issues/2 for the measurements. Not provably
  # x86_64-specific (this is a linker/dead-stripping interaction, not architecture code),
  # but arm64 builds happen to keep most chains intact.
  #
  # The four N64-hardware symbols this flag used to incidentally hide are unaffected:
  # port/src/port_n64_unused.c defines all four for real, so the link stays clean
  # without -dead_strip too.
  clang -target "$TARGET" -isysroot "$SDK" -o "$BIN" \
    "${roots[@]}" "$BUILD/libge.a" \
    "$SDL/lib/libSDL2.a" ${LUALIBS[@]+"${LUALIBS[@]}"} ${IMGUILIBS[@]+"${IMGUILIBS[@]}"} \
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
