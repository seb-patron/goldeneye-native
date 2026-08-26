# Packaging

Icons for all three desktop platforms come from one source image and one script.

    tools/make_icons.sh

It reads `assets/icon/goldeneye-plus-transparent.png` and writes:

| Output | Used by |
| --- | --- |
| `assets/icon/goldeneye-plus.icns` | the macOS bundle |
| `assets/icon/goldeneye-plus.ico` | the Windows executable and its shortcuts |
| `assets/icon/hicolor/<size>/apps/goldeneye-plus.png` | the Linux icon theme |
| `getv/port/src/ge_icon.h` | the running window, on every platform |

Every output is committed, so a normal build needs none of this -- run the script only after
changing the source image. It needs ImageMagick, and `iconutil` for the `.icns` (macOS only; on
other systems it leaves the `.iconset` directory behind for a Mac to finish).

The window icon is compiled in rather than loaded from disk. SDL2 without SDL_image cannot read a
PNG, and a build people copy between machines should not lose its icon to a missing file.

## Linux

Install the theme icons under `/usr/share/icons/hicolor/` mirroring the directory layout, and
`goldeneye-plus.desktop` under `/usr/share/applications/`, then:

    gtk-update-icon-cache /usr/share/icons/hicolor

## Windows

`goldeneye-plus.ico` is the resource to attach to the built executable, which also gives Explorer
shortcuts and the taskbar button the right image.

## macOS

`goldeneye-plus.icns` is the `CFBundleIconFile` for a `.app` bundle. The bare binary the build
produces today has no bundle, and takes its dock icon from the window instead.
