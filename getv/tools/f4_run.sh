#!/usr/bin/env bash
# Runner: one measured launch of the Mac build, always killed, always bounded by
# GETV_EXIT_FRAME.
# usage: f4_run.sh <tag> <stage> [extra env assignments...]
set -uo pipefail
G="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$G/getv/build-mac-f4/goldeneye"
tag="$1"; stage="$2"; shift 2
log="$G/scratchpad/f4_${tag}.log"
( env GETV_STAGE="$stage" GETV_GUN_SKIPINTRO=1 GETV_EXIT_FRAME=181 "$@" "$BIN" >"$log" 2>&1 & P=$!
  for i in $(seq 1 150); do kill -0 $P 2>/dev/null || break; sleep 1; done
  kill -9 $P 2>/dev/null; wait $P 2>/dev/null )
frames=$(grep -o 'exit_frame reached: frames=[0-9]*' "$log" | tail -1 | cut -d= -f2)
fault=$(grep -o 'FAULT PC: .*' "$log" | head -1)
last=$(grep -o 'frame [0-9]*: tris submitted=[0-9]* drawn=[0-9]*' "$log" | tail -1)
printf '%-22s stage=%-3s frames=%-5s %s %s\n' "$tag" "$stage" "${frames:-CRASH}" "${last:-}" "${fault:-}"
