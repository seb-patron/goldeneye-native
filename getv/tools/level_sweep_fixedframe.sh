#!/usr/bin/env bash
# level_sweep_fixedframe.sh -- sweep every level at a fixed frame instead of a fixed
# wall clock, and capture the screenshot while the app is still alive.
#
# Why this exists -- the measurement artifact it removes
# ------------------------------------------------------
# The apparent bimodality of outcomes across launches of an identical binary is a harness
# artifact, not a memory bug. `gfx_end_frame()` prints `tris submitted=/drawn=` every 60
# frames and resets the counters each frame, so every line is an instantaneous sample.
# level_sweep.sh takes `tail -1`: the checkpoint that happened to be last before a fixed
# wall-clock timeout. How many frames fit in that wall clock is set by host load, which
# varies widely, and levels genuinely step their triangle count during their scripted
# opening.
#
# On DAM, one binary, three launches at three frame budgets:
#  frame 61 -> submitted=3831 drawn=2541
#  frame 181 -> submitted=3814 drawn=2366
#  frame 301 -> submitted=3389 drawn=2099
# Those are the same three values a wall-clock run reports as a 3389-3831 spread across
# launches: one run, three different instants.
#
# GETV_EXIT_FRAME=N ends the run after N rendered frames, so the last checkpoint is the
# same frame on every launch and on every host, and comparisons become frame-for-frame.
#
# The flag cannot simply be added to level_sweep.sh: with GETV_EXIT_FRAME the app calls
# _exit(0), and level_sweep.sh's screenshot fires after its wait loop sees the process
# gone, so it would capture the tvOS springboard. That scores 99.90-99.91% non-black on
# every level and would hand the highest coverage in the table to levels that drew
# nothing. This runner polls the screenshot while the process is alive and keeps the last
# live capture, so coverage stays gated on the app being on screen.
#
# The screenshot is still taken at a load-dependent frame even though the run ends at a
# fixed one: it is whatever frame the app had reached when the last poll landed. The
# counters are reproducible; `coverage`, `distinct` and `top1share` mean "late in a
# fixed-frame run" and are approximate. Do not read the picture columns as being as solid
# as the counter columns.
#
# USAGE
#  export GETV_SLOT=sweep3 GETV_SIM="GETV-sweep3"
#  GETV_EXIT_FRAME=181 getv/tools/level_sweep_fixedframe.sh 33 34 35
# Writes the same results.tsv schema level_sweep.sh does, so sweep_aggregate.py consumes
# either without knowing which produced it.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GETV="$(cd "$HERE/.." && pwd)"
SLOT="${GETV_SLOT:-sweep}"
OUT="${GETV_SWEEP_OUT:-$GETV/build-sim-$SLOT/sweep-ff}"
BUNDLE_ID="org.goldeneyenative.getv"
EXIT_FRAME="${GETV_EXIT_FRAME:-181}"
# Hard ceiling only. A healthy run ends on its own at EXIT_FRAME; this exists so a level
# that HANGS cannot stall the sweep forever. A run that hits it is reported as a hang.
CEILING="${GETV_FF_CEILING:-300}"
SHOT_EVERY="${GETV_FF_SHOT_EVERY:-6}"
SHOT_AFTER="${GETV_FF_SHOT_AFTER:-12}"

LEVELS=(
  0:DEFAULT   9:BUNKER1   20:SILO     22:STATUE   23:CONTROL  24:ARCHIVES
  25:TRAIN    26:FRIGATE  27:BUNKER2  28:AZTEC    29:STREETS  30:DEPOT
  31:COMPLEX  32:EGYPT    33:DAM      34:FACILITY 35:RUNWAY   36:SURFACE
  37:JUNGLE   38:TEMPLE   39:CAVERNS  40:CITADEL  41:CRADLE   42:SHO
  43:SURFACE2 44:ELD      45:BASEMENT 46:STACK    47:LUE      48:LIBRARY
  49:RIT      50:CAVES    51:EAR      52:LEE      53:LIP      54:CUBA
  55:WAX      56:PAM      57:MAX      90:TITLE
)

SIM_NAME="${GETV_SIM:-Apple TV 4K (3rd generation)}"
sim_udid() {
  xcrun simctl list devices available \
    | awk -v n="$SIM_NAME" '
        /^-- tvOS/ { ok = 1; next } /^-- / { ok = 0; next }
        ok && index($0, n) && match($0, /[0-9A-F-]{36}/) { u = substr($0, RSTART, RLENGTH) }
        END { print u }'
}
UDID="$(sim_udid)"
[ -n "$UDID" ] || { echo "no simulator matching '$SIM_NAME'"; exit 1; }
APP="$GETV/build-sim-$SLOT-dd/Build/Products/Release-appletvsimulator/Goldeneye-Native.app"
[ -d "$APP" ] || { echo "no app at $APP"; exit 1; }

