#!/usr/bin/env bash
# One command, from a fresh clone to a binary you can run. macOS and Linux.
#
# tools/setup.sh already fetched the dependencies and stopped, which was the honest place to
# stop when the rest was four manual steps out of docs/SETUP.md. It is not four steps. It is
# fourteen asset commands in a fixed order, six namespacing invocations with different flags,
# a patch that must go on afterwards and not before, and a build. Every one of them has a way
# to go wrong quietly, and section 3.5 documents each of those ways because somebody hit it.
#
# So this runs the whole thing. It is not a different procedure from docs/SETUP.md, it is that
# procedure with the ordering constraints encoded instead of described.
#
#   bash tools/install.sh
#   bash tools/install.sh --rom ~/Desktop/goldeneye.n64
#   bash tools/install.sh --no-build --yes
#
#   --rom PATH   use this dump. Any byte order; it identifies the header and converts.
#   --no-build   stop once the assets are ready
#   --yes, -y    never prompt. Without it, anything that would write outside the repo asks.
#   --desktop    Linux only: a menu entry and icons under $HOME, never /usr/share
#
# THREE THINGS IT WILL NOT DO
#
#   It will not fetch a ROM. Not from a URL, not from an argument that looks like one, not
#   ever. You supply your own copy of a game you own. Everything else here is either written
#   for this repository or fetched from its own permissively licensed upstream.
#
#   It will not run sudo. Where a system package is missing it prints the exact command for
#   your package manager and stops, because installing system packages is your decision and
#   a script that takes it for you is a script you cannot safely re-run.
#
#   It will not redo a step that is already done. Re-running is safe and is the intended way
#   to resume after fixing whatever stopped it. That matters more here than usual: the
#   namespacing pass in step 8 CORRUPTS the tree if it runs twice over an already-namespaced
#   one (docs/SETUP.md 3.6), so rather than trusting a marker this script wrote, it reads the
#   tree and asks whether the symbols are already prefixed.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

ROM_ARG=""
DO_BUILD=1
ASSUME_YES=0
DO_DESKTOP=0

while [ $# -gt 0 ]; do
    case "$1" in
        --rom)      ROM_ARG="${2:-}"; shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        --yes|-y)   ASSUME_YES=1; shift ;;
        --desktop)  DO_DESKTOP=1; shift ;;
        # Prints the header block above rather than a second copy of it that drifts from it.
        # Bounded by the first line that is not a comment, so editing the header cannot
        # silently truncate the help the way a fixed line range did.
        -h|--help)  awk 'NR==1 {next} /^#/ {sub(/^# ?/, ""); print; next} {exit}' "$0"
                    exit 0 ;;
        *)          echo "unknown option: $1 (try --help)"; exit 2 ;;
    esac
done

case "$(uname -s)" in
    Darwin) PLATFORM=mac ;;
    Linux)  PLATFORM=linux ;;
    *) printf 'error: %s is not supported here. Windows uses getv\\build_windows.ps1\n' "$(uname -s)"
       exit 1 ;;
esac

STEP=0
say()  { STEP=$((STEP + 1)); printf '\n== %d. %s\n' "$STEP" "$1"; }
info() { printf '   %s\n' "$1"; }
die()  { printf '\nstopped: %s\n' "$1" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# Prompt unless --yes. Defaults to no, so a non-interactive run never takes an action the
# person running it did not ask for.
confirm() {
    [ "$ASSUME_YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    printf '   %s [y/N] ' "$1"
    read -r reply
    case "$reply" in [yY]*) return 0 ;; *) return 1 ;; esac
}

# ---------------------------------------------------------------- 1. the toolchain

say "toolchain"
missing=""
for t in git python3 cmake; do have "$t" || missing="$missing $t"; done
have cc || have gcc || have clang || missing="$missing a-C-compiler"
have make || missing="$missing make"

