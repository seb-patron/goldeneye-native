#!/usr/bin/env bash
# GoldenEye -> tvOS harness.
#
# Build the game as a
# static library, then wrap it in a minimal app we own. See
# ../../N64toATV/docs/02-harness-pattern.md.
#
# IMPORTANT difference from those two: PD and SM64 shipped with a working PC port
# layer. GoldenEye's decomp has none -- no SDL, no Fast3D, no main(). So `lib` here
# is not yet a linkable game; it is the compile front that tells us how much of the
# game builds for arm64 tvOS. The port layer comes after.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$HERE/../vendor/ge-decomp"
BUILD="$HERE/build"
SDK="$(xcrun -sdk appletvos --show-sdk-path)"
# GETV_RENDERER=gl|metal (default gl). metal selects port/fast3d/gfx_metal.mm -- see
# build_mac.sh's own header comment for the full rationale; this is the same switch,
# same default, on the tvOS device target. Only one physical Apple TV exists to deploy
# to, so unlike build_mac.sh this does NOT give metal its own BUILD dir -- switching
# renderers here always means a fresh cmd_lib anyway.
RENDERER="${GETV_RENDERER:-gl}"
case "$RENDERER" in
  gl|metal) ;;
  *) echo "unknown GETV_RENDERER: $RENDERER (want gl or metal)" >&2; exit 1 ;;
esac

# ImGui, if fetched for tvOS -- same pattern as build_ios.sh. Optional: unlike iOS/mac,
# this used to have no fetched prefix at all, so absence must stay a silent no-op (empty
# stub bodies compile from ge_launcher.cpp/ge_imgui.cpp) rather than an error.
IMGUI="${N64TVOS_PREFIX:-$HOME/.n64tvos}/imgui-tvos"
IMGUIFLAGS=(); IMGUILIBS=()
if [ -f "$IMGUI/lib/libimgui.a" ] && [ -f "$IMGUI/include/imgui.h" ]; then
  IMGUIFLAGS=( -DGE_WITH_IMGUI -I "$IMGUI/include" )
  IMGUILIBS=( "$IMGUI/lib/libimgui.a" )
fi

# Established by the port work; see the goldeneye_decomp_port memory for why each
# one is needed. -ferror-limit=0 matters: the default of 20 silently truncates and
# makes the error count look far better than it is.
CFLAGS=(
  -target arm64-apple-tvos17.0 -isysroot "$SDK"
  -fms-extensions                       # `inherits X;` is an unnamed struct member
  -include src/ge_port_decls.h          # generated prototypes; IDO allowed implicit
  -I . -I include -I include/PR -I src -I src/game -I src/inflate
  -DVERSION_US -DLANG_US -DREFRESH_NTSC -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM
  -DBUGFIX_R0 -DTARGET_N64 -DGE_PORT_NATIVE
  -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C=1
  # -Wno-everything, not -w: `-w` defeats -Werror=return-type in both orders, while
  # -Wno-everything lets it through. A non-void function falling off the end worked on
  # MIPS/IDO because the callee's result was already in $v0. The simulator build catches
  # this class; without the flag here the device build would not, so a regression would
  # fail sim and ship on device. See docs/research/PORTING_PLAYBOOK.md 1.2.
  -Wno-everything -Werror=return-type -ferror-limit=0 -fno-strict-aliasing -O1
)

# libultra* is the N64 SDK against real hardware (RCP, PI/SI DMA, exception
# handlers). A port replaces it rather than compiling it, so it is excluded.
# ge_layout_audit.c / ge_asset_fileview_check.c are generated verification TUs, not
# game code -- they are compiled by their own tools, not linked into the library.
# A subsystem must be all-stub or all-real. Compiling a reader whose initialiser is
# still stubbed moves the crash earlier, not later.
#
# ramromreplay.c is demo record/playback and is not needed to boot. It compiles, but its
# initialiser fileResetRamRomSave() is still a stub, so the real reader
# test_if_recording_demos_this_stage_load() runs against state nothing has set up and
# faults inside bossMainloop. Held back until the whole subsystem can go real.
#
# snd.c and music.c are no longer held back: audio is now uniformly real, which is what
# the all-stub-or-all-real rule demands. The AL library compiles (audio_sources below),
# port/audio/ge_mixer.c is the RSP, port_audio.c is the audio manager, port_audio_bank.c
# converts the banks, and assets/music/ge_audio_segment.c carries the five ROM segments
# that were previously a zeroed 5x1024 stub. musicSeqPlayerInit() is called from the
# harness again. If audio ever has to be disabled, put both files back here AND re-skip
# musicSeqPlayerInit(); doing only half of that reintroduces exactly the half-real state
# the rule warns about.
#
# initactorpropstuff.c is likewise no longer held back. Its initResolveAnimGroupTable()
# walks `offset + ptr_animation_table` and dereferences the result, which requires the
# animation-table loader alloc_load_expand_ani_table() to be real; it now is. The last
# remaining fault there was in our own decoder, which treated a bit-descriptor offset of
# 0 as "absent" and returned NULL. Zero is a valid offset -- idle's descriptors sit at
# the very start of the segment -- and the N64 relocation was an unconditional
# `unk08 += base`.
#
# audi.c is not in this list because it does not compile at all: it is the N64 audio
# thread, and port_audio.c replaces it wholesale.
GE_HELD_BACK_RE='ramromreplay\.c|audi\.c|usb\.c|rmon\.c|sched\.c|ramrom\.c|init\.c|indy_comms\.c|indy_commands\.c'  # see build_sim.sh: -Wno-everything let the N64 hardware files compile; exclude by name

