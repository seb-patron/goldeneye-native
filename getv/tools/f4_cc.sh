#!/usr/bin/env bash
# Recompile one decomp source into build-mac-f4/obj with the same "mac game" flags.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DECOMP="$HERE/../vendor/ge-decomp"
BUILD="$HERE/build-mac-f4"
SDK="$(xcrun -sdk macosx --show-sdk-path)"
TARGET="arm64-apple-macos13.0"
CFLAGS=(-target "$TARGET" -isysroot "$SDK" -fms-extensions -include src/ge_port_decls.h
  -I . -I include -I include/PR -I src -I src/game -I src/inflate
  -DVERSION_US -DLANG_US -DREFRESH_NTSC -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM
  -DBUGFIX_R0 -DTARGET_N64 -DGE_PORT_NATIVE
  -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C=1
  -Wno-everything -Werror=return-type -ferror-limit=0 -fno-strict-aliasing -O1)
rc=0
for f in "$@"; do
  o="$BUILD/obj/$(echo "${f%.c}" | tr '/' '_').o"
  if ( cd "$DECOMP" && clang "${CFLAGS[@]}" -c "$f" -o "$o" ); then echo "OK   $f -> $(basename "$o")"
  else echo "FAIL $f"; rm -f "$o"; rc=1; fi
done
exit $rc
