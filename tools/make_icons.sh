#!/bin/sh
# Build every platform's launcher icon from one source PNG.
#
# The transparent source is the one that matters: a Mac dock icon, a Windows taskbar button and a
# Linux hicolor theme all composite against backgrounds we do not choose, and an opaque square
# reads as a sticker rather than an app. The opaque file is kept alongside for anywhere a flat
# image is wanted (store art, README, social cards).
#
# Outputs, all committed so a build does not need ImageMagick:
#   assets/icon/goldeneye-plus.icns              macOS bundle
#   assets/icon/goldeneye-plus.ico               Windows executable and shortcut
#   assets/icon/hicolor/<size>/goldeneye-plus.png    Linux icon theme
#   getv/port/src/ge_icon.h                      the running window's icon, via SDL_SetWindowIcon
#
# Usage: tools/make_icons.sh [source.png]
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-$ROOT/assets/icon/goldeneye-plus-transparent.png}
OUT=$ROOT/assets/icon

[ -f "$SRC" ] || { echo "no source icon at $SRC" >&2; exit 1; }
command -v magick >/dev/null 2>&1 || { echo "ImageMagick (magick) is required" >&2; exit 1; }

echo "source: $SRC"

# Linux: the sizes a hicolor theme actually looks for.
for s in 16 24 32 48 64 128 256 512; do
    mkdir -p "$OUT/hicolor/${s}x${s}/apps"
    magick "$SRC" -resize ${s}x${s} "$OUT/hicolor/${s}x${s}/apps/goldeneye-plus.png"
done
echo "linux:   hicolor 16 through 512"

# Windows: one .ico carrying the sizes Explorer picks between.
magick "$SRC" -define icon:auto-resize=256,128,64,48,32,16 "$OUT/goldeneye-plus.ico"
echo "windows: $(basename "$OUT/goldeneye-plus.ico")"

# macOS: iconutil wants a .iconset directory laid out by name, including the @2x variants.
ICONSET=$OUT/goldeneye-plus.iconset
rm -rf "$ICONSET"; mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    magick "$SRC" -resize ${s}x${s}       "$ICONSET/icon_${s}x${s}.png"
    magick "$SRC" -resize $((s*2))x$((s*2)) "$ICONSET/icon_${s}x${s}@2x.png"
done
if command -v iconutil >/dev/null 2>&1; then
    iconutil -c icns "$ICONSET" -o "$OUT/goldeneye-plus.icns"
    rm -rf "$ICONSET"
    echo "macos:   $(basename "$OUT/goldeneye-plus.icns")"
else
    echo "macos:   iconutil not present, left $ICONSET for a Mac to finish"
fi

# The running window. SDL2 with no image library can only be handed raw pixels, so the icon
# travels as an array rather than as a file -- which also means the built binary carries its own
# icon and cannot lose it to a missing asset path.
magick "$SRC" -resize 128x128 -depth 8 RGBA:"$OUT/.icon128.rgba"
python3 "$ROOT/tools/rgba_to_header.py" "$OUT/.icon128.rgba" 128 "$ROOT/getv/port/src/ge_icon.h"
rm -f "$OUT/.icon128.rgba"
echo "window:  getv/port/src/ge_icon.h"