# This must be an export, not a `${var:+name=val}` command prefix. Bash recognises
# assignment prefixes before parameter expansion, so the expanded string is treated as a
# command: an MP run then dies with "SIMCTL_CHILD_GETV_MP=2: command not found" and an
# 85-byte log that the parser scores as SILENT-HANG, making the six MP-only arenas look
# like six broken levels.
if [ -n "${GETV_SWEEP_MP:-}" ]; then export SIMCTL_CHILD_GETV_MP="$GETV_SWEEP_MP"; fi

mkdir -p "$OUT"
echo "sim: $SIM_NAME ($UDID)   exit_frame=$EXIT_FRAME  ceiling=${CEILING}s"
# Guarded: installing during a reparse would disturb a sweep running on the same device.
if [ "${1:-}" != "reparse" ]; then
  xcrun simctl install "$UDID" "$APP" >/dev/null 2>&1 || { echo "install failed"; exit 1; }
fi

RESULTS="$OUT/results.tsv"
printf 'id\tname\tstage_load\tframe_loop\tgfx_tasks\ttris_sub\ttris_drawn\tframes\tcoverage\tuniq\ttop1share\toutcome\tfault_pc\tfault_addr\tlast_mark\n' > "$RESULTS"

# Identical to level_sweep.sh's: both sweeps must go through the same
# pixel counter or a coverage comparison between them is meaningless.
# `arch -arm64` is necessary. This Mac's shell is x86_64 under Rosetta,
# /usr/bin/python3 inherits the parent's architecture, and the installed PIL is
# arm64-only, so without it every level silently reports "-" instead of a number.
COVERAGE_PY='
import sys
try:
    from PIL import Image
except Exception:
    print("- - -"); raise SystemExit
try:
    im = Image.open(sys.argv[1]).convert("RGB")
    lit = [px for px in im.getdata() if sum(px) > 24]
    n = im.width * im.height
    if not lit:
        print("0.00 0 -"); raise SystemExit
    from collections import Counter
    c = Counter(lit); top1 = c.most_common(1)[0][1]
    print("%.2f %d %.1f" % (100.0*len(lit)/n, len(c), 100.0*top1/len(lit)))
except Exception:
    print("- - -")
'

run_one() {
  local id="$1" name="$2"
  local log="$OUT/$id-$name.log" png="$OUT/$id-$name.png" live="$OUT/.$id.live.png"
  xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  rm -f "$png" "$live"
  printf '%-3s %-9s ' "$id" "$name"

  SIMCTL_CHILD_GETV_STAGE="$id" \
  SIMCTL_CHILD_GETV_SUPERSAMPLE="${GETV_SUPERSAMPLE:-1}" \
  SIMCTL_CHILD_GETV_GUN_SKIPINTRO="${GETV_SWEEP_SKIPINTRO:-1}" \
  SIMCTL_CHILD_GETV_LIGHTTRACE="${GETV_SWEEP_LIGHTTRACE:-1}" \
  SIMCTL_CHILD_GETV_ROOMTRACE="${GETV_SWEEP_ROOMTRACE:-1}" \
  SIMCTL_CHILD_GETV_EXIT_FRAME="$EXIT_FRAME" \
    xcrun simctl launch --console-pty "$UDID" "$BUNDLE_ID" > "$log" 2>&1 &
  local pid=$! waited=0 hit_ceiling=0
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$CEILING" ]; then hit_ceiling=1; kill "$pid" 2>/dev/null; break; fi
    # Keep the LAST capture taken while the app was still alive. Overwrite-in-place, so a
    # run that dies between polls leaves the previous good frame rather than a springboard.
    if [ "$waited" -ge "$SHOT_AFTER" ] && [ $(( waited % SHOT_EVERY )) -eq 0 ]; then
      if xcrun simctl io "$UDID" screenshot --type=png "$live" >/dev/null 2>&1; then
        [ -s "$live" ] && mv -f "$live" "$png"
      fi
    fi
    sleep 2; waited=$(( waited + 2 ))
  done
  wait "$pid" 2>/dev/null
  xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  parse_one "$id" "$name" "$log" "$hit_ceiling" "$waited"
}

