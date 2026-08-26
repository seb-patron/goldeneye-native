#!/usr/bin/env bash
# level_sweep.sh -- boot every LEVELID in turn and record how far each one gets.
#
# Purpose
# -------
# Turns "does level N work?" from a manual per-level ritual into a table. This is a
# measuring tool only: it changes nothing in the game, it launches, waits, kills and
# parses.
#
# Isolation -- read this before running two of these at once.
#  * build_sim.sh picks the simulator by name. Two concurrent runs on one device wedge
#  each other; the symptom is a run with no output and no error, and eventually
#  `simctl terminate` hangs. Create a separate device and export GETV_SIM.
#  * Export GETV_SLOT=<name> for a private build-sim-<name>/{obj,libge.a}, derived data
#  and generated xcodeproj. Without it, concurrent runs clobber each other's objects.
#
# USAGE
#  export GETV_SLOT=sweep GETV_SIM="GETV Sweep"
#  ./build_sim.sh lib && ./build_sim.sh app # once, before sweeping
#  getv/tools/level_sweep.sh # all levels
#  getv/tools/level_sweep.sh 33 34 35 # just these
#  GETV_SWEEP_TIMEOUT=60 getv/tools/level_sweep.sh
#
# OUTPUT
#  $OUT/<id>-<name>.log full console capture
#  $OUT/<id>-<name>.png screenshot taken just before the process is killed
#  $OUT/results.tsv one row per level, tab-separated
#  $OUT/summary.md the same thing grouped by failure signature
#
# The LEVELID enum is sparse; it is not 0..19. BUNKER1=9, SILO=20, then a contiguous run
# 22..57, plus TITLE=90. A bogus stage number does not error -- it silently falls back to
# bg table entry 0 and then crashes somewhere unrelated, which presents as a completely
# different bug. The table below is transcribed from vendor/ge-decomp/src/bondconstants.h;
# regenerate it from there, never by guessing.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GETV="$(cd "$HERE/.." && pwd)"
BUILD_SIM="$GETV/build_sim.sh"
OUT="${GETV_SWEEP_OUT:-$GETV/build-sim-${GETV_SLOT:-sweep}/sweep}"
TIMEOUT="${GETV_SWEEP_TIMEOUT:-75}"
BUNDLE_ID="org.goldeneyenative.getv"

# id:name -- straight out of bondconstants.h. LEVELID_DEFAULT=0 and LEVELID_MAX=57 are
# included: DEFAULT is the "did the harness itself work" control, and MAX is
# the one-past-the-end sentinel, so a crash there is expected and confirms that an
# out-of-range stage is distinguishable from a real level failure.
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
# Same runtime-selection rule as build_sim.sh: take the NEWEST runtime hosting this
# device name. The first match would be tvOS 16.1, below our 17.0 deployment target,
# and the app would simply refuse to install.
sim_udid() {
  xcrun simctl list devices available \
    | awk -v n="$SIM_NAME" '
        /^-- tvOS/ { ok = 1; next }
        /^-- /     { ok = 0; next }
        ok && index($0, n) && match($0, /[0-9A-F-]{36}/) { u = substr($0, RSTART, RLENGTH) }
        END { print u }'
}
UDID="$(sim_udid)"
[ -n "$UDID" ] || { echo "no simulator matching '$SIM_NAME' -- create one and export GETV_SIM"; exit 1; }

APP="$GETV/build-sim-${GETV_SLOT:-sweep}-dd/Build/Products/Release-appletvsimulator/Goldeneye-Native.app"
[ -d "$APP" ] || { echo "no app at $APP -- run './build_sim.sh lib && ./build_sim.sh app' first"; exit 1; }

mkdir -p "$OUT"
echo "sim:   $SIM_NAME ($UDID)"
echo "app:   $APP"
echo "out:   $OUT"
echo "limit: ${TIMEOUT}s per level"

# Guarded on reparse. Re-parsing is a pure log-reading operation, and booting or
# installing here would disturb a run in progress on the same device -- exactly the wedge
# this harness is supposed to avoid.
if [ "${1:-}" != "reparse" ]; then
  xcrun simctl boot "$UDID" 2>/dev/null || true
  xcrun simctl bootstatus "$UDID" -b >/dev/null 2>&1 || true
  xcrun simctl install "$UDID" "$APP" >/dev/null 2>&1 || { echo "install failed"; exit 1; }
