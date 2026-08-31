# Provenance and licensing

Read this before redistributing anything.

## What the MIT licence covers

`LICENSE` is MIT and it covers the original work in this repository: the platform layer under
`getv/port/` excluding the fetched third-party sources, the build scripts, `tools/`, and the
documentation. `NOTICE` states the scope precisely.

It does not and cannot cover anything else.

## What it does not cover

**The Fast3D renderer, in `getv/port/fast3d/`.** It descends from sm64ex's copy of
`Emill/n64-fast3d-engine`, and its licence status is **unresolved**. The notice sm64ex ships is
the pre-2021 form, whose second condition bans binary redistribution outright. Do not assume it
is MIT. This is why those files are fetched from a pinned upstream commit rather than vendored
into this repository, and why the port's changes to them live as a patch.

**The decompilation.** `n64decomp/007` has no licence file, and its libultra sources carry SGI
proprietary headers. That is upstream's situation rather than something this port created, but
it is a fact about the base this is built on.

**The ROM, the extracted assets, and anything derived from them.** Never distributable, under
any licence, in any form. Every texture, model, animation, sound bank and level layout in a
build came out of somebody's own cartridge dump.

## The rules that follow from that

- No ROM, no extracted assets and no game data are in this repository, and none ever will be.
- `.gitignore` blocks `roms/`, every `*.z64` / `*.n64` / `*.v64` / `*.elf`, all `getv/build-*`
  directories because they contain object files compiled from extracted ROM data, frame
  captures, and `vendor/` and `deps/` themselves. Do not defeat those rules.
- Prebuilt binaries are not a thing that can be handed out, because a linked binary contains
  the assets.

## Per-directory origin

[`getv/port/PROVENANCE.md`](https://github.com/seb-patron/goldeneye-native/blob/main/getv/port/PROVENANCE.md)
records where every part of the platform layer came from, directory by directory. It is the file
to read before copying anything out of this project into another one.

[`docs/THIRD_PARTY.md`](https://github.com/seb-patron/goldeneye-native/blob/main/docs/THIRD_PARTY.md)
covers the fifteen fetched files: what they are, where they come from, and why they are not
vendored.

[`docs/LICENSING.md`](https://github.com/seb-patron/goldeneye-native/blob/main/docs/LICENSING.md)
covers where every part came from and which terms are settled.

## Not affiliated

This project is not affiliated with, endorsed by, or connected to Nintendo, Rare, MGM, Danjaq or
EON Productions. GoldenEye 007 was made by Rare.