if [ -n "$missing" ]; then
    echo "   missing:$missing"
    if [ "$PLATFORM" = linux ]; then
        if   have apt-get; then
            info "sudo apt install build-essential pkg-config cmake python3 git clang libsdl2-dev libgl1-mesa-dev"
        elif have dnf; then
            info "sudo dnf install gcc-c++ make pkgconf-pkg-config cmake python3 git clang SDL2-devel mesa-libGL-devel"
        elif have pacman; then
            info "sudo pacman -S base-devel cmake python git clang sdl2 mesa"
        else
            info "install: a C compiler, make, cmake, python3, git, SDL2 development headers, OpenGL development headers"
        fi
    else
        info "xcode-select --install"
        info "then: brew install cmake"
    fi
    die "install the above and run this again"
fi
info "git, python3, cmake, make and a compiler are all present"

# clang is preferred on Linux and the reason is not taste. build_linux.sh passes
# -Wno-everything, which is clang-only, and it is not cosmetic there: it suppresses the
# decomp's PR headers warning on every translation unit while -Werror=return-type stays on,
# which is the guard that catches this port's second-largest bug family. Under gcc the
# warning set genuinely differs, so a first build on gcc changes two variables at once.
if [ "$PLATFORM" = linux ]; then
    if have clang; then
        info "clang found, which is what build_linux.sh expects"
    else
        info "clang not found. gcc will be used and -Wno-everything will not apply,"
        info "so expect a large amount of warning output that is not new breakage."
    fi
fi

# ---------------------------------------------------------------- 2. port-layer sources

say "third-party port-layer sources"
if [ -f getv/port/fast3d/gfx_pc.c ]; then
    info "already present"
else
    bash tools/fetch-thirdparty.sh fetch
fi

# ---------------------------------------------------------------- 3. the decompilation

say "the decompilation, and every patch in getv/patches"
FRESH_CLONE=0
if [ -d vendor/ge-decomp/.git ]; then
    info "already cloned"
else
    git clone --depth 1 https://github.com/n64decomp/007 vendor/ge-decomp
    FRESH_CLONE=1
    info "cloned"
fi

# Applied by globbing the directory in numeric order rather than by a hand-written list.
#
# The list is how 0003 and then 0007 came to be committed and never applied by either setup
# script: the tree builds, nothing complains, and the change simply is not there. Two patches
# went missing that way, which is two more than a list is worth. A patch dropped into this
# directory is now applied by definition.
#
# 0002 is the exception and is skipped here. It carries generated asset sources that do not
# exist until the pipeline in step 6 has run, and applying it before the namespacing pass
# double-prefixes the font symbols. It goes on in step 7, which is the only correct moment.
# A fresh clone and an established tree need opposite handling, and conflating them is what
# the first version of this got wrong.
#
# On a fresh clone the order is fixed and every patch must apply. A failure is real.
#
# On an established tree, `git apply --reverse --check` is NOT a test of whether a patch is
# applied, which is the trap. The patches layer: 0003 and 0010 both edit
# src/game/objective_status.c after 0001 has, so 0001 stops reverse-applying the moment 0003
# goes on, even though it is applied and correct. Testing that way reports a healthy tree as
# broken.
#
# So on an established tree the question asked is the other one: does the patch apply CLEANLY
# GOING FORWARD? If it does, its changes are genuinely absent and it should go on, which is
# what catches a patch added since this tree was set up. If it does not, it is either already
# applied or has drifted from regenerated sources, and in both cases the right move is to
# leave it alone and say so.
for p in getv/patches/0*.patch; do
    case "$(basename "$p")" in 0002-*) continue ;; esac
    name="$(basename "$p")"
    if [ "$FRESH_CLONE" = 1 ]; then
        ( cd vendor/ge-decomp && git apply "$ROOT/$p" ) \
            || die "$name failed to apply to a fresh checkout; see getv/patches/README.md"
        info "$name: applied"
    elif ( cd vendor/ge-decomp && git apply --check "$ROOT/$p" ) 2>/dev/null; then
        ( cd vendor/ge-decomp && git apply "$ROOT/$p" ) || die "$name failed to apply"
        info "$name: was missing from this tree, applied now"
    else
        info "$name: already applied, or drifted; left alone"
    fi