fi

RESULTS="$OUT/results.tsv"
printf 'id\tname\tstage_load\tframe_loop\tgfx_tasks\ttris_sub\ttris_drawn\tframes\tcoverage\tuniq\ttop1share\toutcome\tfault_pc\tfault_addr\tlast_mark\n' > "$RESULTS"

# ---- non-black screenshot coverage ----------------------------------------
# Read `coverage`, not `frames`, when deciding whether a level works. "RENDERS" is a
# frame counter, not a picture: levels have been observed counting 4 and 13 frames and
# drawing 323 and 487 triangles while their captured screenshots were 1.3% and 1.2%
# non-black -- geometry submitted that never becomes visible pixels. Reporting those as
# working is a false positive.
#
# ImageMagick's mean-luminance metric is not usable here: it reports 50% for a pure black
# image. This counts pixels directly.
# Use /usr/bin/python3 (Apple's, which has PIL), not `python3` from PATH, which on this
# Mac is a Homebrew x86_64 build with no PIL. Do not "modernise" this to `python3`.
#
# `arch -arm64` is necessary, and dropping it fails silently and wrongly. This Mac's
# shell environment is x86_64 under Rosetta, and /usr/bin/python3 is a universal shim
# that inherits the parent process's architecture, so from a Rosetta shell it comes up
# x86_64 while the installed PIL `_imaging...so` is arm64-only:
#  dlopen(...PIL/_imaging.cpython-39-darwin.so): incompatible architecture
#  (have 'arm64', need 'x86_64')
# The bare `except Exception: print("-")` then turns that into a coverage of "-" for every
# level, so the metric looks unavailable rather than broken. If this column ever comes
# back all-"-", suspect the architecture before the images.
# Validated against synthetic images: black -> 0.00, white -> 100.00, a 64-step ramp ->
# 95.31 (exactly the three sub-threshold columns). The threshold is sum(RGB) > 24.
COVERAGE_PY='
import sys
try:
    from PIL import Image
except Exception:
    print("-"); raise SystemExit
try:
    im = Image.open(sys.argv[1]).convert("RGB")
    lit = [px for px in im.getdata() if sum(px) > 24]
    n = im.width * im.height
    if not lit:
        print("0.00 0 -")
        raise SystemExit
    from collections import Counter
    c = Counter(lit)
    top1 = c.most_common(1)[0][1]
    print("%.2f %d %.1f" % (100.0 * len(lit) / n, len(c), 100.0 * top1 / len(lit)))
except Exception:
    print("- - -")
'
coverage_of() {
  local png="$1"
  [ -f "$png" ] || { echo "- - -"; return; }
  arch -arm64 /usr/bin/python3 -c "$COVERAGE_PY" "$png" 2>/dev/null || echo "- - -"
}

# Which levels to do: args if given, else all.
# `reparse` re-derives results.tsv and summary.md from the .log files already in $OUT
# without launching anything. A full set costs ~45 minutes, so a parser bug must not mean
# re-running everything: the logs are the raw data and they are kept.
SELECT=("$@")
REPARSE=0
if [ "${1:-}" = "reparse" ]; then REPARSE=1; SELECT=(); fi

