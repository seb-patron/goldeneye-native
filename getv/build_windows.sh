#!/usr/bin/env bash
# Build and run GoldenEye natively on Linux (x86_64 or arm64), on the EXISTING Fast3D + GL
# path. Modelled line for line on build_mac.sh; read that file first if anything here looks
# arbitrary, because most of it is explained there and not repeated.
#
# ############################################################################
# # Untested. NO linux machine has ever run this script OR compiled this tree.
# ############################################################################
#
# It was written on macOS from docs/PORTING.md sections 9 and 11 and checked only with
# `bash -n`. Nothing below is a claim that it works. Four things need checking before
# anything else, in this order, because each one will stop the build outright:
#
#   1. getv/Sources/ge_tvos_main.c:88-107 -- the crash handler reads the faulting PC out of
#      a Darwin arm64 ucontext (`_STRUCT_ARM_THREAD_STATE64`, `uc->uc_mcontext->__ss`,
#      `__darwin_arm_thread_state64_get_pc`). Those identifiers do not exist on glibc, and
#      The block is not behind any #ifdef. the harness will not compile until this IS
#      BRANCHED. PORTING.md section 6 sizes it at half a day: backtrace(), dladdr() and
#      sigaction() are all present on glibc, so only the register dump needs the branch
#      (`uc->uc_mcontext.gregs[REG_RIP]` on x86_64, `uc->uc_mcontext.pc` on aarch64).
#      That file is shared verbatim with the tvOS targets, so whoever branches it must not
#      regress them.
#
#   2. vendor/ge-decomp/src/libultra/gu/{sinf,cosf}.c:33-34 -- `#pragma weak sinf = __sinf`
#      and the cosf pair. Mach-O has no weak aliases so clang warns and ignores them; ELF
#      honours them, which is exactly what they ask for. That means libge.a would carry
#      weak global `sinf`, `cosf`, `fsin` and `fcos` competing with libm's. Both files
#      #define fsin/fcos on the very next line, so the pragmas are already redundant for
#      this port; PORTING.md section 7 says to wrap all four in #ifndef GE_PORT_NATIVE.
#      This is the one item where "ELF is closer to what the decomp expected" is a hazard
#      rather than a convenience.
#
#   3. SDL2 discovery. This script asks pkg-config, then sdl2-config, then a prefix built
#      by `./build_windows.sh sdl`. Whether the distro's SDL2 development package is present
#      is the single most likely reason a first run stops before compiling anything.
#
#   4. The compiler. `-Wno-everything` is clang-only and it is not cosmetic -- see
#      warn_flags() below. Under gcc the warning set genuinely differs. Prefer clang for
#      the first build so the only variable is the platform.
#
# The remaining Linux-specific items in PORTING.md section 11 are already satisfied in
# tree and need nothing from this script: getv/port/include/{PR,platform_info.h} are
# relative symlinks; port_paths.c falls through to SDL_GetPrefPath, which is the correct
# XDG answer on Linux; the four link-order-dependent symbols are defined for real in
# getv/port/src/port_n64_unused.c.
#
# This script is additive. It does not read, write or source build.sh, build_sim.sh or
# build_mac.sh. All three of those must keep working; this file exists precisely so they
# are never edited for Linux.
#
# No Metal, no D3D, no GLES. Same renderer the tvOS and Mac builds use, so a regression
# seen here is a regression there. Only the platform bindings differ:
#
#            macOS                        Linux
#   GL       OpenGL 2.1 compat (NSOpenGL)  OpenGL 2.1 compat (GLX/EGL via SDL2)
#   loader   OpenGL.framework exports all  libGL exports all; GL_GLEXT_PROTOTYPES
#   defines  -DGE_PLATFORM_MAC + DESKTOP   -DGE_PLATFORM_DESKTOP only
#   link     -framework OpenGL/Cocoa/...   -lGL -lm -ldl -lpthread + SDL2's own libs
#
# GE_PLATFORM_DESKTOP is the necessary define, not GE_PLATFORM_MAC. It carries the
# keyboard-as-port-0 device (port_input.c) and the resizable 1280x960 window
# (port_support.c, ge_tvos_main.c). Without it this build would come up at a fixed
# 1920x1080 with no keyboard, which is the tvOS arrangement. GE_PLATFORM_MAC is now only
# the macOS user-data directory and one diagnostic string, and defining it here would
# write saves to a $HOME/Library path that does not belong on this platform.
#
# GLEW is deliberately not used. gfx_opengl.c pulls it in only under __MINGW32__ or
# OSX_BUILD, and defines GL_GLEXT_PROTOTYPES otherwise, which is the right answer against
# a normal Linux libGL. Do not add -DOSX_BUILD to "get the extensions".
#
# Supersampling is off here (TVOS_SUPERSAMPLE undefined), so any number measured here is
# an ss=1 number; do not diff it against a tvOS ss=2 run.
#
# usage: ./build_windows.sh {sdl|lib|port|app|all|run|env}
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$HERE/../vendor/ge-decomp"
BUILD="$HERE/build-windows"
SDL="${GETV_SDL_PREFIX:-$HOME/.n64tvos/sdl2-windows}"
SDLSRC="$HERE/../deps/SDL2-2.30.9"