done

# ---------------------------------------------------------------- 4. Lua, ImGui, SDL2

say "Lua and Dear ImGui"
if compgen -G "deps/lua-*" >/dev/null;   then info "Lua already fetched";   else bash tools/fetch_lua.sh   || info "Lua fetch failed; mods will be unavailable"; fi
if compgen -G "deps/imgui-*" >/dev/null; then info "ImGui already fetched"; else bash tools/fetch_imgui.sh || info "ImGui fetch failed; the launcher will be unavailable"; fi

say "SDL2"
if [ "$PLATFORM" = mac ]; then
    if [ -d "$HOME/.n64tvos/sdl2-mac" ]; then
        info "already built"
    else
        # Built from source rather than taken from Homebrew, and that is deliberate: this
        # machine's Homebrew is x86_64 under Rosetta, so brew's SDL2 is Intel-only and would
        # silently give an x86_64 build on an arm64 Mac.
        bash getv/build_mac.sh sdl
    fi
else
    if   pkg-config --exists sdl2 2>/dev/null; then info "found via pkg-config: $(pkg-config --modversion sdl2)"
    elif have sdl2-config;                     then info "found via sdl2-config: $(sdl2-config --version)"
    else
        info "no SDL2 development package found. This is the single most likely reason a"
        info "first Linux build stops before compiling anything."
        if   have apt-get; then info "sudo apt install libsdl2-dev"
        elif have dnf;     then info "sudo dnf install SDL2-devel"
        elif have pacman;  then info "sudo pacman -S sdl2"
        fi
        die "install SDL2 development headers and run this again"
    fi
fi

# ---------------------------------------------------------------- 5. your copy of the game

say "your copy of the game"

ROM_DEST="roms/ge007.u.z64"
DECOMP_ROM="vendor/ge-decomp/baserom.u.z64"
ROM_SHA="abe01e4aeb033b6c0836819f549c791b26cfde83"

sha1_of() {
    if   have shasum;   then shasum -a 1 "$1" | awk '{print $1}'
    elif have sha1sum;  then sha1sum "$1"     | awk '{print $1}'
    else die "no shasum or sha1sum available to verify the ROM"; fi
}

# A dump in the wrong byte order is the normal case, not the exception, and the file
# extension does not tell you which one you have. The header does.
#
#   80371240  z64  big endian, what the build wants
#   37804012  v64  byte-swapped in pairs
#   40123780  n64  word-reversed
#
# This is worth automating because the failure it prevents is not a clear one: the wrong
# order gets past a size check, past a "yes that is 12 MB" glance, and fails later as
# garbage data inside the asset extractor.
normalise_rom() {
    src="$1"; dst="$2"
    python3 - "$src" "$dst" <<'PY'
import sys, pathlib
src, dst = sys.argv[1], sys.argv[2]
b = pathlib.Path(src).read_bytes()
magic = b[:4].hex()
if   magic == "80371240": out = b                                   # z64, already correct
elif magic == "37804012": out = bytes(b[i ^ 1] for i in range(len(b)))   # v64, pairwise
elif magic == "40123780":                                           # n64, word-reversed
    out = b"".join(b[i:i+4][::-1] for i in range(0, len(b), 4))
else:
    print(f"unrecognised ROM header {magic}", file=sys.stderr); raise SystemExit(3)
pathlib.Path(dst).write_bytes(out)
print("z64" if magic == "80371240" else f"converted from {magic}")
PY
}

rom_ok() { [ -f "$1" ] && [ "$(sha1_of "$1")" = "$ROM_SHA" ]; }

if rom_ok "$ROM_DEST"; then
    info "$ROM_DEST verified"