run_one() {
  local id="$1" name="$2"
  local log="$OUT/$id-$name.log" png="$OUT/$id-$name.png"

  # Kill any survivor from the previous iteration BEFORE launching. A leftover process
  # holds the GL context and the next launch dies for a reason that has nothing to do
  # with its level.
  xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true

  printf '%-3s %-9s ' "$id" "$name"

  # --console-pty streams stdout and, unlike devicectl, does not wedge. It also never
  # returns on a healthy run (the frame loop runs forever), so the timeout is the normal
  # exit path, not an error.
  #
  # simctl passes the child's environment only through SIMCTL_CHILD_<NAME>. A variable
  # merely exported in this shell is silently dropped, which is indistinguishable from
  # GETV_STAGE being unimplemented. It is read in vendor/ge-decomp/src/boss.c.
  # GETV_SUPERSAMPLE=1 turns off 2x supersampling. The tvOS simulator has no GPU-backed
  # GLES (GL_RENDERER = "Apple Software Renderer"), so ss=2 costs ~2.4 s/frame against
  # ~600 ms at ss=1 -- a 4x difference that decides whether a level paints anything at
  # all inside the timeout. Default it on here; override with GETV_SUPERSAMPLE=2.
  SIMCTL_CHILD_GETV_STAGE="$id" \
  SIMCTL_CHILD_GETV_SUPERSAMPLE="${GETV_SUPERSAMPLE:-1}" \
    xcrun simctl launch --console-pty "$UDID" "$BUNDLE_ID" > "$log" 2>&1 &
  local pid=$!

  # Screenshot at 80% of the budget: late enough that a level which renders has painted
  # something, early enough that we still grab it before the kill.
  local shot_at=$(( TIMEOUT * 8 / 10 ))
  local waited=0
  local shot_done=0
  while [ "$waited" -lt "$TIMEOUT" ]; do
    kill -0 "$pid" 2>/dev/null || break
    if [ "$shot_done" = 0 ] && [ "$waited" -ge "$shot_at" ]; then
      xcrun simctl io "$UDID" screenshot --type=png "$png" >/dev/null 2>&1
      shot_done=1
    fi
    sleep 2; waited=$(( waited + 2 ))
  done
  # A process that crashed early exits before shot_at, so grab the frame anyway: on a
  # crash the last painted frame is often the most informative artifact available.
  [ "$shot_done" = 0 ] && xcrun simctl io "$UDID" screenshot --type=png "$png" >/dev/null 2>&1

  kill "$pid" 2>/dev/null
  xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  wait "$pid" 2>/dev/null

  parse_one "$id" "$name" "$log"
}

