# FAQ

### Can I play GoldenEye 007 on PC, Mac or Linux without an emulator?

Yes. That is what this is. The game's C source is compiled directly for your machine. You
supply your own ROM; the build extracts the assets from it once, and what you run afterwards
is a native executable.

### What platforms does it run on?

macOS on Apple Silicon and Intel, Linux on x86-64, and Windows. One source tree, one build
script each.

### Does it support mouse and keyboard?

Yes, and it is the default. Mouse look with adjustable sensitivity and Y-invert, WASD
movement, left button fires, right aims, ESC releases the cursor. A gamepad works alongside
it rather than instead of it.

### Does it run at 60fps? Can I run it higher?

It renders at 60 by default at whatever resolution you set. Going above 60 is not simply a
setting in this game: GoldenEye counts many things per frame rather than per second, so a
faster loop makes weapons fire faster and guards react sooner. `framerate = 30` runs the
simulation at the cadence the game was authored for. See [Frame timing](Frame-timing).

### Is there widescreen?

Resolution and aspect are configurable. Whether the widescreen path widens the field of view
or stretches a 4:3 image has not been measured, and the roadmap says so rather than claiming
either.

### Can I mod it?

Three ways. Lua scripts in `mods/` with hooks for frame, spawn and weapon fire. Roughly 275
`GETV_*` behaviour gates. And `goldeneye.cfg`. Texture replacement is a roadmap item, not
implemented.

### Is there online or LAN multiplayer?

No. Split-screen multiplayer works, with all 64 characters. Networked play is on the roadmap
and is downstream of the frame-timing work, because netcode over a variable simulation rate
is not worth attempting.

### Can two people play the single-player campaign?

Partly, and it is alpha. Two to four players spawn into a solo mission with its own geometry,
props and objectives, and every viewport renders. They do not move yet. Objectives, AI and
cutscenes are also authored around one Bond.

### Do I need the ROM?

Yes, your own copy. Nothing in this repository contains game data, and nothing in it will
help you find any.

### Is this legal?

The code here is a port of a public decompilation plus an original platform layer. The game
data is yours and stays yours. See [Provenance](Provenance).

### How is this different from a recompilation?

A static recompiler translates the original machine code into something a modern CPU runs.
The result works but is not readable or editable. This is built from a decompilation: real C,
which can be changed. That is the whole reason the frame-timing problem is fixable here.

### Why not just use an emulator?

Use one if it suits you. The difference is what you can change. An emulator runs the retail
ROM and can only patch it from outside; problems baked into the game's own logic stay baked
in. Here they are ordinary C.
