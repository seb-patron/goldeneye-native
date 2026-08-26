# Reuse audit

What this project should borrow, what it already borrows, and what it must not touch.

The question worth asking before writing anything is not "what feature next" but "has
somebody in the N64 decomp ecosystem already solved this". This table is the answer, kept
honest by two rules:

1. **A licence is checked before anything is copied, and the check is dated.** "Probably
   MIT" is not a licence check. Rows below are marked with what was actually verified.
2. **Already-done is stated as already-done.** Half of the obvious recommendations for this
   project describe work that shipped months ago, and a reuse list that does not know that
   sends people to re-solve solved problems.

---

## Already in the tree

These come up in every outside review of this project as things to adopt. They are adopted.
`docs/THIRD_PARTY.md` carries the pins, the manifest and the patch.

| Problem | Source | Where it lives | Licence |
|---|---|---|---|
| Fast3D display-list translation | **sm64ex**, pinned at `d7ca2c0` | `getv/port/fast3d/` (11 files) | see `THIRD_PARTY.md` §2 - the Emill lineage has never carried a settled licence, and the position is documented rather than assumed |
| N64 audio mixer | **sm64ex** `src/pc/mixer.c` | `getv/port/audio/ge_mixer.c` | same |
| Window, input, audio device, gamepad | **SDL2** 2.30.9 | built from `deps/`, linked static | zlib |
| Mod scripting | **Lua 5.4.7** | `getv/port/src/ge_lua.c` | MIT, checksum-pinned in `tools/fetch_lua.sh` |

So "use Fast3D", "borrow the SM64 audio mixer" and "use SDL for the platform layer" are
**complete**. The renderer abstraction people suggest building - `GfxRenderingAPI` - is a
struct of ~20 function pointers that has been in place since the port booted.

**minimp3 does not apply here.** The Perfect Dark port needs it because PD ships MP3
music. GoldenEye's audio is N64 sequenced audio through Rare's modified libultra AL, with no
MP3 anywhere. This is the clearest example of a recommendation that transfers between two
Rare N64 games and still happens to be wrong.

---

## Worth adopting, in order

| Problem | Source | Licence | Verdict |
|---|---|---|---|
| Developer/debug UI | **Dear ImGui** | MIT | **In progress.** Gated behind `GETV_IMGUI`, off by default. The value is a live panel over player, camera, AI, renderer and memory, which this project currently reads through printf and log grepping. |
| Profiling | **Tracy** | BSD-3-Clause | **Yes**, once ImGui lands. Frame cost is currently unattributed: there is no measurement separating game tick, render, audio and AI. |
| GPU capture | **RenderDoc** | external tool | **Yes, and free.** Not a dependency - it attaches to the running binary. Nothing to integrate; it wants a doc section, not a build change. |
| Metal backend | **libultraship** `gfx_metal.cpp` | **MIT (verified 2026-08-24)** | **Yes.** Removes the deprecated-GL risk on Apple platforms. our own audit scopes the adapter at about 8 signature differences; libultraship refactored the same Emill lineage from a C function-pointer struct into a C++ virtual class. |
| Modern renderer for GoldenEye+ | **RT64** | **MIT (verified 2026-08-24)** | **The serious candidate - see below.** |
| Modern controls reference | GoldenEye digital-controls fork | **unchecked** | Check the licence first. If it is a fork of `n64decomp/007` it inherits that project's terms, which are not permissive by default. |

### RT64, and a correction to this project's own record

Our own notes record that **GoldenRecomp** was rejected in August 2026, and that decision
stands: it needs a special `TLBFREE_NOCOMPRESSION` decomp branch, ships Windows-only
recompiler binaries, and its own README admits broken Dam skybox and Frigate sky/water.

**That was a rejection of the static-recompilation route, not of RT64.** RT64 is a separate
project, it is MIT, and it is an N64 renderer with Direct3D 12, Vulkan and Metal backends,
built specifically to be adopted by native ports.

This matters because it answers the objection that rules out bgfx. bgfx knows nothing about
N64 colour combiners, so the expensive part of any backend - generating a shader from a
combiner mux - stays per-backend and unshared. **RT64 already understands N64 rendering**,
so it is the one option where that work is genuinely already done. It also ships upscaling,
widescreen and **frame interpolation**, and interpolation is exactly the mechanism the
fixed-tick problem in `VISION.md` needs.

The cost is real and should not be glossed: adopting RT64 replaces the renderer, and the
current Fast3D + GL path works on macOS and Linux today. So the sequencing is
**keep Fast3D now, evaluate RT64 when Phase 5 wants shadows, SSAO and HDR** - and evaluate
it ahead of bgfx, not alongside it.

---

## Reference only - read, never copy

| Project | Licence | Why it is here |
|---|---|---|
| `Graslu/1964gepd`, Mouse Injector | **gpl-2.0 (verified 2026-08-24)** | Carries real GoldenEye timing, input and camera knowledge. **Permission from the fork maintainer would not be enough**: upstream 1964 is Joel Middendorf's gpl work, so a maintainer can relicense only their own additions. Take the *bug list*, never the code - behaviour is not copyrightable. |
| GoldenRecomp, `cblock85/GoldenEye64Recomp`, `chrissotraidis/goldenpad` | GPL | Standing quarantine. |
| `DeeStiz/007` | unlicensed | No licence means no permission. Read to understand, never adapt. |
| Star Fox 64, Banjo-Kazooie, DK64, Jet Force Gemini decomps | varies, mostly unlicensed | **Comparative reverse engineering only.** The Rare titles are the valuable ones: same studio, same lineage, same asset and animation conventions, so "how did Rare structure this" is answerable. That is a reading exercise, and decomp repositories rarely carry a licence that permits anything else. |

**The asymmetry that makes the GPL quarantine cheap.** Every one of those projects
patches a game it does not have the source to - an emulator hooked from outside, or a
recompiled binary. This project has the decompiled source. Every fix they inject by memory
patching can be made correctly at its origin here. Their real contribution is a list of
which bugs are worth fixing, and a list is not a derivative work.

---

## Ideas worth keeping, independent of any repository

**A native actor wrapper.** Ocarina of Time's `Actor` (position, rotation, update, draw,
collision, state) is a cleaner abstraction than GoldenEye's setup/prop system, and a
port-side `GEActor` wrapper would let new objects exist without inheriting the original
engine's assumptions about prop tables and setup files. This is the same architectural move
as separating player, camera and weapon orientation in `VISION.md`: an escape hatch that
gets cheaper the earlier it is built, and it is what the Lua host needs before it can create
anything rather than only observe.

**Randomizer architecture.** The transferable idea is not any randomizer's code but its
central discipline: how to alter a finished game's data without violating the assumptions
the game still makes. `VISION.md` records the GoldenEye form of it - objectives need
declared capabilities before they can be shuffled, or "photograph the computer" lands in a
level with no computer.

**Ship of Harkinian's mod loader and asset-override model.** Worth studying as a design for
`mods/<name>/` growing from scripts into asset replacement. The Lua host already establishes
the directory convention; asset override is the next layer, and the lookup order is the part
worth copying rather than inventing.

---

## Standing rule

Before adding a dependency, answer three questions in this file:

1. **What problem does it solve that this project actually has?** Not one it might have.
2. **What is its licence, checked today, and does it survive contact with `LICENSING.md`?**
3. **What does it cost to remove again?** A profiler is free to drop. A renderer is not.
