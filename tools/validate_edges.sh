#!/bin/bash
# Measure every level's walkable edges by asking the game, one boot per level.
#
# The offline test (path_is_clear, triangle intersection against exported wall polys) disagrees
# with bondviewTestLineUnobstructed, and the engine is the authority. This replaces assumption
# with measurement for the whole graph.
#
# Needs a packed world that already contains the synthetic nodes -- spawn, doors, portals --
# so run it AFTER gen_level_routes and pack_world, then regenerate routes with the verdicts.
# Validating a pack without them measures a graph nobody routes on.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=getv/build-mac/goldeneye
[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 1; }

# stage id : extractor level name. Same table as tools/dump_spawns.py; MP-only stages excluded.
LEVELS="9:bunker1 20:silo 22:statue 23:control 24:archives 25:train 26:frigate 27:bunker2
28:aztec 29:streets 30:depot 32:egypt 33:dam 34:facility 35:runway 36:surface 37:jungle
39:caverns 41:cradle 43:surface2"

ok=0; fail=0
for entry in $LEVELS; do
    stage="${entry%%:*}"; name="${entry##*:}"
    out=$(env GETV_WORLD_DIR=build/world GETV_BOT_ROUTE_LEVEL="$name" GETV_BOT_ROUTE=0 \
              GETV_PADS=2 GETV_EDGEVALIDATE=1 GETV_STAGE="$stage" GETV_EXIT_FRAME=701 \
              "$BIN" 2>&1 | grep '\[getv\]\[edges\]')
    if [ -n "$out" ]; then
        echo "  $out"
        ok=$((ok+1))
    else
        # Named, not counted silently: a short table reads like a complete one.
        echo "  $name (stage $stage) FAILED -- no edge line"
        fail=$((fail+1))
    fi
done
echo
echo "$ok level(s) measured, $fail failed"