sources() {
  (cd "$DECOMP" && { find src -name '*.c' \
      -not -path 'src/libultra/*' -not -path 'src/libultrare/*' \
      -not -name 'ge_layout_audit.c' -not -name 'ge_asset_fileview_check.c'
    # src/libultra/gu is compiled. The blanket libultra exclusion is about hardware --
    # PI/SI DMA, the VI, exception handlers -- but gu/ is the graphics-utility math:
    # guPerspective, guLookAt, guMtxF2L, guRotate/Scale/Translate. It has no hardware in
    # it and builds clean as-is. Excluding it leaves those ten as link stubs that return
    # 0 without writing their output matrix, so every projection/view/model matrix handed
    # to the renderer is uninitialised stack -- invisible in a log, and indistinguishable
    # from the game rendering nothing.
    # sqrtf.s / libm_vals.s stay out: MIPS assembly, and arm64 libm supplies sqrtf.
    find src/libultra/gu -name '*.c'
  } | grep -vE "/($GE_HELD_BACK_RE)$" | sort)
}

# The audio half of libultra is the one part of the SDK a port does not replace.
# Everything else under src/libultra* talks to real hardware (PI/SI DMA, the VI, the
# exception handlers) and is excluded above, but the audio library is pure DSP and
# sequencing logic that only emits RSP commands at the very bottom. So it compiles
# natively, and the port has to supply just the RSP itself (the mixer) and the thing
# that used to run the command list (the frame driver in port_audio.c).
#
# The file list is not "everything in the directory" -- it is exactly the list in
# src/libultrare/Makefile.libultrare, which is the decomp's record of what GoldenEye
# actually linked: the 37 stock files plus Rare's replacements for drvrNew, env and
# reverb. Do not add sndplayer.c; GoldenEye has its own sfx player in src/snd.c.
#
# These TUs get -DGE_AUDIO_MIXER, which is what makes <PR/abi.h> pull in ge_mixer.h
# and turn each a* command macro into a direct call. Nothing else in the tree sees it.
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