parse_one() {
  local id="$1" name="$2" log="$3" ceil="${4:-0}" secs="${5:-0}"
  local stage_load=no frame_loop=no
  grep -q 'boot-> bm:lvlStageLoad'  "$log" && stage_load=yes
  grep -q 'boot-> bm:viInitBuffers' "$log" && stage_load=returned
  grep -qE 'boot-> bm:(waitForNextFrame|drain-enter|pre-recv-loop)' "$log" && frame_loop=yes

  local gfx_tasks frames tris_sub tris_drawn
  gfx_tasks=$(grep -c 'gfx task ' "$log" 2>/dev/null || true)
  frames=$(grep -c 'tris submitted=' "$log" 2>/dev/null || true)
  : "${gfx_tasks:=0}" "${frames:=0}"
  # Safe to take the last checkpoint ONLY because the run ends at a fixed frame: the
  # last checkpoint is now the same frame on every launch and every host.
  tris_sub=$(grep 'tris submitted=' "$log" | tail -1 | sed -n 's/.*submitted=\([0-9]*\).*/\1/p')
  tris_drawn=$(grep 'tris submitted=' "$log" | tail -1 | sed -n 's/.*drawn=\([0-9]*\).*/\1/p')
  : "${tris_sub:=-}" "${tris_drawn:=-}"

  local fault_pc fault_addr
  fault_pc=$(grep -m1 'FAULT PC:' "$log" | sed -n 's/.*FAULT PC: [^=]*= \([A-Za-z0-9_]*\) + \([0-9-]*\).*/\1+\2/p')
  [ -z "$fault_pc" ] && fault_pc=$(grep -m1 'FAULT PC:' "$log" | sed -n 's/.*FAULT PC: \(0x[0-9a-f]*\).*/\1(nosym)/p')
  : "${fault_pc:=-}"
  fault_addr=$(grep -m1 -oE 'addr[= ]0x[0-9a-f]+' "$log" | head -1 | grep -oE '0x[0-9a-f]+')
  : "${fault_addr:=-}"

  local reached=0
  grep -q 'exit_frame reached' "$log" && reached=1

  local coverage="-" uniq="-" top1="-"
  # Gate unchanged in spirit: never score a picture for a run that faulted or never drew.
  if [ "$fault_pc" = "-" ] && [ "$frame_loop" = yes ] && [ "$frames" -gt 0 ] && [ -s "${log%.log}.png" ]; then
    read -r coverage uniq top1 <<< "$(arch -arm64 /usr/bin/python3 -c "$COVERAGE_PY" "${log%.log}.png" 2>/dev/null || echo '- - -')"
  fi
  : "${coverage:=-}" "${uniq:=-}" "${top1:=-}"

  local last_mark
  last_mark=$(grep 'last boot mark:' "$log" | tail -1 | sed -n 's/.*last boot mark: \([^ ]*\).*/\1/p')
  [ -z "$last_mark" ] && last_mark=$(grep -o 'boot-> [^ ]*' "$log" | tail -1 | sed 's/boot-> //')
  : "${last_mark:=NONE}"

  local outcome
  if   [ ! -s "$log" ];                                     then outcome=NO-OUTPUT
  elif grep -q 'MEMP.*DIE\|GE_MEMP_DIE\|OUT OF MEM' "$log"; then outcome=OOM
  elif grep -q 'FAULT PC:' "$log";                          then outcome=CRASH
  elif [ "$ceil" = 1 ];                                     then outcome=HANG-CEILING
  elif [ "$frame_loop" = yes ] && [ "$frames" -gt 0 ]; then
    if   awk -v c="$coverage" 'BEGIN{exit !(c+0 < 5)}' 2>/dev/null; then outcome=RENDERS-BLACK
    elif awk -v u="$uniq" -v t="$top1" 'BEGIN{exit !(u+0 < 32 || t+0 >= 85)}' 2>/dev/null; then outcome=RENDERS-FLAT
    else outcome=RENDERS; fi
  elif [ "$frame_loop" = yes ];                             then outcome=LOOP-NO-FRAMES
  else outcome=SILENT-HANG; fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$id" "$name" "$stage_load" "$frame_loop" "$gfx_tasks" "$tris_sub" "$tris_drawn" \
    "$frames" "$coverage" "$uniq" "$top1" "$outcome" "$fault_pc" "$fault_addr" "$last_mark" >> "$RESULTS"
  printf '%-14s %3ds ef=%s f=%-3s cov=%-7s dist=%-6s top1=%-5s sub=%-6s drawn=%-6s %s\n' \
    "$outcome" "$secs" "$reached" "$frames" "$coverage" "$uniq" "$top1" "$tris_sub" "$tris_drawn" \
    "$([ "$fault_pc" != - ] && echo "@ $fault_pc" || echo "mark=$last_mark")"
}

# `reparse` rebuilds results.tsv from the .log/.png files already in $out without
# launching anything.
# This is not optional polish. results.tsv is truncated at the top of every invocation,
# so re-running one level into an existing output directory silently destroys every other
# row. The logs are the raw data and they survive, so a re-run plus a reparse restores the
# table. Repeat the level, then reparse.
SELECT=("$@")
REPARSE=0
if [ "${1:-}" = "reparse" ]; then REPARSE=1; SELECT=(); fi

for entry in "${LEVELS[@]}"; do
  id="${entry%%:*}"; name="${entry##*:}"
  if [ "$REPARSE" = 1 ]; then
    [ -f "$OUT/$id-$name.log" ] || continue
    printf '%-3s %-9s ' "$id" "$name"
    parse_one "$id" "$name" "$OUT/$id-$name.log" 0 0
    continue
  fi
  if [ ${#SELECT[@]} -gt 0 ]; then
    keep=0; for s in "${SELECT[@]}"; do [ "$s" = "$id" ] && keep=1; done
    [ "$keep" = 1 ] || continue
  fi
  run_one "$id" "$name"
done
echo "results -> $RESULTS"