else
    CANDIDATE="$ROM_ARG"
    if [ -z "$CANDIDATE" ]; then
        # Only somewhere the person running this would obviously have put it. No walking the
        # whole disk looking for game data.
        for d in "roms" "$HOME/Desktop" "$HOME/Downloads"; do
            [ -d "$d" ] || continue
            for f in "$d"/*.z64 "$d"/*.n64 "$d"/*.v64; do
                [ -f "$f" ] || continue
                [ "$(wc -c < "$f")" = "12582912" ] || continue
                CANDIDATE="$f"; break 2
            done
        done
    fi

    if [ -z "$CANDIDATE" ]; then
        cat <<'ROM'
   No ROM found, and nothing here will download one.

   Supply your own copy of GoldenEye 007 (USA), 12,582,912 bytes. Any byte order
   works; this converts it. Put it at roms/ge007.u.z64 or pass --rom <path>, then
   run this again. docs/SETUP.md section 3 covers what a correct dump looks like.
ROM
        die "no ROM"
    fi

    info "candidate: $CANDIDATE"
    if [ "$CANDIDATE" != "$ROM_DEST" ] && ! confirm "convert and copy this into roms/ ?"; then
        die "declined. Pass --rom <path> or put the file at $ROM_DEST yourself."
    fi

    mkdir -p roms
    TMP_ROM="$(mktemp)"
    normalise_rom "$CANDIDATE" "$TMP_ROM" | sed 's/^/   /'
    got="$(sha1_of "$TMP_ROM")"
    if [ "$got" != "$ROM_SHA" ]; then
        rm -f "$TMP_ROM"
        info "sha1 after conversion: $got"
        info "expected:              $ROM_SHA"
        die "that is not the US retail dump this port builds from. docs/SETUP.md 3.4 covers what to do."
    fi
    mv "$TMP_ROM" "$ROM_DEST"
    chmod 644 "$ROM_DEST"
    info "$ROM_DEST written and verified"
fi

# ---------------------------------------------------------------- 6. the asset pipeline

# The extractor reads `baserom.u.z64` from inside the decomp, not roms/ge007.u.z64, and it
# defaults that name with no way to pass another. docs/SETUP.md 3.2 says to symlink it and
# every working tree here has that link; the installer never made one, so on a genuinely fresh
# install the extractor found no ROM, EXITED 0, and wrote nothing. The failure then surfaced
# eleven steps later as "combined.bin not found", which names a file two stages downstream of
# the actual problem.
#
# Verified rather than assumed this time: a fresh clone reached the images step with
# assets/images/split holding 0 of its 2,698 entries and not one .bin anywhere under
# assets/obseg.
if [ ! -e "$DECOMP_ROM" ]; then
    ln -sf ../../roms/ge007.u.z64 vendor/ge-decomp/baserom.u.z64
    info "linked vendor/ge-decomp/baserom.u.z64 -> roms/ge007.u.z64"
fi

say "generating the asset sources"

# Order is not stylistic. It was established by building from a clean checkout and every
# entry has something downstream that fails without it, usually with an error naming a
# different file. docs/SETUP.md 3.5 is the long form of this list.
#
# Each step is skipped if its own output is already there, so a run that stops halfway can
# be resumed by running this again.
# The marker may be a literal path or a glob. The three model generators need a glob: they
# write <type>/<name>/Model.c with several hundred arbitrary names and no single fixed file,
# so there is nothing literal to point at.
marker_present() {
    [ -e "$1" ] && return 0

    # A glob marker of the shape <base>/*/<file> must not be satisfied by ONE match, and this
    # used to be, because compgen -G succeeds on any single hit. The three model generators
    # write <type>/<name>/Model.c across dozens of directories; if one dies partway having
    # written a single file, every later run reports "already done" off that one file and the
    # build then succeeds with the rest of the models simply absent. Nothing in the compile,
    # the archive or the link mentions an asset it was never handed. The Windows lane hit this
    # for real: 667 assets built, 0 failed, with 79 of 80 character models missing.
    #
    # So for that shape, count. Every subdirectory the generator walks should end up with the
    # file, which makes the expected number the directory count rather than something hardcoded
    # that drifts as assets change. Measured today: chr 80, gun 92, prop 340, all matching.
    case "$1" in
        */\*/*)
            base="${1%%/\*/*}"
            leaf="${1##*/}"
            [ -d "$base" ] || return 1
            want=$(find "$base" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l | tr -d ' ')
            have=$(find "$base" -mindepth 2 -maxdepth 2 -name "$leaf" -type f 2>/dev/null | wc -l | tr -d ' ')
            [ "$want" -gt 0 ] && [ "$have" -eq "$want" ]
            return $?
            ;;
    esac

    compgen -G "$1" >/dev/null 2>&1
}