# The decomp extracts every asset to C source under assets/ (1,324 files, 34 MB). On the
# N64 those objects were placed in ROM segments and DMA'd in at runtime, which is why the
# game refers to _xxxSegmentRomStart/End. Compiled natively they are ordinary linked-in
# data with real pointers -- no ROM loader and no offset-to-pointer conversion is needed
# for this class of asset.
# .inc.c files are #included by other TUs, never compiled directly.
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

  # libultra audio: the game flags plus the mixer redirect. -DNDEBUG matches the
  # original build (Makefile.libultrare) and keeps the ALFailIf/assert paths out.
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

  # The software RSP. Deliberately NOT built with CFLAGS: it is port code, it wants
  # the system <string.h>/<stdint.h>, and the decomp's include/ shadows those.
  if clang -target arm64-apple-tvos17.0 -isysroot "$SDK" \
       -I "$HERE/port/include" -I "$HERE/port/audio" \
       -D_LANGUAGE_C=1 -DTARGET_N64 -DGE_PORT_NATIVE \
       -Wno-everything -Werror=return-type -ferror-limit=0 -O2 -c "$HERE/port/audio/ge_mixer.c" \
       -o "$BUILD/obj/port_ge_mixer.o" 2>/dev/null; then
    echo "software RSP: ge_mixer.o built"
  else
    echo "  port FAILED: ge_mixer.c"
  fi

  # assets: same flags plus -I assets
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

  # The port layer: Fast3D (from sm64ex -- NOT Perfect Dark, whose Fast3D was
  # rewritten for PD's custom 12-byte vertex; GoldenEye uses the standard N64 Vtx,
  # same as SM64) plus the host shims it needs.
  # NOTE: GE's include/ is deliberately absent here. The decomp ships its own
  # math.h/string.h/stdlib.h/stddef.h which shadow the system headers; port/include
  # exposes only PR/ via a symlink.
  local PORTFLAGS=(
    -target arm64-apple-tvos17.0 -isysroot "$SDK"
    -I "$HERE/port" -I "$HERE/port/include" -I "$HERE/port/fast3d" -I "$HERE/port/src"
    # both: some TUs include <SDL.h>, others <SDL2/SDL.h>
    -I ${N64TVOS_PREFIX:-$HOME/.n64tvos}/sdl2-tvos/include
    -I ${N64TVOS_PREFIX:-$HOME/.n64tvos}/sdl2-tvos/include/SDL2
    -DTARGET_N64 -DGE_PORT_NATIVE
    # port_vi.c includes PR/os.h, which defines nothing without this
    -D_LANGUAGE_C=1
    # Backend selection. sm64ex wraps whole files in these: without RAPI_GL/RAPI_METAL
    # and WAPI_SDL2, gfx_opengl.c/gfx_metal.mm and gfx_sdl2.c compile to EMPTY objects
    # and report success, then the link fails on the missing gfx_*_api / gfx_sdl structs.
    -DWAPI_SDL2
    $([ "$RENDERER" = "metal" ] && echo -DRAPI_METAL || echo -DRAPI_GL)
    # GL ES-only concerns: tvOS is GL ES only there, and supersampling is the sole route
    # to a sharper image because tvOS exposes exactly one 1920x1080 mode. Metal needs
    # neither -- USE_GLES gates GL ES headers gfx_metal.mm never includes, and
    # TVOS_SUPERSAMPLE's offscreen-framebuffer path is GL-specific (gfx_metal.mm has no
    # supersample path yet at all -- known v1 gap, see its header comment).
    $([ "$RENDERER" = "gl" ] && echo "-DUSE_GLES -DTVOS_SUPERSAMPLE")
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
  # Objective-C++: gfx_metal.mm (inert under -DRAPI_GL, symmetric with gfx_opengl.c under
  # -DRAPI_METAL) plus port/src's two ObjC++ files -- ge_virtual_controller.mm (no-op stub
  # on tvOS, real body on iOS; see its own header) and ge_launcher_metal.mm (the launcher's
  # standalone Metal context, referenced unconditionally from ge_launcher.cpp's RAPI_METAL
  # branch regardless of platform). Same ARC flag as build_ios.sh/build_mac.sh's loops,
  # which already cover both directories -- this one silently didn't and was masked by
  # stale pre-existing objects in $BUILD/obj still getting archived every run.
  for f in "$HERE"/port/fast3d/*.mm "$HERE"/port/src/*.mm; do
    [ -e "$f" ] || continue
    local o="$BUILD/obj/port_$(basename "${f%.mm}").o"
    if clang++ "${PORTFLAGS[@]}" -std=c++17 -fno-exceptions -fno-rtti -fobjc-arc -c "$f" -o "$o" 2>/dev/null; then
      pok=$((pok+1))
    else
      pfail=$((pfail+1)); echo "  port FAILED: $(basename "$f")"
    fi
  done
  # C++ in the port layer -- see build_sim.sh's identical loop for why this is needed
  # (gfx_sdl2.c's gePortImgui*() calls). IMGUIFLAGS above is empty (and this compiles
  # ge_launcher.cpp/ge_imgui.cpp's stub bodies) unless imgui-tvos has actually been
  # fetched, same as build_ios.sh.
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
    # Verify the artifact, never the exit code: check the platform is really tvOS.
    ar rcs "$BUILD/libge.a" "$BUILD"/obj/*.o
    echo "archived -> $BUILD/libge.a ($(du -h "$BUILD/libge.a" | cut -f1))"
    # Do not end this pipeline in `grep -q`. Under `set -o pipefail` grep -q exits as
    # soon as it matches, otool dies of SIGPIPE, and the pipeline reports failure even
    # though the check passed -- so a correct archive prints the warning below. It only
    # shows up once the archive is large enough (~20 MB) that otool stops finishing
    # first, so the check fails quietly rather than consistently.
    local platforms
    platforms="$(otool -l "$BUILD/libge.a" 2>/dev/null | grep -A2 LC_BUILD_VERSION | grep -c 'platform 3')"
    if [ "${platforms:-0}" -gt 0 ]; then
      echo "verified: platform 3 (tvOS), arm64"
    else
      echo "WARNING: archive is not tvOS platform 3 -- check the SDK and -target"
    fi
  fi
}

DEV_DEVICECTL="F052F5AF-631E-5842-A449-EC788A18C74D"   # "Guest Bedroom (2)" Apple TV 4K
DEV_XCODEBUILD="634383e218b182eaad97337eeade7c82e9e231cf"  # NOT the same id as devicectl
# Overridable, because a bundle identifier is the builder's own namespace rather than
# the project's. Set GETV_BUNDLE_ID to something you control before signing for a
# device; the default is deliberately generic and owned by nobody.
BUNDLE_ID="${GETV_BUNDLE_ID:-org.goldeneyenative.getv}"

cmd_app() {
  # project.yml's GCC_PREPROCESSOR_DEFINITIONS reads this via xcodegen's own ${VAR}
  # substitution (same mechanism already used there for ${N64TVOS_PREFIX} and
  # ${DEVELOPMENT_TEAM}) so ge_tvos_main.c's Xcode-compiled copy picks the SAME
  # renderer libge.a was just built with -- exported, not just set, so the subshell
  # xcodegen runs in below inherits it.
  export GETV_RAPI_DEFINE
  GETV_RAPI_DEFINE="$([ "$RENDERER" = "metal" ] && echo RAPI_METAL || echo RAPI_GL)"
  # project.yml's HEADER_SEARCH_PATHS/OTHER_LDFLAGS reference the literal ${N64TVOS_PREFIX}
  # too, and xcodegen's substitution has no concept of bash's ${VAR:-default} -- an unset
  # N64TVOS_PREFIX resolves to an empty string there, not to ~/.n64tvos, which is exactly
  # how ge_tvos_main.c stopped finding SDL.h. build_sim.sh never hits this: its sed pass
  # already replaces the whole ${N64TVOS_PREFIX}/sdl2-tvos substring with a literal path
  # before xcodegen ever sees it. This script calls xcodegen on the raw project.yml, so it
  # has to export the default itself.
  export N64TVOS_PREFIX="${N64TVOS_PREFIX:-$HOME/.n64tvos}"
  # project.yml's OTHER_LDFLAGS references ${GETV_IMGUI_LIB} the same way it references
  # ${N64TVOS_PREFIX} above -- same build_ios.sh pattern. Empty when imgui-tvos was
  # never fetched, which is fine: xcodegen substitutes an empty list entry.
  export GETV_IMGUI_LIB="${IMGUILIBS[0]:-}"
  # See build_sim.sh's identical rm -- regenerating over an existing xcodeproj can leave
  # the scheme's buildable "supported platforms" empty, which surfaces later as an
  # opaque "Found no destinations" build failure. Delete-then-regenerate is clean.
  rm -rf "$HERE/Goldeneye-Native.xcodeproj"
  ( cd "$HERE" && xcodegen generate ) || return 1
  # Build the named scheme. Passing -destination without a scheme makes xcodebuild a
  # silent no-op that reports success and produces nothing.
  xcodebuild -project "$HERE/Goldeneye-Native.xcodeproj" -scheme Goldeneye-Native \
    -configuration Release -destination "id=$DEV_XCODEBUILD" \
    -derivedDataPath "$HERE/build-device" build
}

cmd_deploy() {
  local app="$HERE/build-device/Build/Products/Release-appletvos/Goldeneye-Native.app"
  [ -d "$app" ] || { echo "no app at $app - run '$0 app' first"; return 1; }
  xcrun devicectl device uninstall app --device "$DEV_DEVICECTL" "$BUNDLE_ID" || true
  xcrun devicectl device install app --device "$DEV_DEVICECTL" "$app"
  xcrun devicectl device process launch --device "$DEV_DEVICECTL" --console "$BUNDLE_ID"
}

cmd_clean() { rm -rf "$BUILD" "$HERE/build-device"; echo "cleaned"; }

case "${1:-lib}" in
  lib)    cmd_lib ;;
  app)    cmd_app ;;
  deploy) cmd_deploy ;;
  all)    cmd_lib && cmd_app && cmd_deploy ;;
  clean)  cmd_clean ;;
  *) echo "usage: $0 {lib|app|deploy|all|clean}"; exit 2 ;;
esac
