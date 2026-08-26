#!/usr/bin/env bash
# level_sweep_multi.sh -- run level_sweep.sh N times and report the SPREAD.
#
# Why this exists (PORTING_PLAYBOOK.md 2.5)
# -----------------------------------------
# `level_sweep.sh` takes exactly one sample per level. Outcomes on this port vary across
# launches of an identical binary (`submitted` has come out 64 on six launches and 1139
# on a seventh, same binary, same knobs, same device). A one-sample sweep is therefore
# not a measurement.
#
# This wrapper does not replace the harness; it calls it n times, into n separate output
# directories, then aggregates. level_sweep.sh is used widely, so wrapping rather than
# editing keeps a bug here from breaking other callers.
#
# How the extra knobs get in without touching level_sweep.sh
#   `xcrun simctl launch` forwards every SIMCTL_CHILD_* variable in its own environment
#   to the child. level_sweep.sh sets SIMCTL_CHILD_GETV_STAGE/_SUPERSAMPLE as a command
#   prefix, which adds to rather than replaces what is exported here. So exporting
#   SIMCTL_CHILD_GETV_GUN_SKIPINTRO=1 below is enough to move the whole run from the
#   intro camera into first-person gameplay.
#
# Measure in first person. Some levels render richly under the intro fly-by camera and
# collapse the moment gameplay begins, so an intro-camera board measures a camera rather
# than a level. GETV_GUN_SKIPINTRO=1 is on by default here; set GETV_SWEEP_SKIPINTRO=0 to
# take the intro-camera reading instead.
#
# GETV_SUPERSAMPLE is not a neutral speed knob (2.1). It changes framebuffer size, which
# changes heap layout, which changes outcomes. It is pinned for every sample in a run and
# stamped into the output so results cannot be diffed across settings by accident.
#
# USAGE
#   export GETV_SLOT=sweep3 GETV_SIM="GETV-sweep3"
#   ./build_sim.sh lib && ./build_sim.sh app
#   getv/tools/level_sweep_multi.sh                 # all levels, n=3
#   GETV_SWEEP_N=5 getv/tools/level_sweep_multi.sh 33 34
#
# OUTPUT ($BASE = build-sim-<slot>/sweep-multi)
#   $BASE/s<i>/            a complete ordinary level_sweep.sh output tree per sample
#   $BASE/board.tsv        one row per level: median/min/max + stability class
#   $BASE/board.md         the same as a table, with the bimodal levels called out
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GETV="$(cd "$HERE/.." && pwd)"
SLOT="${GETV_SLOT:-sweep}"
N="${GETV_SWEEP_N:-3}"
BASE="${GETV_SWEEP_MULTI_OUT:-$GETV/build-sim-$SLOT/sweep-multi}"
SS="${GETV_SUPERSAMPLE:-1}"
SKIPINTRO="${GETV_SWEEP_SKIPINTRO:-1}"
LIGHTTRACE="${GETV_SWEEP_LIGHTTRACE:-1}"
ROOMTRACE="${GETV_SWEEP_ROOMTRACE:-1}"
MP="${GETV_SWEEP_MP:-}"

# Held constant across every sample in this run, and echoed into board.md. A knob that
# is not recorded alongside the number makes the number unusable later.
export GETV_SUPERSAMPLE="$SS"
export SIMCTL_CHILD_GETV_GUN_SKIPINTRO="$SKIPINTRO"
# GETV_LIGHTTRACE is what makes clip/cull observable at all: the per-triangle reject
# counters are only incremented, and only printed, when it is on. It prints one block
# per frame to stderr, which level_sweep.sh folds into the .log.
export SIMCTL_CHILD_GETV_LIGHTTRACE="$LIGHTTRACE"
# GETV_ROOMTRACE is what proves the run was in first person. It prints
# `cam=<CAMERAMODE>` and the deterministic `eyeheight=` scalar; without it the log holds
# no evidence that GETV_GUN_SKIPINTRO took effect on a given level. It is cheap, gated to
# frames below 6 and every 60th thereafter, unlike LIGHTTRACE which prints every frame.
export SIMCTL_CHILD_GETV_ROOMTRACE="$ROOMTRACE"
# Six stages are multiplayer-only (GE_GAME_FACTS.md 3): COMPLEX 31, TEMPLE 38,
# basement 45, stack 46, library 48, caves 50. prop.c synthesises the setup filename as
# "Usetup<code>Z" at 1 player and "Ump_setup<code>Z" at 2 or more, and the solo form of
# those six does not exist -- not on disk, not in the ROM manifest. Run solo they load
# geometry and no setup at all: no props, no guards, no Bond spawn. The resulting
# "submitted=27 drawn=7 distinct=3" is an absent setup, not a renderer bug. Run them with
# GETV_SWEEP_MP=2.
if [ -n "$MP" ]; then export SIMCTL_CHILD_GETV_MP="$MP"; fi

mkdir -p "$BASE"
echo "slot=$SLOT sim=${GETV_SIM:-<default>} n=$N ss=$SS skipintro=$SKIPINTRO lighttrace=$LIGHTTRACE roomtrace=$ROOMTRACE mp=${MP:-off}"
echo "base: $BASE"

for i in $(seq 1 "$N"); do
  echo
  echo "================ SAMPLE $i / $N ================"
  GETV_SWEEP_OUT="$BASE/s$i" "$HERE/level_sweep.sh" "$@" || true
done

echo
echo "================ AGGREGATING ================"
arch -arm64 /usr/bin/python3 "$HERE/sweep_aggregate.py" "$BASE" \
 --n "$N" --ss "$SS" --skipintro "$SKIPINTRO" --lighttrace "$LIGHTTRACE" \
 --sim "${GETV_SIM:-default}" --slot "$SLOT" --mp "${MP:-off}"
echo
echo "board -> $BASE/board.md"
