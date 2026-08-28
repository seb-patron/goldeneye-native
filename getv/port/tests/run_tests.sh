#!/usr/bin/env bash
# Unit tests for the port layer, on macOS and Linux.
#
# The POSIX twin of run_tests.ps1, which was the only runner and therefore made 301 checks
# Windows-only. They are the same tests: each is a standalone .c that includes the unit under
# test directly, so it can reach static functions without those being made non-static purely
# to suit a test.
#
# These run WITHOUT the game, which is the whole point. The bugs this directory exists for
# were invisible from outside a running session: input was accepted, the bot moved, and the
# only symptom was the direction it moved in. Anything that needs a level loaded belongs in
# tools/playtest.py instead.
#
# Usage:
#   ./run_tests.sh                # build and run every test_*.c here
#   ./run_tests.sh intent         # just the ones whose name contains "intent"
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GETV="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/_bin"
FILTER="${1:-}"

CC="${CC:-cc}"
command -v "$CC" >/dev/null || { echo "no compiler: set CC" >&2; exit 1; }

# Mirrors build_mac.sh's PORTFLAGS for the port layer. No forced include of ge_win_compat.h:
# that exists to supply s32/u16/OSContPad where the Windows headers do not, and here the
# decomp's own PR headers do. SDL is on the include path because several units pull it in
# transitively even though no test needs a window.
SDL_INC=""
for p in "$HOME/.n64tvos/sdl2-mac/include" /usr/include/SDL2 /usr/local/include/SDL2 \
         /opt/homebrew/include/SDL2; do
  [ -d "$p" ] && SDL_INC="$SDL_INC -I$p"
done

FLAGS=(
  -I"$GETV/port" -I"$GETV/port/include" -I"$GETV/port/fast3d" -I"$GETV/port/src"
  $SDL_INC
  -DTARGET_N64 -DGE_PORT_NATIVE -D_LANGUAGE_C=1 -DRAPI_GL -DWAPI_SDL2
  -DGE_PLATFORM_DESKTOP
  -std=gnu17 -O1
  # PR/os.h declares sprintf, memmove and friends itself. macOS turns _FORTIFY_SOURCE on at
  # -O1, which redirects those to __builtin___*_chk and then reports the decomp's own
  # prototypes as conflicting types. Not a fault in the test or the header, just two
  # declarations of the same function that cannot both win.
  -D_FORTIFY_SOURCE=0
  # PR/os.h declares bcopy, bcmp and bzero with `int` lengths, the N64 signatures. Darwin's
  # <strings.h> declares the same three with size_t, and the two cannot coexist. Which one a
  # file sees first depends on whether it happened to include <string.h>, which is why this
  # bites test_player_queue and not test_intent even though both pull in the same port source.
  # Suppressing the system header lets the N64 declarations stand, and that is the correct
  # choice rather than a workaround: ge_link_stubs.c DEFINES bcopy and bzero with the int
  # signatures, and port_os.c calls them, so the whole port already runs on the N64 shapes.
  # The guard differs per libc and both are needed, because the tests run on both. Darwin's
  # is __STRINGS_H_ with two underscores, on the SDK-internal _strings.h that <string.h>
  # pulls in -- _STRINGS_H_ is a different header there and suppresses nothing. glibc's is
  # _STRINGS_H, and its <string.h> includes <strings.h> under __USE_MISC, so the same clash
  # arrives by the same route. Safe on both because nothing under tests/ or port/src uses
  # strcasecmp, strncasecmp or ffs, which is all else that header provides.
  -D__STRINGS_H_ -D_STRINGS_H
  # -Wno-everything for the same reason the build scripts use it: the decomp's PR headers
  # warn on every translation unit and would bury a real diagnostic. return-type stays an
  # error because a non-void function falling off the end is a real bug family here.
  -Wno-everything -Werror=return-type
)

# Linked after the source rather than in FLAGS, and that is the whole reason it is a separate
# array: GNU ld resolves in command order, so a -l placed before the object that needs it
# contributes nothing and the reference stays undefined. Darwin folds libm into libSystem and
# links it either way, which is why sqrt, fmodf and sincos only went missing on Linux.
LIBS=( -lm )

mkdir -p "$OUT"
shopt -s nullglob
tests=("$HERE"/test_*.c)
[ -n "$FILTER" ] && tests=("$HERE"/*"$FILTER"*.c)
[ ${#tests[@]} -eq 0 ] && { echo "no tests matched"; exit 0; }

pass=0; fail=0; buildfail=0; failed=()
printf "  %-26s %8s  %s\n" "test" "result" "checks"
for t in "${tests[@]}"; do
  name="$(basename "$t" .c)"
  exe="$OUT/$name"
  log="$OUT/$name.buildlog"
  if ! "$CC" "${FLAGS[@]}" -o "$exe" "$t" "${LIBS[@]}" >"$log" 2>&1; then
    printf "  %-26s %8s\n" "$name" "BUILD"
    grep -E "error|undefined" "$log" | head -3 | sed 's/^/      /'
    buildfail=$((buildfail+1)); failed+=("$name"); continue
  fi
  out="$("$exe" 2>&1)"; rc=$?
  n="$(printf '%s\n' "$out" | grep -cE '^\s*(ok|OK|PASS|FAIL)' || true)"
  # A test that exits 0 having reported nothing is not a passing test, it is a test that did not
  # run. Three here printed only on failure, so they scored zero checks and read as green next to
  # twelve that were doing real work. Emptying one of their main() bodies would have looked
  # identical. Exit status alone cannot tell those apart, so the count is held to as well.
  if [ "$rc" -eq 0 ] && [ "${n:-0}" -eq 0 ]; then
    printf "  %-26s %8s  %s\n" "$name" "SILENT" "0"
    echo "      exited 0 but reported no checks: does main() still assert anything?"
    fail=$((fail+1)); failed+=("$name"); continue
  fi
  if [ "$rc" -eq 0 ]; then
    printf "  %-26s %8s  %s\n" "$name" "pass" "${n:-?}"
    pass=$((pass+1))
  else
    printf "  %-26s %8s  %s\n" "$name" "FAIL" "${n:-?}"
    printf '%s\n' "$out" | grep -iE 'fail|expected|got' | head -5 | sed 's/^/      /'
    fail=$((fail+1)); failed+=("$name")
  fi
done

echo
echo "  $pass passed, $fail failed, $buildfail did not build"
[ ${#failed[@]} -gt 0 ] && echo "  failing: ${failed[*]}"
[ $((fail + buildfail)) -eq 0 ]