# Optional Lua (mod scripting) and Dear ImGui (dev overlay + launcher), both built by their
# fetch scripts into out-of-repo prefixes because deps/ is not tracked. Absent is the normal
# case and must stay buildable: without the library the corresponding -DGE_WITH_* is omitted
# and ge_lua.c / ge_imgui.cpp / ge_launcher.cpp compile to empty entry points, so no call
# site anywhere needs an #ifdef. The prefixes are per-OS (-linux, not -mac) because these are
# native static libraries and the two hosts cannot share them.
# MinGW-w64 from MSYS2. Named absolutely rather than trusted to PATH: this script is
# normally invoked over SSH from another machine, where the interactive PATH that puts
# /mingw64/bin first is not in effect, and picking up the MSYS (not MinGW) gcc silently
# produces a binary that needs msys-2.0.dll and is not a native Windows build.
MINGW="${GETV_MINGW:-/mingw64}"
export PATH="$MINGW/bin:$PATH"
CC="${CC:-$MINGW/bin/gcc.exe}"

LUA="${GETV_LUA_PREFIX:-$HOME/.n64tvos/lua-win}"
LUAFLAGS=(); LUALIBS=()
if [ -f "$LUA/lib/liblua.a" ] && [ -f "$LUA/include/lua.h" ]; then
  LUAFLAGS=( -DGE_WITH_LUA -I "$LUA/include" )
  LUALIBS=( "$LUA/lib/liblua.a" )
fi

IMGUI="${GETV_IMGUI_PREFIX:-$HOME/.n64tvos/imgui-win}"
IMGUIFLAGS=(); IMGUILIBS=()
if [ -f "$IMGUI/lib/libimgui.a" ] && [ -f "$IMGUI/include/imgui.h" ]; then
  IMGUIFLAGS=( -DGE_WITH_IMGUI -I "$IMGUI/include" )
  IMGUILIBS=( "$IMGUI/lib/libimgui.a" )
fi

# The C++ compiler, needed only for the two .cpp files in the port layer. Derived from $CC so
# that CC=clang gets clang++ rather than silently mixing toolchains, which on Linux means
# mixing libstdc++ and libc++ and failing at link with unresolved std:: symbols.
if [ -n "${CXX:-}" ]; then :
elif [ "${CC:-cc}" = "clang" ]; then CXX=clang++
elif [ "${CC:-cc}" = "gcc" ];   then CXX=g++
else CXX=c++
fi
BIN="$BUILD/goldeneye"

# clang by default. gcc is expected to work and is not the recommended first attempt:
# see warn_flags(). Override with CC=gcc.
CC="${CC:-clang}"
command -v "$CC" >/dev/null 2>&1 || { echo "error: compiler '$CC' not found (set CC=)"; exit 1; }
case "$("$CC" --version 2>/dev/null | head -1)" in
  *clang*) CC_KIND=clang ;;
  *)       CC_KIND=gcc ;;
esac

