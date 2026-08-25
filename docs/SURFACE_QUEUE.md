# Surface agent: next queue

The post-process pass landed and is merged: FXAA and CRT now run on macOS and Linux too,
measured on both. Two portability fixes were needed in `ge_launcher.cpp` on the way --
`struct stat` without `<sys/stat.h>` and `self_path` used 280 lines above its definition.
MinGW forgives both and Clang does not, so **build-tests one platform, ships three**.

**Before starting: merge `mac-work`.** It carries the post-process merge, the RGBA16 and
RGBA32 colour probes, `GETV_IMPACT`, and the Linux compiler-selection fix.

---

## 1. The player API -- the headline item

This is the hook that **three separate features all need**, which is why it comes before any
of them rather than being built three times:

- **bots** need to drive a player slot
- **an external AI** needs to drive a player slot and see what it is doing
- **LAN/WAN netplay** needs to serialise exactly the same thing across a wire

So build the seam once, in these two halves:

**Input injection, per player slot.** Most of this exists and is not yet a real interface.
`GETV_SCRIPT` already injects `A B X Y START BACK Z L R DU DD DL DR CU CD CL CR` plus
`SX=`/`SY=` at a chosen frame, and `port_os.c` already carries four player slots and a
per-action binding table. What is missing is a **callable** entry point -- something a bot,
a socket or a Lua mod can hand a controller state to for player n on a given tick, rather
than a string parsed at startup.

**State readout.** Also mostly present and scattered across `GETV_STATE`, `GETV_GUN_DEBUG`
and `GETV_CULLSTAT` as printf output. Wanted: the same facts returned as data -- position,
angle, health, armour, current weapon, ammo, and the visible-character list.

⚠️ **Design it against a real consumer, exactly like the post-process pass.** Take one bot
that walks to a pad and fires, and let its needs decide the shape. An API designed in the
abstract will be wrong in ways nobody notices until the third consumer.

⚠️ **Tick-accurate, not wall-clock.** Input must be applied on a numbered simulation tick.
This is the property netplay is built on, and it is cheap now and expensive to retrofit.

## 2. Bots -- reuse GoldenEye's own AI first

🔑 **Before mining the Perfect Dark port for a bot system: GoldenEye already has one.**
`chr.c` and `chraction.c` are a complete guard AI with a documented opcode list, and the
game already runs it for every guard in every level. A bot is much closer to "run the
existing AI on a player slot" than to "port PD's bots", and the AI is already written
against this game's own stan, pads and weapons.

Mine PD for what GoldenEye genuinely lacks -- difficulty tuning, bot personalities, the
character-select plumbing for adding bots to a match -- not for the AI itself.

⚠️ **AI opcodes branch on RENDER VISIBILITY** (`IFImOnScreen`, `IFMyRoomIsOnScreen`), so a
bot on an unrendered split-screen viewport will behave differently from one on screen. Check
this early; it will otherwise look like a bot bug and be a culling question.

## 3. Then netplay, on top of item 1

Not before it. Netplay needs deterministic simulation plus serialised per-tick input, and
the fixed-timestep work on the Mac side is the other half of that prerequisite. Attempting
it before both are in place is the standard way these projects acquire desync bugs nobody
can reproduce.

## 4. Depth attachment as a texture

Still open, still skipped, still blocking the depth-aware Tier 2 list. `ss_depth` is a
renderbuffer so nothing can sample it. No visible result on its own, which is exactly why it
keeps losing to more interesting work.

## 5. Windows `dist` packaging

A `-Target dist` staging exe, DLLs, `assets/fonts/` including `OFL.txt`, a default
`goldeneye.cfg` and a short README into `build-windows/dist/`. Verify from a copy on a
machine with no toolchain, which is the only test that means anything here.

## What NOT to touch

`getv/port/src/port_input.c`, `vendor/ge-decomp/**`, `getv/port/fast3d/gfx_pc.c`, and
`docs/` other than your own Windows documents and this file. The Mac is actively in the
colour-decode path and in `port_input.c`.

## Standing rules

- Never commit generated output as a fix. Anything that must survive belongs in a generator
  under `tools/`, because `vendor/` is gitignored and does not travel.
- Never push to GitHub. The user does that themselves, from their own terminal.
- `GETV_EXIT_FRAME` on every measurement, so two runs are comparable.
- **A probe that changes nothing has told you something.** `GETV_RGBA32BE` gave three
  byte-identical frames because the format is never uploaded. Check coverage before reading
  an identical result as proof of safety -- a five-level census here looked like proof and
  had none.