run_asset_step() {
    marker="$1"; shift
    label="$1"; shift
    if marker_present "vendor/ge-decomp/$marker"; then
        info "$label: already done"
        return 0
    fi
    info "$label"
    ( cd vendor/ge-decomp && "$@" ) || die "$label failed"

    # And check it actually produced the thing, because an exit code of 0 is not the same
    # claim. scripts/extract_baserom.u.sh returns 0 with no ROM to read and writes nothing at
    # all, which is how a missing baserom.u.z64 travelled eleven steps before surfacing as a
    # complaint about a file two stages further on. A step that ran and produced nothing is a
    # failure here, reported against the step that actually failed.
    if ! marker_present "vendor/ge-decomp/$marker"; then
        die "$label ran and exited 0 but produced no $marker"
    fi
}

# enable_bg_extraction must run BEFORE extraction. The decomp ships 25 of the 34 bg rows with
# their extract flag at 0, because upstream builds those from checked-in .c files. This port
# compiles the blobs, so without this the link ends with 25 undefined symbols and nothing
# earlier hints at why. Running it twice is harmless, so it is not marker-guarded.
info "enabling background extraction"
( cd vendor/ge-decomp && python3 ../../tools/enable_bg_extraction.py ) >/dev/null 2>&1 || die "enable_bg_extraction.py failed"

# Markers are a specific file each generator writes, never the directory it writes into: an
# empty directory left behind by a run that died halfway would otherwise read as "done".
run_asset_step "assets/obseg/bg/bg_ame_all_p.bin"    "extracting from the ROM"  bash scripts/extract_baserom.u.sh
run_asset_step "assets/obseg/chr/*/Model.c"          "character models"         python3 scripts/generate_chr_c.py
run_asset_step "assets/obseg/gun/*/Model.c"          "weapon models"            python3 scripts/generate_gun_c.py
run_asset_step "assets/obseg/prop/*/Model.c"         "prop models"              python3 scripts/generate_prop_model_c.py
run_asset_step "assets/obseg/ge_obseg_blobs.c"       "obseg blobs"              python3 ../../tools/gen_obseg_blobs.py
run_asset_step "build/imagelist.csv"                 "image list"               python3 scripts/make/sync_imagelist_with_def.py build/imagelist.csv
run_asset_step "assets/images/combined/combined.bin" "combining images"         bash scripts/make/combine_images_named.sh build/imagelist.csv assets/images/combined
# combined.bin becomes a C array rather than an object. Upstream turns it into one with
# `ld -r -b binary`, a GNU extension Mach-O has no equivalent for.
run_asset_step "assets/images/ge_images_segment.c"   "images segment"           python3 ../../tools/gen_images_segment.py
run_asset_step "assets/ge_animation_offsets.h"       "animation blobs"          python3 ../../tools/gen_anim_blobs.py
# The marker is the .c and not the .h, and that distinction is the whole point:
# 0001-source.patch already carries src/ge_audio_segment.h, and patches are applied five
# steps before this one. Marking on the header means the header is always present by the
# time we get here, the generator never runs, and the 1.3 MB array that actually DEFINES
# geAudioSegment is never written -- which surfaces as an undefined reference from music.c
# at the final link, long after the step that was silently skipped.
run_asset_step "assets/music/ge_audio_segment.c"      "audio segment"            python3 ../../tools/gen_audio_segment.py
run_asset_step "src/ge_asset_fileview.h"             "asset file views"         python3 ../../tools/gen_asset_fileview.py