# The warning contract, which is a correctness requirement rather than tidiness. The
# accidental-$v0-return family -- a non-void function falling off the end, which worked on
# MIPS/IDO because the last callee's result was already in $v0 -- is caught by
# -Werror=return-type and by nothing else, and plain -w defeats that flag in both orders
# on clang. build_mac.sh therefore uses -Wno-everything, which silences everything except
# the -Werror= flags that follow it.
#
# gcc has no -Wno-everything. The substitute is to pass no blanket suppression at all:
# without -Wall gcc is already close to quiet, and -Werror=return-type then actually
# fires. The cost is a noisier build, not a weaker guard. Passing -w under gcc instead
# would reproduce exactly the no-op guard this project already got caught by once.
#
# -ferror-limit=0 is clang-only. gcc's default is already unlimited, so there is nothing
# to pass there; the mac script's note about the default 20 truncating and flattering the
# count applies to clang only.
warn_flags() {
  if [ "$CC_KIND" = clang ]; then
    printf '%s\n' -Wno-everything -Werror=return-type -ferror-limit=0
  else
    printf '%s\n' -Werror=return-type
  fi
}

# SDL2 discovery, in preference order. The mac script hand-builds SDL2 because that Mac's
# Homebrew is x86_64 under Rosetta and cannot produce an arm64 library; on Linux the
# distro package is normally correct, so the hand-built prefix is the fallback rather than
# the rule.
#
# Both the prefix include dir and its SDL2/ subdirectory go on the search path, because
# the port includes SDL as <SDL.h> (gfx_sdl2.c, ge_tvos_main.c) while gfx_opengl.c:26 asks
# for <SDL2/SDL.h>. pkg-config only reports the latter.
#
# --static is used only for the locally built prefix, which `sdl` installs as a static
# archive. A distro SDL2 is shared and its plain --libs is the correct answer; asking for
# --static there drags in a long list of X11/wayland/pulse libraries that may not have
# development packages installed.
SDL_CFLAGS=()
SDL_LIBS=()
SDL_FROM="none"
sdl_flags() {
  [ "${#SDL_LIBS[@]}" -gt 0 ] && return 0
  local out
  if [ -f "$SDL/lib/pkgconfig/sdl2.pc" ]; then
    out="$(PKG_CONFIG_PATH="$SDL/lib/pkgconfig" pkg-config --cflags sdl2 2>/dev/null)" || return 1
    read -r -a SDL_CFLAGS <<< "$out"
    SDL_CFLAGS+=(-I "$SDL/include" -I "$SDL/include/SDL2")
    out="$(PKG_CONFIG_PATH="$SDL/lib/pkgconfig" pkg-config --static --libs sdl2 2>/dev/null)" || return 1
    read -r -a SDL_LIBS <<< "$out"
    SDL_FROM="$SDL (built by './build_windows.sh sdl')"
  elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2 2>/dev/null; then
    out="$(pkg-config --cflags sdl2)"; read -r -a SDL_CFLAGS <<< "$out"
    out="$(pkg-config --libs sdl2)";   read -r -a SDL_LIBS   <<< "$out"
    SDL_FROM="pkg-config sdl2 $(pkg-config --modversion sdl2)"
  elif command -v sdl2-config >/dev/null 2>&1; then
    out="$(sdl2-config --cflags)"; read -r -a SDL_CFLAGS <<< "$out"
    out="$(sdl2-config --libs)";   read -r -a SDL_LIBS   <<< "$out"
    SDL_FROM="sdl2-config $(sdl2-config --version)"
  else
    echo "error: no SDL2 found." >&2
    echo "       install the distro development package (libsdl2-dev / SDL2-devel)," >&2
    echo "       or run './build_windows.sh sdl' to build 2.30.9 from deps/ into $SDL" >&2
    return 1
  fi
  return 0
}

# --------------------------------------------------------------------------- SDL2
cmd_sdl() {
  [ -d "$SDLSRC" ] || { echo "no SDL2 source at $SDLSRC"; return 1; }
  local b="$HOME/.n64tvos/build-sdl2-windows"
  # CMAKE_POLICY_VERSION_MINIMUM: SDL 2.30.9 declares cmake_minimum_required(3.0),
  # which CMake 4 refuses outright. This is the sanctioned opt-back-in.
  cmake -S "$SDLSRC" -B "$b" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$SDL" >/dev/null || return 1
  cmake --build "$b" -j8 >/dev/null || return 1
  cmake --install "$b" >/dev/null || return 1
  echo "SDL2 -> $SDL"
  # SDL2's CMake build silently omits a video backend whose development headers are
  # absent, and the result still installs and still links -- it just cannot open a
  # window at runtime. Say which backends were configured in, while there is still
  # something to read.
  grep -E '^SDL_(X11|WAYLAND|KMSDRM|PULSEAUDIO|ALSA)(_SHARED)?:' "$b/CMakeCache.txt" 2>/dev/null
}