parse_one() {
  local id="$1" name="$2" log="$3"

  local stage_load=no frame_loop=no
  grep -q 'boot-> bm:lvlStageLoad'   "$log" && stage_load=yes
  # Reaching viInitBuffers means lvlStageLoad returned: the strongest single signal that
  # level data actually loaded, and the one that separates a loader failure from a
  # renderer failure.
  grep -q 'boot-> bm:viInitBuffers'  "$log" && stage_load=returned
  grep -qE 'boot-> bm:(waitForNextFrame|drain-enter|pre-recv-loop)' "$log" && frame_loop=yes

  local gfx_tasks tris_sub tris_drawn frames
  # Never write `grep -c ... || echo 0`. With zero matches grep -c prints "0" and still
  # exits 1, so the `|| echo 0` appends a second line and the variable becomes "0\n0".
  # That breaks `[ "$frames" -gt 0 ]` with "integer expected" and embeds a newline in the
  # TSV, splitting one row into two so the file looks longer than the number of levels
  # run. Use `|| true`: grep -c already prints a count.
  gfx_tasks=$(grep -c 'gfx task ' "$log" 2>/dev/null || true)
  frames=$(grep -c 'tris submitted=' "$log" 2>/dev/null || true)
  : "${gfx_tasks:=0}" "${frames:=0}"
  # Take the LAST frame's counters, not the first: frame 0 is often a clear-only frame
  # and reports zero triangles even on a level that renders fine.
  tris_sub=$(grep 'tris submitted=' "$log" | tail -1 | sed -n 's/.*submitted=\([0-9]*\).*/\1/p')
  tris_drawn=$(grep 'tris submitted=' "$log" | tail -1 | sed -n 's/.*drawn=\([0-9]*\).*/\1/p')
  : "${tris_sub:=-}" "${tris_drawn:=-}"

  local fault_pc="-" fault_addr="-"
  fault_pc=$(grep -m1 'FAULT PC:' "$log" | sed -n 's/.*FAULT PC: [^=]*= \([A-Za-z0-9_]*\) + \([0-9-]*\).*/\1+\2/p')
  [ -z "$fault_pc" ] && fault_pc=$(grep -m1 'FAULT PC:' "$log" | sed -n 's/.*FAULT PC: \(0x[0-9a-f]*\).*/\1(nosym)/p')
  : "${fault_pc:=-}"
  fault_addr=$(grep -m1 -oE 'addr[= ]0x[0-9a-f]+' "$log" | head -1 | grep -oE '0x[0-9a-f]+')
  : "${fault_addr:=-}"

  # Screenshot coverage, derived from the .png beside the .log, so `reparse` recomputes
  # it for an older output tree too. That is what makes a coverage comparison across runs
  # meaningful: both sides go through this same code path rather than two ad-hoc scripts.
  #
  # Gated on the app still being on screen, and the gate is not optional. `simctl io
  # screenshot` captures the device, not the app. Once the game has faulted the app is
  # gone and the capture is the tvOS springboard, which is bright chrome on a grey field
  # and scores 99.91% non-black on every crashed level (default, control, sho and Cradle
  # all reported exactly that). Ungated, the one column that exists to catch "counts
  # frames, paints nothing" would award the highest score in the table to levels that
  # never drew a pixel. A crashed level therefore gets "-" (unmeasurable), never a number.
  #
  # Three numbers, because coverage alone is not enough. `uniq` is the count of distinct
  # non-black colours. A level can light up 68% of the screen with a handful of untextured
  # quads and no level geometry at all: EGYPT scored 68.42% while painting three nested
  # rectangles in 16 colours, and temple, basement, stack, library, caves and complex
  # painted a byte-identical 3-colour rectangle as each other. Coverage says "not black";
  # uniq says "actually textured geometry". Real frames land in the hundreds to thousands
  # (Dam 119, Jungle 93, Surface 2451, Cuba 8608); flat fills land at 3-20. Report both.
  #
  # `top1share` is the percentage of the lit pixels that are a single colour, and it is
  # the sharpest of the three: a legitimate G_CYC_FILL sky or a stuck fade quad drives it
  # toward 100%, real geometry drives it down. On DAM across a renderer fix, coverage fell
  # 67.1% -> 64.0% while distinct rose 12 -> 725 and top1share fell 75.4% -> 25.1%: the
  # frame improved substantially while the coverage number got worse. Lead with `distinct`
  # and `top1share`, since coverage alone can move the wrong way.
  local coverage="-" uniq="-" top1="-"
  if ! grep -q 'FAULT PC:' "$log" && [ "$frame_loop" = yes ] && [ "$frames" -gt 0 ]; then
    read -r coverage uniq top1 <<< "$(coverage_of "${log%.log}.png")"
  fi
  : "${coverage:=-}" "${uniq:=-}" "${top1:=-}"

  local last_mark
  last_mark=$(grep 'last boot mark:' "$log" | tail -1 | sed -n 's/.*last boot mark: \([^ ]*\).*/\1/p')
  # No crash line means the crash handler never fired; fall back to the last mark the
  # boot trace actually printed.
  [ -z "$last_mark" ] && last_mark=$(grep -o 'boot-> [^ ]*' "$log" | tail -1 | sed 's/boot-> //')
  : "${last_mark:=NONE}"

  # Outcome, ordered most-informative first.
  #
  # SILENT-HANG is not the same as a signal crash; do not conflate them.
  # -fstack-protector turns a fixed-size-array overrun into SIGABRT via
  # __stack_chk_fail, which bypasses our crash handler entirely, and
  # mempAllocBytesInBank fails by `while(1);` -- the N64's genuine out-of-memory
  # behaviour -- which produces no signal and no final line at all. Both present as a run
  # that simply stops logging. For a SILENT-HANG, read
  # ~/Library/Logs/DiagnosticReports/Goldeneye-Native-*.ips; it is two concatenated JSON
  # documents, so json.loads each half separately.
  local outcome
  if   [ ! -s "$log" ];                             then outcome=NO-OUTPUT
  elif grep -q 'MEMP.*DIE\|GE_MEMP_DIE\|OUT OF MEM' "$log"; then outcome=OOM
  elif grep -q 'FAULT PC:' "$log";                  then outcome=CRASH
  elif [ "$frame_loop" = yes ] && [ "$frames" -gt 0 ]; then
    # RENDERS and RENDERS-BLACK are split in the outcome column rather than noted in a
    # footnote, because the grouped summary keys on outcome: a level that counts frames
    # while painting nothing must not hide inside the "paints" group and inflate the
    # headline. Threshold is 5% non-black; measured levels sit either around 1% (nothing
    # visible) or above 50% (real imagery), so nothing lands near the line.
    if awk -v c="$coverage" 'BEGIN{exit !(c+0 < 5)}' 2>/dev/null; then
      outcome=RENDERS-BLACK
    elif awk -v u="$uniq" -v t="$top1" 'BEGIN{exit !(u+0 < 32 || t+0 >= 85)}' 2>/dev/null; then
      # Lit, but not a picture. Two ways to fail, because levels fail both ways:
      #  distinct < 32 -- a handful of untextured quads (EGYPT: 16 colours at 68% lit)
      #  top1share >= 85 -- one colour owns nearly every lit pixel, i.e. a G_CYC_FILL sky
      #  or a stuck fade quad with a few stray polys on top. This
      #  catches frames that have many distinct colours yet are still
      #  essentially one flat field.
      # Kept out of RENDERS so the headline count cannot be inflated by either.
      outcome=RENDERS-FLAT
    else
      outcome=RENDERS
    fi
  elif [ "$frame_loop" = yes ];                     then outcome=LOOP-NO-FRAMES
  else outcome=SILENT-HANG; fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$id" "$name" "$stage_load" "$frame_loop" "$gfx_tasks" "$tris_sub" \
    "$tris_drawn" "$frames" "$coverage" "$uniq" "$top1" "$outcome" "$fault_pc" \
    "$fault_addr" "$last_mark" >> "$RESULTS"

  printf '%-13s f=%-4s cov=%-7s distinct=%-6s top1=%-6s tris=%-7s %s\n' "$outcome" "$frames" "$coverage" "$uniq" "$top1" "$tris_sub" \
    "$([ "$fault_pc" != - ] && echo "@ $fault_pc" || echo "mark=$last_mark")"
}

