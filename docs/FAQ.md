# Frequently asked

Questions that come up often enough to be worth a page. The short answers are on the
[front page](../README.md); these are the longer ones.

## Is this an emulator?

No, and the difference is the whole point. An emulator runs the retail ROM by pretending to be
an N64, which works but cannot change the game, because the game inside it is compiled MIPS
machine code. This is built from the [decompilation](https://github.com/n64decomp/007): the game
as editable C, compiled to a normal executable for your operating system. No core to configure,
no plugin to pick, no ROM loaded at runtime.

Longer version, including why the renderer is the easy part:
[front page](../README.md#why-this-isnt-an-emulator).

## Is this the Xbox 360 remaster, or the cancelled XBLA build?

Neither. Those are separate codebases. This is the Nintendo 64 game, from its decompilation.

## How is this different from a source port like Ship of Harkinian?

Same idea, different game. A decompilation gets turned into a native program with a modern
platform layer. Ship of Harkinian is Ocarina of Time; this one is GoldenEye. Their work proved
the shape of this years before I started, and it is credited on the front page.

## Do I need the ROM?

Yes, your own legal copy of the NTSC (US) cartridge. Nothing in this repository contains game
data and nothing ever will: every texture, model, animation, sound bank and level layout is read
out of your dump at build time and emitted as C. The expected size and SHA-1 are in
[the ROM section](../README.md#the-rom).

The build reads it once. What you run afterwards never touches it.

## Does it support mouse and keyboard, or a controller?

Both, at the same time, and mouse and keyboard is the default. Mouse look with sensitivity and
Y-invert, WASD to move, ESC to let go of the cursor.

Controllers go through SDL2's game controller layer, so a DualSense, DualShock 4, Xbox pad,
Switch Pro controller or 8BitDo is recognised without a mapping file, wired or wireless. Both
analogue sticks are live, which the N64 controller could never do, and bindings are per player.

A pad works alongside the keyboard rather than replacing it: whichever input is being held wins,
so plugging one in never takes the keyboard away. See [`MOUSE.md`](MOUSE.md) for the mouse
implementation.

## Is there a crouch button?

Yes, and the original doesn't have one. Retail crouch means holding aim, pushing down, then
releasing aim while staying low. Here it's `C` or `L Shift`, with `V` to stand. The original
gesture still works if you're attached to it.

## Does it run at 60fps? Can it go higher?

It renders at 60 out of the box and the resolution is arbitrary.

Above 60 you need `GETV_REALCLOCK=1`, because the game's per-frame systems run faster than they
were tuned for and the default clock counts a rendered frame as a video field. The game warns
you if you forget. That path is reasoned from the code and has never been measured on real
high-refresh hardware.

`framerate = 30` is the faithful setting. The whole story is in
[`FRAME_TIMING.md`](FRAME_TIMING.md), and the short version is on the
[front page](../README.md#the-frame-rate-problem).

## Can it run at 4K?

Internal resolution is arbitrary and supersampling is available, so yes. Whether it is a good
idea depends on the frame-timing answer above.

## Is there widescreen?

Resolution is fully configurable, but here is the measured truth: **changing the window aspect
does not widen the field of view.** The renderer fits the 4:3 view to whatever window you give
it.

Use `fov` if you want to genuinely see more. That is the setting that changes what you can see.
Real aspect-aware widescreen is a roadmap item and is not done.

## Can I mod it?

Three ways, none of which need a rebuild: Lua scripts in `mods/`, roughly 275 `GETV_*` behaviour
gates, and `goldeneye.cfg`. Start at [`MODDING.md`](MODDING.md).

Texture replacement is on the roadmap and not implemented.

## Is multiplayer online?

No. Split-screen multiplayer works, with all 64 characters, the radar and the full multiplayer
setup.

LAN and online sit downstream of the frame-timing work. A lockstep session exists in the tree,
and whether it can work at all is honestly disputed here: [`NETPLAY.md`](NETPLAY.md) argues for
it and [`PLAYER_API.md`](PLAYER_API.md) argues against it from measurements. Both documents are
kept.

## Can two people play the single-player missions?

Partly, and it is alpha. Two to four players spawn into a solo mission with its own geometry,
props and objectives, and every viewport renders. Then they stand there: none of them moves yet.

The limit is measured rather than guessed, and [`COOP.md`](COOP.md) has the detail.

## Does it need Wine, Proton, WSL or a compatibility layer?

No. Every platform gets a real native binary. Windows uses mingw-w64, with no MSYS2, Cygwin or
WSL anywhere near it.

## Is the source available?

All of it. That's the point. What is *not* mine to license is spelled out in
[`LICENSING.md`](LICENSING.md), and the unresolved parts are named rather than glossed.