# --------------------------------------------------------------------- game objects
# Identical to build_mac.sh's CFLAGS except for the target triple and sysroot, which have
# no Linux equivalent and are simply absent: the native compiler already targets the host
# and already knows where its headers are. Every remaining flag matters and is
# documented at length in build_sim.sh:
#   -fms-extensions            anonymous struct/union members the decomp relies on
#   -include ge_port_decls.h   prototypes; IDO allowed implicit declarations
#   -Wno-everything -Werror=return-type   see warn_flags()
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

# Two symlinks under getv/port/include/ expose the decomp's PR/ headers without exposing
# its math.h/string.h/stdlib.h/stddef.h, which shadow the system ones. They are relative,
# so they survive a fresh checkout on any machine -- but a git clone made with symlinks
# disabled materialises them as ordinary text files, and the failure then reads as a
# missing <PR/gbi.h> rather than as a checkout problem.
require_symlinks() {
  local f
  for f in "$HERE/port/include/PR" "$HERE/port/include/platform_info.h"; do
    if [ ! -e "$f" ]; then
      echo "error: $f does not resolve." >&2
      echo "       It is a relative symlink into vendor/ge-decomp/include/. Either the" >&2
      echo "       decomp is missing, or this clone was made without symlink support." >&2
      exit 1
    fi
  done
}

linux_cflags() {
  local w; w=()
  while IFS= read -r _l; do w+=("$_l"); done < <(warn_flags)
  CFLAGS=(
    -fms-extensions -include src/ge_port_decls.h
    -I . -I include -I include/PR -I src -I src/game -I src/inflate
    -DVERSION_US -DLANG_US -DREFRESH_NTSC -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM
    -DBUGFIX_R0 -DTARGET_N64 -DGE_PORT_NATIVE
    -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C=1
    -std=gnu17 -mno-ms-bitfields
    -Wno-error=incompatible-pointer-types -Wno-error=int-conversion
    -Wno-error=implicit-function-declaration -Wno-error=implicit-int
    -Wno-error=return-mismatch
    "${w[@]}" -fno-strict-aliasing -O1
  )
  if [ "${GETV_DEBUGMENU:-0}" = "1" ]; then
    CFLAGS+=(-DDEBUGMENU)
    echo "  GETV_DEBUGMENU=1 -- debug menu ENABLED. START is repurposed."
  fi
}