for entry in "${LEVELS[@]}"; do
  id="${entry%%:*}"; name="${entry##*:}"
  if [ "$REPARSE" = 1 ]; then
    [ -f "$OUT/$id-$name.log" ] || continue
    printf '%-3s %-9s ' "$id" "$name"
    parse_one "$id" "$name" "$OUT/$id-$name.log"
    continue
  fi
  if [ ${#SELECT[@]} -gt 0 ]; then
    keep=0
    for s in "${SELECT[@]}"; do [ "$s" = "$id" ] && keep=1; done
    [ "$keep" = 1 ] || continue
  fi
  run_one "$id" "$name"
done

# ---- grouped summary -------------------------------------------------------
# Grouping by shared failure signature is the point of the sweep: one fix that clears a
# signature clears every level under it, so the group sizes are the priority order.
# Signature = outcome + faulting symbol. The offset is dropped because the same bug
# reached by two call paths lands a few instructions apart and must not split into two
# groups.
{
  echo "# Level sweep -- $(date '+%Y-%m-%d %H:%M')"
  echo
  echo "sim: \`$SIM_NAME\`  timeout: ${TIMEOUT}s  levels: $(( $(wc -l < "$RESULTS") - 1 ))"
  echo
  echo '| id | level | stageLoad | loop | gfx tasks | tris sub | tris drawn | frames | non-black % | distinct | top1share | outcome | fault | last mark |'
  echo '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
  tail -n +2 "$RESULTS" | awk -F'\t' \
    '{printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | **%s** | `%s` | `%s` |\n",
      $1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$15}'
  echo
  echo '## Grouped by shared failure signature'
  echo
  tail -n +2 "$RESULTS" | awk -F'\t' '{
      sym = $13; sub(/\+.*/, "", sym);
      # A fault PC with no symbol is a jump through a corrupt function pointer: the PC is
      # not in any function, so there is nothing to name it. Each such crash carries a
      # different garbage address, so keying on the address would split one bug into N
      # groups of 1 and hide the largest signature in the table. Collapse them.
      if (sym ~ /^0x.*\(nosym\)$/) sym = "CORRUPT-FNPTR";
      key = $12 (sym == "-" ? " @ " $15 : " @ " sym);
      list[key] = list[key] " " $2; n[key]++
    }
    END { for (k in list) printf "%d\t%s\t%s\n", n[k], k, list[k] }' \
    | sort -rn | awk -F'\t' '{printf "- **%s** (%s levels):%s\n", $2, $1, $3}'
} > "$OUT/summary.md"

echo
echo "results -> $RESULTS"
echo "summary -> $OUT/summary.md"
echo
cat "$OUT/summary.md"
