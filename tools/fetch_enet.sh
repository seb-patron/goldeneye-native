#!/usr/bin/env bash
# Fetch ENet for the netplay transport (getv/port/src/ge_net_enet.c).
#
# ENet is not vendored into this repository, for the same reason nothing else third-party is:
# getv/port/thirdparty/ is gitignored and upstream source is fetched on demand, so the tree
# never carries code it did not write. See docs/THIRD_PARTY.md for the wider arrangement.
#
# WHY ENET RATHER THAN THE HAND-ROLLED UDP IN ge_net_udp.c
#
# ge_net_udp.c works, but it reimplements things ENet has done since 2002 and does better:
# connection establishment and teardown, timeouts and disconnection detection, sequencing,
# fragmentation of anything over the MTU, and per-channel reliability. None of that is where
# the interesting problems in this project live, and every line of it is a line that can be
# subtly wrong on a link nobody tested.
#
# What ENet does NOT replace is ge_net.c. Deciding when a tick is ready, when to stall and when
# the machines have diverged is lockstep logic, not transport, and it stays ours.
#
# Licence: MIT (LICENSE in the fetched tree, Copyright (c) 2002-2024 Lee Salzman). Compatible
# with this project's position in docs/LICENSING.md.
#
# Without it, everything still builds: the build detects the absence and keeps the hand-rolled
# UDP transport. ENet is an upgrade, never a prerequisite.
set -euo pipefail

# Pinned. A moving target is how a build starts failing for reasons nobody changed.
ENET_URL="https://github.com/lsalzman/enet.git"
ENET_COMMIT="5a9c537"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dst="$here/getv/port/thirdparty/enet"

if [ -d "$dst/.git" ]; then
  echo "enet already present at $dst"
  git -C "$dst" fetch --depth 1 origin "$ENET_COMMIT" 2>/dev/null || true
  git -C "$dst" checkout -q "$ENET_COMMIT" 2>/dev/null || true
else
  mkdir -p "$(dirname "$dst")"
  git clone "$ENET_URL" "$dst"
  git -C "$dst" checkout -q "$ENET_COMMIT"
fi

echo "enet at $(git -C "$dst" rev-parse --short HEAD) -- $dst"
echo "licence: $(head -1 "$dst/LICENSE")"
