# Frequently asked

Questions that come up often enough to be worth a page. The short version of most of them is on
the [front page](../README.md); this is where the longer answers live.


**Can I play GoldenEye 007 on a modern PC, Mac or Linux machine without an emulator?**
Yes. That's the whole idea. You supply your own ROM, the build extracts the assets from it, and
what you run afterwards is a native executable.

**Does it support mouse and keyboard?** Yes, and it's the default. Mouse look with sensitivity
and Y-invert, WASD to move, ESC to let go of the cursor.

**Does it run at 60fps? Can it go higher?** It renders at 60 out of the box and the resolution
is arbitrary. Above 60 the game's per-frame systems run faster than they were tuned for — see
the section above. `framerate = 30` is the faithful setting.

**Is there widescreen?** Resolution is fully configurable, but here's the measured truth:
**changing the window aspect does not widen the field of view.** The renderer fits the 4:3 view
to whatever window you give it. Use `fov` if you want to genuinely see more; that's the setting
that does it. Proper aspect-aware widescreen is still a roadmap item.

**Can I mod it?** Three ways. Lua scripts in `mods/`, roughly 275 `GETV_*` behaviour gates, and
`goldeneye.cfg`. Texture replacement is on the roadmap and not implemented.

**Is multiplayer online?** No. Split-screen works. LAN and online sit downstream of the
frame-timing work; there's a lockstep session in the tree and an honest argument about whether
it can work at all, [below](#whats-half-built).

**Can two people play the single-player missions?** Partly, and it's alpha. Two to four players
spawn into a solo mission with its own geometry, props and objectives, and every viewport
renders. They don't move yet. Details below.

**Do I need the ROM?** Yes, your own copy. Nothing in this repository contains game data.

**Is this the Xbox 360 remaster, or the cancelled XBLA build?** Neither. Those are separate
codebases. This is the Nintendo 64 game, from its decompilation.

**How is this different from a source port like Ship of Harkinian?** Same idea, different game.
A decompilation gets turned into a native program with a modern platform layer. This one is
GoldenEye.

**Does it need Wine, Proton, WSL or a compatibility layer?** No. Every platform gets a real
native binary. Windows uses mingw-w64 with no MSYS2, Cygwin or WSL anywhere near it.

**What about split-screen on one PC?** Works, with all 64 characters, the radar and the full
multiplayer setup.

**Can I use a PS5 or Xbox controller?** Yes. Input goes through SDL2's game controller layer, so
a DualSense, DualShock 4, Xbox pad, Switch Pro controller or 8BitDo is recognised without a
mapping file, wired or wireless. Both analogue sticks are live, which the N64 controller could
never do.

**Is there a crouch button?** Yes, and the original doesn't have one. Retail crouch means
holding aim, pushing down, then releasing aim while staying low. Here it's C or LSHIFT, with V
to stand, and the old gesture still works if you're attached to it.

**Does it support gamepads?** Yes, through SDL2, including wireless pads your OS has already
paired. Keyboard and mouse are the default and both work at once.

**Can it run at 4K?** Internal resolution is arbitrary and supersampling is there. Whether
that's a good idea on the frame-timing front is covered above.

**Is the source available?** All of it. That's the point.

