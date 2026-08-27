#!/bin/sh
# Build the tvOS and iOS app icons from the same source used for Mac/Windows/Linux
# (assets/icon/goldeneye-plus-transparent.png, assets/icon/goldeneye-plus.png -- see
# make_icons.sh's header for why the transparent one is the canonical source).
#
# tvOS's icon format is a layered "brandassets" catalog (parallax front/middle/back per
# size, plus wide promotional Top Shelf images) rather than a single PNG, so this only
# overwrites the PNGs already referenced by getv/Sources/Assets.xcassets's existing
# Contents.json files -- filenames and dimensions are unchanged, only pixel content is.
# iOS gets a single 1024x1024 AppIcon.appiconset (Xcode 14+ single-size icon), created
# fresh since no iOS icon existed before this.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TRANSPARENT="$ROOT/assets/icon/goldeneye-plus-transparent.png"
OPAQUE="$ROOT/assets/icon/goldeneye-plus.png"
XCASSETS="$ROOT/getv/Sources/Assets.xcassets"
BRAND="$XCASSETS/App Icon & Top Shelf Image.brandassets"

[ -f "$TRANSPARENT" ] || { echo "missing $TRANSPARENT" >&2; exit 1; }
[ -f "$OPAQUE" ] || { echo "missing $OPAQUE" >&2; exit 1; }
command -v magick >/dev/null 2>&1 || { echo "ImageMagick (magick) is required" >&2; exit 1; }

# front_on_transparent SRC WxH OUT -- the ring, aspect-preserved, centred on a fully
# transparent canvas of the exact target size. Used for every icon layer's Front.
front_on_transparent() {
  magick "$TRANSPARENT" -resize "x$2" -gravity center -background none -extent "$1" "$3"
}

# solid_black WxH OUT -- an opaque black rectangle. Used for every icon layer's Back.
solid_black() {
  magick -size "$1" xc:"#08090b" "$2"
}

# front_on_black SRC WxH OUT -- the ring, aspect-preserved, centred on a SOLID black
# canvas. Used for the Top Shelf images, which have no separate Back layer to show
# through the way the App Icon imagestacks do -- left transparent, the ring would float
# on whatever backdrop tvOS's home screen happens to have that day, not the icon's own
# dark theme.
front_on_black() {
  magick "$TRANSPARENT" -resize "x$2" -gravity center -background "#08090b" -extent "$1" "$3"
}

echo "tvOS: App Icon.imagestack (400x240 / 800x480)"
front_on_transparent 400x240 240   "$BRAND/App Icon.imagestack/Front.imagestacklayer/Content.imageset/front.png"
front_on_transparent 800x480 480   "$BRAND/App Icon.imagestack/Front.imagestacklayer/Content.imageset/front@2x.png"
solid_black 400x240                "$BRAND/App Icon.imagestack/Back.imagestacklayer/Content.imageset/back.png"
solid_black 800x480                "$BRAND/App Icon.imagestack/Back.imagestacklayer/Content.imageset/back@2x.png"
magick -size 400x240 xc:none       "$BRAND/App Icon.imagestack/Middle.imagestacklayer/Content.imageset/middle.png"
magick -size 800x480 xc:none       "$BRAND/App Icon.imagestack/Middle.imagestacklayer/Content.imageset/middle@2x.png"

echo "tvOS: App Icon - App Store.imagestack (1280x768)"
front_on_transparent 1280x768 768  "$BRAND/App Icon - App Store.imagestack/Front.imagestacklayer/Content.imageset/front.png"
solid_black 1280x768                "$BRAND/App Icon - App Store.imagestack/Back.imagestacklayer/Content.imageset/back.png"
magick -size 1280x768 xc:none       "$BRAND/App Icon - App Store.imagestack/Middle.imagestacklayer/Content.imageset/middle.png"

echo "tvOS: Top Shelf Image (1920x720 / 3840x1440)"
front_on_black 1920x720 720   "$BRAND/Top Shelf Image.imageset/topshelf.png"
front_on_black 3840x1440 1440 "$BRAND/Top Shelf Image.imageset/topshelf@2x.png"

echo "tvOS: Top Shelf Image Wide (2320x720 / 4640x1440)"
front_on_black 2320x720 720   "$BRAND/Top Shelf Image Wide.imageset/topshelf.png"
front_on_black 4640x1440 1440 "$BRAND/Top Shelf Image Wide.imageset/topshelf@2x.png"

echo "iOS: AppIcon.appiconset (1024x1024, single-size)"
IOSSET="$XCASSETS/AppIcon.appiconset"
mkdir -p "$IOSSET"
magick "$OPAQUE" -resize 1024x1024 "$IOSSET/icon-1024.png"
cat > "$IOSSET/Contents.json" <<'EOF'
{
  "images" : [
    {
      "filename" : "icon-1024.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
EOF

echo "done"