# No marker on either of these two, for opposite reasons.
#
# fix_asset_switchnodes retypes `u32 SwitchNodes[]` to real ModelNode pointers. Its own
# regex requires the `u32` spelling, so a converted file no longer matches and a second run
# is a no-op. It is idempotent by construction rather than by a guard.
info "switch nodes"
( cd vendor/ge-decomp && python3 ../../tools/fix_asset_switchnodes.py ) >/dev/null 2>&1 || die "fix_asset_switchnodes.py failed"

# gen_propdef_layout is a CHECK, not a generator, despite sitting in a list of generators.
# It compiles a throwaway translation unit, dumps clang's record layouts and asserts the
# N64 file layout still matches the native one. It writes nothing the build consumes, so it
# should run every time rather than be skipped once it has passed once.
# Its full report is a couple of hundred lines of record layouts, which is the right amount
# of detail when the check fails and pure noise when it passes. Held and printed only if it
# does fail.
info "propdef layout check"
PROPDEF_LOG="$(mktemp)"
if ( cd vendor/ge-decomp && python3 tools/gen_propdef_layout.py ) >"$PROPDEF_LOG" 2>&1; then
    rm -f "$PROPDEF_LOG"
else
    cat "$PROPDEF_LOG"
    rm -f "$PROPDEF_LOG"
    die "propdef layout check failed. The output above is its report."
fi

# ---------------------------------------------------------------- 7. namespacing

say "namespacing the asset symbols"

