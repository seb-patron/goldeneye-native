#!/bin/bash
# Load every named stage id and report what happens.
#
# The README states how many stages load and how many carry no data. This is where those
# numbers come from, so they can be rechecked rather than trusted.
#
# Stages are tried on their own first and retried with two players if that fails, because six
# of them are multiplayer-only and have no solo setup. A stage that neither loads nor reports
# a reason is a real fault and is listed as BAD.
#
# usage: tools/stage_census.sh [path-to-binary]
set -uo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${1:-$HERE/getv/build-mac/goldeneye}"
[ -x "$GAME" ] || { echo "no binary at $GAME -- build first, or pass one"; exit 1; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

IDS="9:Bunker1 20:Silo 22:Statue 23:Control 24:Archives 25:Train 26:Frigate 27:Bunker2
     28:Aztec 29:Streets 30:Depot 31:Complex 32:Egypt 33:Dam 34:Facility 35:Runway
     36:Surface 37:Jungle 38:Temple 39:Caverns 40:Citadel 41:Cradle 42:Sho 43:Surface2
     44:Eld 45:Basement 46:Stack 47:Lue 48:Library 49:Rit 50:Caves 51:Ear 52:Lee 53:Lip
     54:Cuba 55:Wax 56:Pam"

run() { GETV_STAGE="$1" GETV_EXIT_FRAME=90 GETV_NO_AUDIO=1 GETV_WINDOW=1 \
        ${2:+env GETV_MP=2} timeout -s KILL 75 "$GAME" >"$TMP/o" 2>&1; }

solo=0; mp=0; nodata=0; bad=0
for e in $IDS; do
  id=${e%%:*}; name=${e##*:}
  if GETV_STAGE=$id GETV_EXIT_FRAME=90 GETV_NO_AUDIO=1 GETV_WINDOW=1 \
     timeout -s KILL 75 "$GAME" >"$TMP/o" 2>&1 && grep -q 'exit_frame reached' "$TMP/o"; then
    printf '  %-10s %-3s loads\n' "$name" "$id"; solo=$((solo+1)); continue
  fi
  if GETV_MP=2 GETV_STAGE=$id GETV_EXIT_FRAME=90 GETV_NO_AUDIO=1 GETV_WINDOW=1 \
     timeout -s KILL 75 "$GAME" >"$TMP/o" 2>&1 && grep -q 'exit_frame reached' "$TMP/o"; then
    printf '  %-10s %-3s loads (multiplayer only)\n' "$name" "$id"; mp=$((mp+1)); continue
  fi
  if grep -q '\[getv\] stage .* has ' "$TMP/o"; then
    printf '  %-10s %-3s no data: %s\n' "$name" "$id" \
      "$(grep -o 'has [a-z ]*' "$TMP/o" | head -1)"; nodata=$((nodata+1)); continue
  fi
  printf '  %-10s %-3s BAD (no reason given)\n' "$name" "$id"; bad=$((bad+1))
done

echo
echo "  $solo load, $mp multiplayer only, $nodata carry no data, $bad unexplained"
[ "$bad" -eq 0 ] || exit 1
