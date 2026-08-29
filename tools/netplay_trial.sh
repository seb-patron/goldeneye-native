#!/usr/bin/env bash
# Run one host and one joiner as two real processes and report whether they stayed in sync.
#
# Lockstep fails silently by design: two machines apply the same inputs, diverge, and nothing
# says so until the players are visibly standing in different places. ge_net.c already carries
# the detector -- every machine periodically publishes a fingerprint of its RNG state for a
# tick, and a peer that reaches the same tick with a different value counts a desync. This just
# drives that from outside and counts trials.
#
# Neither process is given any input. Two runs of the same binary on the same machine with no
# input MUST agree; if they do not, the divergence is ours and not the network's.
#
#   bash tools/netplay_trial.sh [trials] [frames]
#
# Env passed through to both processes, so a trial can be run under any profile:
#   GETV_REALCLOCK, GETV_STAGE, GETV_MP, GETV_NET_DELAY, and anything else the game reads.
set -u

TRIALS="${1:-5}"
FRAMES="${2:-601}"
BIN="${GETV_BIN:-./getv/build-mac/goldeneye}"
# A hard ceiling per process. gePortNetTick stalls the whole tick/render pass while it waits on
# a peer, so a process whose peer never arrives never advances a frame, never reaches
# GETV_EXIT_FRAME, and never exits. Without this the harness hangs instead of reporting.
LIMIT="${GETV_TRIAL_TIMEOUT:-180}"
PORT_BASE="${GETV_NET_PORT:-38700}"
OUT="${TMPDIR:-/tmp}/getv-netplay-trial.$$"

[ -x "$BIN" ] || { echo "no binary at $BIN (set GETV_BIN)"; exit 1; }
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

# A stage both players can actually load. The MP-only arenas have no solo setup at all, so a
# trial on a solo stage measures two half-initialised games rather than a session.
STAGE="${GETV_STAGE:-45}"

echo "binary   $BIN"
echo "stage    $STAGE   frames $FRAMES   trials $TRIALS"
echo "clock    GETV_REALCLOCK=${GETV_REALCLOCK:-unset}"
echo

agree=0; desync=0; nosession=0; hangs=0

for t in $(seq 1 "$TRIALS"); do
    port=$((PORT_BASE + t))
    h="$OUT/host.$t.log"; j="$OUT/join.$t.log"

    GETV_SEEDTRACE=1 GETV_NET_HOST="$port" GETV_NET_PLAYERS=2 GETV_STAGE="$STAGE" GETV_MP="${GETV_MP:-2}" \
        GETV_EXIT_FRAME="$FRAMES" timeout "$LIMIT" "$BIN" >"$h" 2>&1 &
    hp=$!
    # The joiner retries its JOIN, so it does not need the host to be listening first; a short
    # stagger just keeps the logs readable and avoids both binding at the same instant.
    GETV_SEEDTRACE=1 GETV_NET_JOIN="127.0.0.1:$port" GETV_STAGE="$STAGE" GETV_MP="${GETV_MP:-2}" \
        GETV_EXIT_FRAME="$FRAMES" timeout "$LIMIT" "$BIN" >"$j" 2>&1 &
    jp=$!

    wait "$hp" 2>/dev/null; hrc=$?
    wait "$jp" 2>/dev/null; jrc=$?
    # 124 is timeout's own exit code. A trial that had to be killed is a hang, and a hang is a
    # distinct outcome from a desync -- conflating them would hide whichever is rarer.
    hung=0; { [ "$hrc" = 124 ] || [ "$jrc" = 124 ]; } && hung=1

    # Three outcomes, kept apart on purpose. A trial where the session never opened is not
    # evidence of agreement, and counting it as a pass is how a broken harness reports success.
    if [ "$hung" = 1 ]; then
        hangs=$((hangs + 1)); verdict="HUNG"
    elif ! grep -q 'session open' "$h" 2>/dev/null || ! grep -q 'session open' "$j" 2>/dev/null; then
        nosession=$((nosession + 1)); verdict="NO SESSION"
    elif grep -qi 'desync' "$h" "$j" 2>/dev/null; then
        desync=$((desync + 1)); verdict="DESYNC"
    else
        agree=$((agree + 1)); verdict="agree"
    fi

    # The seed each side booted with, so a desync can be attributed rather than just counted.
    hs=$(grep -oE 'randomSetSeed\([0-9]+\)' "$h" 2>/dev/null | head -1)
    js=$(grep -oE 'randomSetSeed\([0-9]+\)' "$j" 2>/dev/null | head -1)
    printf '  trial %-3s %-11s host %-24s join %s\n' "$t" "$verdict" "${hs:-seed?}" "${js:-seed?}"
done

echo
echo "  agree $agree   desync $desync   no-session $nosession   hung $hangs   of $TRIALS"
[ "$nosession" -gt 0 ] && echo "  a no-session trial measures nothing; fix that before reading the rest"
[ "$desync" -eq 0 ] && [ "$nosession" -eq 0 ] && [ "$hangs" -eq 0 ]