build_port_layer() {
  require_thirdparty
  require_symlinks
  sdl_flags || return 1
  local pok=0 pfail=0
  local w; w=()
  while IFS= read -r _l; do w+=("$_l"); done < <(warn_flags)
  # The only intentional divergences from build_mac.sh's PORTFLAGS: GE_PLATFORM_MAC and
  # GL_SILENCE_DEPRECATION are absent, and _GNU_SOURCE is present. Everything else must
  # match, or this build stops being a valid proxy for the Mac and tvOS ones.
  #
  # -D_GNU_SOURCE is required by the harness, not by taste: glibc declares dladdr() and
  # Dl_info in <dlfcn.h> only under _GNU_SOURCE, and the crash handler uses both. On
  # Darwin they are unconditional, which is why build_mac.sh needs no equivalent.
  local PORTFLAGS=(
    -I "$HERE/port" -I "$HERE/port/include" -I "$HERE/port/fast3d" -I "$HERE/port/src"
    "${SDL_CFLAGS[@]}"
    -include "$HERE/port/include/ge_win_compat.h"
    -std=gnu17 -mno-ms-bitfields
    -Wno-error=incompatible-pointer-types -Wno-error=int-conversion
    -Wno-error=implicit-function-declaration -Wno-error=implicit-int
    -Wno-error=return-mismatch
    -DTARGET_N64 -DGE_PORT_NATIVE -D_LANGUAGE_C=1 -DRAPI_GL -DWAPI_SDL2
    -DGE_PLATFORM_DESKTOP
    ${LUAFLAGS[@]+"${LUAFLAGS[@]}"}
    ${IMGUIFLAGS[@]+"${IMGUIFLAGS[@]}"}
    "${w[@]}" -O1
  )
  mkdir -p "$BUILD/obj"
  for f in "$HERE"/port/fast3d/*.c "$HERE"/port/src/*.c "$HERE"/port/audio/*.c; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.c}").o"
    if "$CC" "${PORTFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"; echo "  windows port FAILED: $(basename "$f")"; fi
  done
  # C++ in the port layer: ge_imgui.cpp and ge_launcher.cpp. Same reasoning as build_mac.sh
  # -- ImGui is C++, everything calling it is C, and the boundary is two translation units
  # behind plain-C headers. PORTFLAGS with -std=c++17 added and nothing removed, so a define
  # reaching the C files reaches these too.
  #
  # The loop runs whether or not ImGui is installed; without -DGE_WITH_IMGUI both files
  # compile to empty entry points. -Wno-everything is a clang spelling and g++ rejects it, so
  # warn_flags is not reused here: these are third-party-adjacent files whose warnings are not
  # this project's to fix, and -w is the portable way to say so.
  for f in "$HERE"/port/src/*.cpp; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.cpp}").o"
    if "$CXX" "${PORTFLAGS[@]/-Wno-everything/-w}" -std=c++17 -fno-exceptions -fno-rtti \
              -w -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"; echo "  windows port FAILED: $(basename "$f")"; fi
  done

  # The harness. ge_tvos_main.c is shared verbatim with the tvOS and Mac targets -- it is
  # plain C + SDL with no UIKit in it -- and it defines SDL_main(). ge_mac_main.c supplies
  # the real main() that forwards to it; despite living in port/mac/ that file contains no
  # Apple code at all, just a main() and two hand-written externs, and it is correct here
  # unchanged. On Linux SDL2 defines neither SDL_MAIN_NEEDED nor SDL_MAIN_AVAILABLE, so
  # <SDL.h> does not rename main and libSDL2main is not linked, exactly as on macOS.
  #
  # ge_tvos_main.c is the file item 1 in the header warns about. Expect this loop to
  # report exactly one failure until its ucontext block is branched for glibc.
  for f in "$HERE"/Sources/ge_tvos_main.c "$HERE"/port/mac/ge_mac_main.c; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.c}").o"
    if "$CC" "${PORTFLAGS[@]}" -c "$f" -o "$o" 2>/dev/null; then pok=$((pok+1))
    else pfail=$((pfail+1)); rm -f "$o"
         echo "  windows port FAILED: $(basename "$f")"; fi
  done
  echo "windows port layer: $pok built, $pfail failed"
}

# Parallel, unlike build_sim.sh, which compiles all ~823 objects sequentially. Nothing in
# the decomp generates a shared intermediate and every object path is unique, so the work
# is embarrassingly parallel.
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
# The read loop is deliberately not `mapfile`, which is bash 4 only. Every current Linux
# distribution ships bash 5, so this is portability kept rather than portability needed --
# but it costs nothing and it keeps this file diffable against build_mac.sh, where macOS's
# stock bash 3.2 makes it mandatory.
run_batch() {
  local label="$1"; shift
  local res
  printf '%s\n' "$@" > "$BUILD/flags.txt"
  res="$(GE_FLAGFILE="$BUILD/flags.txt" GE_BUILD="$BUILD" GE_DECOMP="$DECOMP" GE_CC="$CC" \
         xargs -P "$GE_JOBS" -I{} bash -c '
            F=(); while IFS= read -r _l; do F+=("$_l"); done < "$GE_FLAGFILE"
            o="$GE_BUILD/obj/$(echo "${1%.c}" | tr "/" "_").o"
            if ( cd "$GE_DECOMP" && "$GE_CC" "${F[@]}" -c "$1" -o "$o" 2>/dev/null ); then
              echo "OK $1"
            else
              # Never leave the previous object behind: a stale .o outlives the source
              # that produced it and turns a broken file into a green-looking build.
              rm -f "$o"; echo "FAIL $1"
            fi' _ {})"
  echo "$res" | grep '^FAIL ' | sed 's/^FAIL /  windows FAILED: /'
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
  require_symlinks
  mkdir -p "$BUILD/obj"
  # Fresh manifest per full compile; run_batch appends to it.
  : > "$BUILD/objects.txt"
  local CFLAGS; linux_cflags

  (cd "$DECOMP" && { find src -name '*.c' \
             -not -path 'src/libultra/*' -not -path 'src/libultrare/*' \
             -not -name 'ge_layout_audit.c' -not -name 'ge_asset_fileview_check.c'
           # Held back on purpose; mirrors build_mac.sh, build_sim.sh and build.sh exactly.
           # usb.c/rmon.c/sched.c/ramrom.c/init.c/indy_* are N64 hardware and dev-host
           # files: compiling them turns logging stubs into code that writes real RCP/PI
           # registers or talks to an SGI host. They used to fail to build, and that
           # failure was the guard. Switching -w -> -Wno-everything (needed because -w
           # defeats -Werror=return-type) also suppresses clang's default-error
           # diagnostics, so all seven started compiling -- a weaker guard, not progress.
           # Excluding them by name is explicit. This list must stay in step with the
           # other three scripts or this build stops being a valid proxy for them.
           find src/libultra/gu -name '*.c'; } | grep -vE '/(ramromreplay\.c|audi\.c|usb\.c|rmon\.c|sched\.c|ramrom\.c|init\.c|indy_comms\.c|indy_commands\.c|tlb_manage\.c)$' | sort) \
    | run_batch "windows game" "${CFLAGS[@]}"

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
    | run_batch "windows assets" "${CFLAGS[@]}"

  # -DNDEBUG is passed to the mixer only, exactly as the other three builds do. Do not
  # widen it: SUPPORT_CHECK in gfx_pc.c is an assert() and is deliberately armed.
  (cd "$DECOMP" && find src/libultra/audio src/libultrare/audio -name '*.c' 2>/dev/null | sort) \
    | run_batch "windows audio" "${CFLAGS[@]}" -DGE_AUDIO_MIXER -DNDEBUG \
        -I src/libultra -I src/libultrare -I "$HERE/port/audio"

  build_port_layer
}

cmd_port() {
  if [ ! -d "$BUILD/obj" ] || [ -z "$(ls -A "$BUILD/obj" 2>/dev/null)" ]; then
    echo "no objects in $BUILD/obj -- run './build_windows.sh lib' once first"; return 1
  fi
  build_port_layer
}

# --------------------------------------------------------------------------- link
cmd_app() {
  [ -d "$BUILD/obj" ] || { echo "nothing built -- run './build_windows.sh lib'"; return 1; }
  sdl_flags || return 1

  # An archive, not a pile of objects, and that is a correctness requirement rather than
  # a packaging preference. Linking build-windows/obj/*.o directly fails with ~30 undefined
  # symbols (`_codeSegmentRomStart`, `__osGetFpcCsr`, `crashRenderFrame`, ...). Every one
  # of them is an N64 linker-script or hardware symbol referenced only by src/init.c,
  # src/sched.c and src/rmon.c -- files this port compiles but deliberately never links.
  # A static archive pulls a member only when it resolves an undefined symbol, so those
  # three objects are never dragged in; a direct object link has no such filter.
  #
  # Do not reach for --whole-archive, the GNU spelling of -force_load: assets/obseg/bg/
  # {u,e,j}/ contain colliding bare symbols (`header`, `room_data_table`) which are
  # harmless today only because those objects are never pulled.
  #
  # GNU ld and lld both re-scan a single archive until it stops resolving new symbols, so
  # the mutual references inside libge.a need no --start-group.
  #
  # The two harness objects stay outside the archive: they carry main()/SDL_main() and
  # are the roots the whole link is discovered from.
  local -a roots=("$BUILD/obj/port_ge_tvos_main.o" "$BUILD/obj/port_ge_mac_main.o")
  local f
  for f in "${roots[@]}"; do
    [ -e "$f" ] || { echo "missing harness object $f -- run './build_windows.sh port'"; return 1; }
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
      echo "linux excluded $orphans orphaned object(s) whose source left the build"
    fi
  else
    echo "linux note: no object manifest; archiving every object found -- run './build_windows.sh lib'"
    for f in "$BUILD"/obj/*.o; do
      case "$f" in
        *"/port_ge_tvos_main.o"|*"/port_ge_mac_main.o") continue ;;
      esac
      members+=("$f")
    done
  fi
  ar rcs "$BUILD/libge.a" "${members[@]}" || return 1
  echo "windows libge.a: $(du -h "$BUILD/libge.a" | cut -f1), ${#members[@]} members"

  # No --gc-sections, and that is deliberate. build_mac.sh needed -dead_strip only because
  # ld64 strips before it diagnoses undefined symbols, which was hiding four references
  # from functions this port never calls. getv/port/src/port_n64_unused.c now defines all
  # four for real, so nothing here depends on a linker collecting garbage first -- which
  # matters, because GNU ld makes no promise to diagnose unresolved externals only after
  # --gc-sections. If a link does fail on an undefined symbol, that is information: find
  # out who references it before reaching for a flag that might merely hide it again.
  #
  # -lc++ has no counterpart here. build_mac.sh's comment says Fast3D is C++; in this tree
  # port/fast3d/ is five .c files and nothing else, so no C++ runtime is needed. If a link
  # ever does fail on C++ ABI symbols, add -lstdc++ (or -lc++ under clang with libc++).
  #
  # -lz and -liconv likewise have no counterpart: on macOS they are static-SDL2
  # dependencies, and here SDL2's own pkg-config output names whatever it needs.
  #
  # -lGL last-resort note: gfx_opengl.c relies on GL_GLEXT_PROTOTYPES and libGL's exported
  # entry points, so there is no loader to initialise and no GLEW. -ldl and -lpthread are
  # listed explicitly rather than left to SDL2's flags, because a statically linked SDL2
  # needs them and a shared one does not, and the redundancy is free.
  # $BIN is removed first: a link can fail and leave the previous run's binary in
  # place, and the -x test below would then pass and report success. The exit status
  # is taken from PIPESTATUS rather than from the pipeline, because the pipeline's
  # status is head's, which is 0 even when the compiler failed. Both of those together
  # is what let a build with 27 uncompiled translation units and a screenful of
  # undefined references still print a binary line and exit 0.
  rm -f "$BIN"
  # -rdynamic puts the static functions into .dynsym, which is what dladdr() reads.
  # Without it the crash handler in Sources/ge_tvos_main.c resolves every frame to
  # "./goldeneye(+0xe2a1e)" and a fault is unreadable. Mach-O needs no equivalent: its
  # symbol table is always present, which is why this only matters here.
  "$CC" -o "$BIN" -rdynamic \
    "${roots[@]}" "$BUILD/libge.a" \
    ${LUALIBS[@]+"${LUALIBS[@]}"} ${IMGUILIBS[@]+"${IMGUILIBS[@]}"} \
    "${SDL_LIBS[@]}" \
    -lstdc++ -lopengl32 -ldbghelp -lm \
    2>&1 | head -40
  linkrc=${PIPESTATUS[0]}
  if [ "$linkrc" -ne 0 ]; then
    echo "LINK FAILED (compiler driver exit $linkrc)"; return 1
  fi
  if [ -x "$BIN" ]; then
    echo "windows binary: $BIN ($(du -h "$BIN" | cut -f1), $(file -b "$BIN" | cut -d, -f1-2))"
  else
    echo "LINK FAILED"; return 1
  fi
}

cmd_run() {
  [ -x "$BIN" ] || { echo "no binary -- run './build_windows.sh all'"; return 1; }
  # Nothing to forward: a native process just inherits the environment, so every GETV_*
  # knob works exactly as exported.
  "$BIN" "$@"
}

case "${1:-}" in
  sdl)  cmd_sdl ;;
  lib)  cmd_lib ;;
  port) cmd_port ;;
  app)  cmd_app ;;
  all)  cmd_lib && cmd_app ;;
  run)  shift; cmd_run "$@" ;;
  env)  echo "CC=$CC ($CC_KIND)"
        sdl_flags && { echo "SDL=$SDL_FROM"; echo "SDL_CFLAGS=${SDL_CFLAGS[*]}"
                       echo "SDL_LIBS=${SDL_LIBS[*]}"; }
        echo "WARN=$(warn_flags | tr '\n' ' ')"
        echo "BUILD=$BUILD"; echo "BIN=$BIN" ;;
  *)    echo "usage: $0 {sdl|lib|port|app|all|run|env}"
        echo "  sdl  = build SDL2 2.30.9 static from deps/ into $SDL (only if the"
        echo "         distro package is missing or too old)"
        echo "  lib  = compile game + assets + audio + port layer for this host"
        echo "  port = recompile getv/port/** and the harness only (seconds)"
        echo "  app  = link $BIN"
        echo "  run  = launch it" ;;
esac