# THE ONE STEP THAT MUST NOT RUN TWICE.
#
# Getools emits every asset with generic global names and emits the SAME names in every
# level's file, so linked together all 29 stan files and all 50 setup files define the same
# symbols and each one binds to whichever object the linker saw first. Before this pass
# existed, the Dam's stan header resolved to another level's tile_0 and its 368-entry pad
# list resolved to a 62-entry list belonging to somewhere else. Every level ran on some
# other level's data, and it was not a link error, so nothing said so.
#
# Running it a second time over an already-patched directory double-prefixes symbols while
# leaving their uses alone, which breaks the files it previously fixed. Hence the marker:
# the presence of 0002 alone is not enough to prove the pass ran, so both are recorded.
# Two guards, and the second is the one that matters on a tree this did not build.
#
# The marker only exists on trees this installer made. Every tree set up by hand from
# docs/SETUP.md predates it, is fully namespaced, and would otherwise walk straight into the
# pass that breaks it. The applied state of 0002 is not a usable proxy either: on a working
# tree here it does not reverse-apply, because the three setup/{u,j,e}/UsetuplenZ.c files
# drift from it, which docs/SETUP.md 3.6 records as the known steady state.
#
# So ask the tree instead of asking a patch. A fresh checkout declares `PadRecord padlist[]`
# in every setup file; after the pass each one carries its own file stem, as
# `Ump_setupameZ_padlist`. No bare declaration left means the pass has run.
MARKER="vendor/ge-decomp/.getv-assets-namespaced"
namespacing_done() {
    # Ask the tree, not the marker. A marker records only that the pass RAN; it cannot know
    # whether the pass DID anything, and this pass was a silent no-op on Linux for as long as
    # the symbol reader assumed Mach-O's leading underscore. Running it a second time
    # double-prefixes and corrupts the tree, so this has to be a real test rather than a
    # re-run by default -- and the declarations themselves are the only honest evidence.
    #
    # Two tells, one per shape the pass produces. Setup files take their file stem
    # (`padlist` -> `Ump_setupameZ_padlist`); chr, gun and prop models take their directory
    # (`GFX_PRIMARY_0x48e8` -> `armourguard_Model_GFX_PRIMARY_0x48e8`). Testing only the first
    # would pass a tree whose several hundred models were never touched.
    asked=0
    if compgen -G "vendor/ge-decomp/assets/obseg/setup/*.c" >/dev/null 2>&1; then
        asked=1
        if grep -qE '(^|[[:space:]])padlist\[' vendor/ge-decomp/assets/obseg/setup/*.c 2>/dev/null; then
            return 1
        fi
    fi
    for d in chr gun prop; do
        if compgen -G "vendor/ge-decomp/assets/obseg/$d/*/Model.c" >/dev/null 2>&1; then
            asked=1
            if grep -qE '(^|[[:space:]])GFX_PRIMARY_' vendor/ge-decomp/assets/obseg/$d/*/Model.c 2>/dev/null; then
                return 1
            fi
        fi
    done
    if [ "$asked" = 1 ]; then
        return 0
    fi
    [ -e "$MARKER" ]
}

NAMESPACED_NOW=0
if namespacing_done; then
    info "already namespaced; not running again, which would corrupt it"
    touch "$MARKER"
else
    NAMESPACED_NOW=1
    # chr, gun and prop need --recurse: their models are <dir>/<name>/Model.c, so a flat glob
    # finds only .inc.c files, which the tool skips, and it prints nothing and exits 0.
    ( cd vendor/ge-decomp && python3 ../../tools/uniquify_asset_symbols.py assets/obseg/chr  --recurse ) || die "namespacing chr failed"
    ( cd vendor/ge-decomp && python3 ../../tools/uniquify_asset_symbols.py assets/obseg/gun  --recurse ) || die "namespacing gun failed"
    ( cd vendor/ge-decomp && python3 ../../tools/uniquify_asset_symbols.py assets/obseg/prop --recurse ) || die "namespacing prop failed"
    # setup must NOT be recursed: its level setups sit flat and take the file stem as prefix.
    ( cd vendor/ge-decomp && python3 ../../tools/uniquify_asset_symbols.py assets/obseg/setup )   || die "namespacing setup failed"
    # setup/u passed directly, which collapses the prefix to the bare stem, matching what the
    # rest of the tree expects.
    #
    # This one is EXPECTED to exit non-zero, and only for UsetuplenZ.c. The tool reads a file's
    # globals by compiling it and running nm, so a file that does not compile keeps the generic
    # Getools names and is reported as a SKIP. All three setup/{u,j,e}/UsetuplenZ.c are in that
    # state, which is exactly why 0002-assets.patch carries corrected copies of all three: the
    # tool cannot produce them and the patch supplies them in step 8.
    #
    # So the exit code is not the test. The list of skipped files is. Anything other than
    # UsetuplenZ.c in that list is a real failure and stops the install, because a silently
    # colliding setup file binds a level to another level's data and the build still succeeds.
    {
        # `|| nsrc=$?` rather than a bare assignment followed by `$?`. Under `set -e` a command
        # substitution that exits non-zero aborts the script AT THE ASSIGNMENT, so `nsrc=$?`
        # never runs, die() never runs, and the installer exits 1 having printed nothing at
        # all. That is precisely how this looked the first time: a fresh install stopped dead
        # in the middle of the namespacing output with no error line anywhere.
        nsrc=0
        nsout=$( cd vendor/ge-decomp && python3 ../../tools/uniquify_asset_symbols.py assets/obseg/setup/u 2>&1 ) || nsrc=$?
        unexpected=$(printf '%s\n' "$nsout" | awk '/^SKIP/ && $0 !~ /UsetuplenZ\.c/ {print}')
        if [ -n "$unexpected" ]; then
            printf '%s\n' "$nsout" | tail -20
            die "namespacing setup/u skipped a file 0002 does not supply:
$unexpected"
        fi
        if [ "$nsrc" != 0 ]; then
            info "setup/u: UsetuplenZ.c skipped, which is the known case 0002 supplies"
        fi
    }
    # stan is the easy one to leave out and the omission is silent: 29 definitions of _tile_0
    # in a static archive is not an error, it just quietly binds 28 levels to the wrong
    # collision data. This is the exact fault the pass exists to prevent.
    ( cd vendor/ge-decomp && python3 ../../tools/uniquify_asset_symbols.py assets/obseg/stan )    || die "namespacing stan failed"
    touch "$MARKER"
    info "done"
fi

# And only now 0002, which carries the corrected font files. Over assets/font the tool
# double-prefixes an already-prefixed symbol while leaving the uses alone, so the patch
# supplies those two translation units instead of the tool producing them.
if ( cd vendor/ge-decomp && git apply --reverse --check "$ROOT/getv/patches/0002-assets.patch" ) 2>/dev/null; then
    info "0002-assets.patch: already applied"
elif ( cd vendor/ge-decomp && git apply "$ROOT/getv/patches/0002-assets.patch" ) 2>/dev/null; then
    info "0002-assets.patch: applied"
elif [ "$NAMESPACED_NOW" = 1 ]; then
    # Fresh tree. The generators just ran, the namespacing pass just ran, and the patch was
    # made against exactly that state, so a failure here is real and stopping is right.
    die "0002-assets.patch failed to apply to a freshly generated tree. See getv/patches/README.md"
else
    # An established tree, which is a different situation and must not be treated as a
    # failure. Assets here have been regenerated since 0002 was cut, so it applies in
    # neither direction while the tree itself builds and runs. Forcing it would be the
    # damaging move.
    info "0002-assets.patch does not apply in either direction against this tree."
    info "That is drift between the patch and regenerated assets, not a broken checkout,"
    info "and it is left alone deliberately. A fresh install is the case 0002 is for."
fi

# ---------------------------------------------------------------- 8. build

if [ "$DO_BUILD" = 0 ]; then
    say "build skipped (--no-build)"
else
    say "building"
    if [ "$PLATFORM" = mac ]; then
        bash getv/build_mac.sh all
        BIN="getv/build-mac/goldeneye"
    else
        bash getv/build_linux.sh all
        BIN="getv/build-linux/goldeneye"
    fi
    [ -f "$BIN" ] || die "the build reported success but $BIN is not there"
    info "built $BIN"
fi

# ---------------------------------------------------------------- 9. desktop entry

if [ "$PLATFORM" = linux ] && [ "$DO_DESKTOP" = 1 ]; then
    say "desktop entry"
    # Under $HOME, never /usr/share, because this must not need root and must be removable
    # by the person who installed it.
    APPS="$HOME/.local/share/applications"
    ICONS="$HOME/.local/share/icons/hicolor"
    mkdir -p "$APPS"
    sed "s|^Exec=.*|Exec=$ROOT/getv/build-linux/goldeneye|" \
        packaging/goldeneye-plus.desktop > "$APPS/goldeneye-plus.desktop"
    if [ -d assets/icon/hicolor ]; then
        mkdir -p "$ICONS"
        cp -R assets/icon/hicolor/. "$ICONS/"
        have gtk-update-icon-cache && gtk-update-icon-cache "$ICONS" 2>/dev/null || true
    fi
    info "installed to $APPS (remove that file to uninstall)"
fi

# ---------------------------------------------------------------- done

cat <<DONE

== done

DONE
if [ "$DO_BUILD" = 1 ]; then
    if [ "$PLATFORM" = mac ]; then
        echo "  run it:      ./getv/build_mac.sh run"
        echo "  or launcher: ./getv/build-mac/goldeneye --launcher"
    else
        echo "  run it:      ./getv/build-linux/goldeneye"
    fi
    echo "  settings:    docs/CONFIGURATION.md"
    echo "  self-test:   bash getv/port/tests/run_tests.sh"
else
    echo "  assets are ready. Build with:"
    [ "$PLATFORM" = mac ] && echo "    ./getv/build_mac.sh all" || echo "    ./getv/build_linux.sh all"
fi
