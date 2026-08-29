#!/bin/bash
# Double-click this in Finder to install GoldenEye 007.
#
# A .command file is the macOS equivalent of the Windows setup wizard: Finder opens it in
# Terminal and runs it, so nobody has to know what a terminal is or how to reach this folder
# from one. It exists because "open Terminal, cd to the folder you unzipped, type bash
# tools/install.sh" was the first instruction in the README, and that sentence loses most
# people before they have done anything at all.
#
# It does nothing tools/install.sh does not. The value is entirely that it is double-clickable.
set -u

# Finder starts a .command in the user's home directory, not beside the file, so every relative
# path in the installer would resolve somewhere wrong. Anchor to this script's own location.
cd "$(dirname "${BASH_SOURCE[0]}")" || {
    echo "Could not find the project folder. Move this file back beside tools/ and try again."
    read -r -p "Press Return to close."
    exit 1
}

if [ ! -f tools/install.sh ]; then
    echo "This file has been moved away from the rest of the project."
    echo
    echo "It needs to sit in the same folder as tools/, docs/ and getv/. Move it back and"
    echo "double-click it again."
    read -r -p "Press Return to close."
    exit 1
fi

# Only when there is a terminal to clear; harmless but noisy otherwise.
[ -t 1 ] && clear
cat <<'BANNER'
  GoldenEye 007, native build

  This will set up and build the game. It takes 10 to 40 minutes the first time,
  mostly reading your own copy of the cartridge. You can leave it running.

  You need your own GoldenEye 007 ROM. Leave it on the Desktop or in Downloads
  and it will be found automatically.

BANNER
read -r -p "  Press Return to start, or close this window to cancel."
echo

bash tools/install.sh
rc=$?

echo
if [ "$rc" -eq 0 ]; then
    echo "  Finished. To play, double-click 'Play GoldenEye.command' in this folder."
else
    echo "  It stopped, and the reason is in the messages above. Nothing is broken:"
    echo "  double-clicking this again picks up where it left off."
fi
# Without this the Terminal window closes on exit and takes the explanation with it, which is
# the one thing a person who hit an error most needs to read.
read -r -p "  Press Return to close."
