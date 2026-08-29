#!/bin/bash
# Double-click this in Finder to play, once the install has finished.
#
# Same reasoning as "Install on Mac.command": the built binary sits at
# getv/build-mac/goldeneye, and telling someone to reach it from a terminal is a worse
# experience than an icon they can double-click, or drag to the Dock and keep.
set -u

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

BIN="getv/build-mac/goldeneye"

if [ ! -x "$BIN" ]; then
    [ -t 1 ] && clear
    echo "  The game has not been built yet."
    echo
    echo "  Double-click 'Install on Mac.command' in this folder first. It takes 10 to 40"
    echo "  minutes the first time and only has to be done once."
    read -r -p "  Press Return to close."
    exit 1
fi

# --launcher opens the settings window rather than starting a level immediately, which is the
# right default for a double-click: someone who has just installed it wants to choose a level
# and look at the video options, not be dropped straight into whatever ran last.
exec "$BIN" --launcher
