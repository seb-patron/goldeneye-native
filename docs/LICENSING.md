# Licensing and provenance

This document records where every part of Goldeneye-Native came from and under what terms, and
it states plainly which of those terms are settled and which are not. It is a factual record.
It is not legal advice, and it does not reach a legal conclusion about anything.

One question in particular — the licence covering the Fast3D renderer and the audio mixer this
project inherited — is **unresolved**, and section 4 sets out the evidence and the available
options without choosing among them.

Last verified 2026-08-22 against the working tree and the repository's git history. Every claim
below states how it was checked.

---

## 1. What this repository contains, and what it does not

### It does not contain game data

There is **no ROM, no extracted assets, no game data, no textures, no audio and no level data**
in this repository, in either the working tree or the git history.

How that was checked:

- **`git ls-files`** — the repository tracks **95 files**. Every one was enumerated. They are:
  the port layer under `getv/port/` (C sources and headers), three build wrappers
  (`getv/build.sh`, `tvos/build.sh`, `sm64tv/build.sh`), four `xcodegen` project manifests,
  four patch files under `*/patches/`, eight Python tools under `tools/`, two documents under
  `docs/`, `.gitignore`, and fifteen PNGs plus their `Contents.json` manifests under
  `getv/Sources/Assets.xcassets/` (see section 2.1 — those PNGs are not game data,
  but they are also not this project's own work).
- **Git history, not just the current tree.** A file deleted in a later commit is still
  distributed with the repository, so the tip being clean proves nothing on its own. Two checks
  were run over all 19 commits on `main`:
  - Every blob ever committed was resolved with `git rev-list --objects --all` piped through
    `git cat-file --batch-check` and sorted by size. **42 blobs exceed 500 KB**; the largest is
    3.59 MB. Every one of them is SDL2 upstream source or an Xcode build cache (section 2.2).
    `.git` totals 28 MB.
  - Every distinct path ever committed was enumerated — **2,519 paths**, of which 2,405 are
    under `deps/` (the vendored SDL2 tree). That list was matched against every game-data shape:
    `*.z64`, `*.n64`, `*.v64`, `*.otr`, `*.o2r`, `base.zip`, `libge*`, `getv_shot*`, `/assets/`,
    `obseg`, `*.inc.c`, `build-mac*`, `build-sim*`, `scratchpad`. **The only hit was
    `tools/gen_obseg_blobs.py`** — a generator script whose filename contains "obseg", not data.

  **No ROM, extracted asset, asset object file, frame capture or `base.zip` has ever been
  committed to this repository.** Nothing needs to be rewritten out of history on game-data
  grounds, and no history rewrite has been attempted.
- **Ignore rules verified against real paths, not read as text.** `git check-ignore -v` was run
  against each game-derived path that exists on disk today. All are matched:
  `roms/` (the ROM dumps), `getv/build/libge.a` and its 1,842 asset object files,
  105 `getv/build-*` per-slot build directories, `getv/getv_shot.bmp`,
  `sm64tv/Resources/res/base.zip`, `tvos/Resources/data/pd.ntsc-final.z64`, and `vendor/`.
- **A whole-tree sweep for game-data-shaped files** outside the ignored directories found
  nothing untracked and unignored: every hit was already covered by a rule.
- **`git status --untracked-files=all`** lists **71** untracked, unignored files — everything a
  `git add -A` would stage. All are shell scripts, Python tools, Markdown documents or small C
  sources. None is binary and none exceeds 200 KB. For comparison, before the ignore rules were
  hardened this figure was in the region of 41,800 files, including 31 MB `libge.a` archives and
  per-slot object files compiled from extracted ROM data.

### It does contain

| area | what it is |
|---|---|
| `getv/port/src/`, `getv/port/mac/`, `getv/port/include/` | This project's platform layer: windowing, input, audio device, filesystem, save handling, asset bridge, render loop. Roughly 8,700 lines. |
| `getv/port/fast3d/` | The Fast3D display-list renderer, inherited from sm64ex. Roughly 7,700 lines. **Licence unresolved — see section 4.** |
| `getv/port/audio/ge_mixer.{c,h}` | The N64 audio microcode in software, inherited from sm64ex. **Same unresolved licence — see section 4.** |
| `getv/port/configfile.h`, `getv/port/fs/fs.h` | Verbatim copies of sm64ex headers. **See section 4.4.** |
| `getv/port/fast3d/ge_sky_rdp.{c,h}` | Written for this project. Decodes GoldenEye's hand-assembled RDP triangle commands. |
| `getv/port/include/stb/stb_image.h` | stb_image v2.19 by Sean Barrett. Dual-licensed MIT / public domain (Unlicense); the notice is retained verbatim in the file. |
| `tools/`, `getv/patches/` | Build tooling and patches. This project's own work. |
| `getv/Sources/Assets.xcassets/` | Apple TV app icon and top-shelf artwork. **See section 2.** |

Two tracked entries are **symlinks, not files** (git mode `120000`):
`getv/port/include/PR` and `getv/port/include/platform_info.h`, both pointing into
`vendor/ge-decomp/include/`. Both are **relative** (`../../../vendor/ge-decomp/...`) and so
resolve correctly in any checkout. They carry no content of their own.

### The game's own source is not in this repository

The C source of GoldenEye 007 itself is **not** vendored here. It is obtained by the builder from
`n64decomp/007` into `vendor/ge-decomp/`, which `.gitignore` blocks. This repository supplies
only the platform layer that source is compiled against.

---

## 2. Four things settled before the first push

None of these was game data. All four were questions a maintainer should decide deliberately
rather than discover after publication, so each is recorded here with what was actually done.

### 2.1 The app icon and top-shelf artwork — removed

Fifteen PNGs under `getv/Sources/Assets.xcassets/App Icon & Top Shelf Image.brandassets/`
rendered the **"GoldenEye" wordmark and the 007 gun logo**. They were inspected directly, not
inferred from filenames.

They were not extracted from the ROM, but neither were they original work by this project: they
carried no provenance metadata, and the marks they reproduce belong to third parties — the 007
logo and the James Bond marks to Danjaq LLC and MGM, the GoldenEye 007 game branding to Nintendo
and Rare. They are not required to build or run anything.

They are no longer tracked. `.gitignore` blocks `**/*.xcassets/**` and `**/*.brandassets/**`, and
no commit in the published history contains them. Anyone building the tvOS target supplies their
own artwork.

### 2.2 SDL2 and Xcode build caches — not in the published history

An earlier local history vendored the full **SDL2 2.30.9** source tree, its tvOS CMake build
directory (a 2.8 MB `libSDL2.a` and several hundred `.o` files) and 1.2 MB of Xcode SDK stat
caches, along with the project's internal development notes.

The published history is a fresh one and contains none of it. `deps/SDL2`, `libSDL2.a`,
`tvos/build-device`, and the internal notes each appear in zero commits; the whole `.git`
directory is about 4 MB. `.gitignore` blocks `deps/`, `build/` and the notes so they cannot
return.

SDL2 is under the zlib licence, which permits source redistribution with the notice intact, so
this was repository hygiene and clone size rather than a licence problem. It is recorded because
the earlier state was described publicly and the record should not be left half-told.

### 2.3 Absolute local paths and personal identifiers — parameterised

Several tracked build files hardcoded an absolute home-directory path to a hand-built tvOS SDL2,
an Apple Developer Team ID, and one specific Apple TV's device identifier. None was a credential,
but all were personal identifiers that would have shipped, and the paths broke on every other
checkout.

All are now variables with sensible defaults:

- `${N64TVOS_PREFIX:-$HOME/.n64tvos}` in `getv/build.sh`, `getv/bt_build_sim.sh` and
  `getv/build_mac.sh`
- `${DEVELOPMENT_TEAM}` in `getv/project.yml`
- `$DEV_DEVICECTL` in `getv/build.sh`

The two tracked symlinks that had the same problem were made relative. `sm64tv/` and `tvos/` are
no longer tracked at all.

### 2.4 The repository is now published

It is at `https://github.com/SegfaultEvan/goldeneye-native`, which is also what `origin` resolves
to. The earlier mismatch between the configured remote and the intended release repository has
been reconciled.

That changes the cost of everything above. While the history was local, rewriting it was cheap;
now it is not. The history was therefore made clean *before* the first push rather than after, and
the checks in 2.1 through 2.3 are the record of that. **Nothing should be rewritten now without an
explicit decision**, because anyone who has already cloned holds the old objects regardless.

---

## 3. Per-component provenance

| component | path | origin | terms | state |
|---|---|---|---|---|
| Platform layer | `getv/port/src/`, `getv/port/mac/`, `getv/port/include/{config,platform,platform_info,port_support,system}.h` | This project | Not yet declared — the repository has **no root LICENSE file** | Open, but ours to decide |
| Sky RDP decoder | `getv/port/fast3d/ge_sky_rdp.{c,h}` | This project | Same as above | Ours |
| Build tooling | `tools/*.py`, `getv/patches/`, `*/build.sh`, `*/project.yml` | This project | Same as above | Ours |
| Fast3D renderer | `getv/port/fast3d/gfx_*.{c,h}` | **sm64ex**, which took it from **`Emill/n64-fast3d-engine`** | **Contested** | **Unresolved — section 4** |
| Audio mixer | `getv/port/audio/ge_mixer.{c,h}` | **sm64ex** `src/pc/mixer.{c,h}`, Emill's implementation of the N64 audio microcode | **Contested — same lineage** | **Unresolved — section 4** |
| Two inherited headers | `getv/port/configfile.h`, `getv/port/fs/fs.h` | **sm64ex** `src/pc/configfile.h`, `src/pc/fs/fs.h` — copied verbatim | **sm64ex ships no licence at all** | **Unresolved — section 4.4** |
| Image loader | `getv/port/include/stb/stb_image.h` | `nothings/stb`, v2.19 | MIT **or** public domain, at the user's option; notice retained in-file | Settled |
| Game source | `vendor/ge-decomp/` (not distributed here) | `n64decomp/007` | **No licence file in the upstream repository.** 58 files under `src/libultra/` carry Silicon Graphics proprietary notices | Upstream's situation; see section 5 |
| Reference: Perfect Dark | `vendor/pd-ext`, `vendor/pd-port` (not distributed here) | `perfect-dark-pc-port/perfect_dark` | MIT, © 2022 Ryan Dwyer — verified by reading `vendor/pd-port/LICENSE` | Cleared for adaptation **with attribution** |
| Reference: mgb64 | `akratch/mgb64` | Upstream | MIT | Cleared for adaptation **with attribution** |

### Quarantined — no code may be taken from these

| project | licence | why |
|---|---|---|
| GoldenRecomp | GPL-3.0 | Incompatible with permissive publication |
| `cblock85/GoldenEye64Recomp` | GPL-3.0 | Same |
| `chrissotraidis/goldenpad` | No top-level licence; documents an N64ModernRuntime GPL-3.0 obligation | Both problems at once |
| `DeeStiz/007` | **No licence at all** | May be read for understanding; nothing may be copied or adapted |

---

## 4. The Fast3D question

This is the one unresolved item. It is stated here as fact, with the evidence, and the options
are laid out with their consequences. **No option is chosen here.**

### 4.1 What is established

**`Emill/n64-fast3d-engine` has never been MIT-licensed.** From its own repository history:

- `LICENSE.txt` has exactly four commits.
- The initial commit **`a99492dd`** (2020-04-24) is a BSD-2-Clause-shaped notice whose
  condition 2 reads, in full: **"Redistributions in binary form are not allowed."** A flat ban.
- Commit **`881eb68b`** (2021-10-26, *"Updating license"*) changed one line, adding the
  carve-out *"except in cases where the binary contains no assets you do not have the right to
  distribute."*
- **GitHub classifies the repository `NOASSERTION`** — its licence detector does not recognise
  the text as any standard licence.

**This project's lineage is the stricter, pre-2021 one.** Verified locally against
`vendor/sm64ex`:

- sm64ex ships **no root licence file** (`ls vendor/sm64ex | grep -i licen` returns nothing).
- It reproduces Emill's notice in exactly one place, `src/pc/README-n64-fast32-engine.md`, and
  that copy is the **pre-2021 form**: `Copyright (c) 2020, Emill`, condition 2 reading
  `Redistributions in binary form are not allowed.`, **with no asset carve-out.**

So the strict reading is not one downstream project's opinion. It is the notice this project's
own upstream ships.

**How much of that upstream is actually still present** was measured line-by-line with a
sequence matcher over line-ending-normalised text (`difflib.SequenceMatcher`, `autojunk=False`),
comparing `vendor/sm64ex/src/pc/gfx/` against `getv/port/fast3d/`:

| file | sm64ex lines | our lines | identical lines | share of upstream surviving verbatim |
|---|---|---|---|---|
| `gfx_pc.c` | 1,832 | 5,515 | 1,593 | 86% |
| `gfx_opengl.c` | 791 | 968 | 736 | 93% |
| `gfx_cc.c` | 41 | 47 | 40 | 97% |
| `gfx_sdl2.c` | 354 | 432 | 348 | 98% |
| `gfx_cc.h` | 58 | 104 | 55 | 94% |
| `gfx_rendering_api.h` | 36 | 36 | 34 | 94% |
| `gfx_pc.h`, `gfx_opengl.h`, `gfx_sdl.h`, `gfx_screen_config.h`, `gfx_window_manager_api.h` | 83 | 83 | 83 | 100% |
| `src/pc/mixer.c` → `audio/ge_mixer.c` | 871 | 1,244 | 852 | 97% |

The port has grown these files substantially — `gfx_pc.c` is three times its upstream length —
but it has **not** displaced them. Between 86% and 100% of upstream's text is still present
verbatim. This is inherited code that has been extended, not a reimplementation.

Note that this extends to **`getv/port/audio/ge_mixer.c`**, which an earlier internal note
listed as original work. Its own file header states it is sm64ex's `src/pc/mixer.c`, Emill's
implementation of the N64 audio microcode, and the measurement above confirms 97% of that file
survives verbatim. The Fast3D question therefore covers the mixer too.

**Downstream projects label the same lineage inconsistently:**

- **Perfect Dark's port** carries `port/fast3d/LICENSE.txt` reading plain **MIT**,
  `Copyright (c) 2020 Emill, MaikelChan` — verified by reading the file in `vendor/pd-port`.
  That label arrived in commit **`9508b136`** (2023-08-01, *"replace old fast3d with
  libultraship-fast3d"*), which **deleted Emill's custom text and inserted standard MIT while
  keeping the Emill copyright line.** PD's fast3d files are `.cpp` and do not correspond
  file-for-file to ours; ours are the `.c` sm64ex set.
- **mgb64**, which is MIT overall, **explicitly retracts** an earlier MIT claim about the Emill
  engine and describes it as custom BSD-2-Clause with a binary-redistribution restriction.

### 4.2 What is not established

Whether this project's Fast3D is nearer to Emill's original or to libultraship's later rewrite,
and what either text permits. Nobody on this project has asked Emill.

At present `getv/port/fast3d/` carries **no licence file, no provenance note and no attribution
header of any kind**; `gfx_pc.c` still opens on `#include <math.h>`. Whatever is decided below,
that gap should be closed: the notice has to travel with the source under every reading of it.

### 4.3 A related finding: the inheritance is wider than Fast3D

The measurement above was run as a sweep, not a spot check. Every one of the 45 C and header
files under `getv/port/` (excluding the vendored `stb/`) was compared against all 1,924 sources
in `vendor/sm64ex`, `vendor/sm64-port` and `vendor/pd-port`, matching on filename and near-name
variants. Everything scoring at or above 25% overlap with an upstream file is listed here:

| our file | upstream | share of upstream surviving verbatim |
|---|---|---|
| `getv/port/configfile.h` | `sm64ex/src/pc/configfile.h` | **100%** |
| `getv/port/fs/fs.h` | `sm64ex/src/pc/fs/fs.h` | **100%** |
| `getv/port/audio/ge_mixer.c` | `sm64ex/src/pc/mixer.c` | 97% |
| `getv/port/audio/ge_mixer.h` | `sm64ex/src/pc/mixer.h` | 66% |
| `getv/port/fast3d/*` | `sm64ex/src/pc/gfx/*` | 86–100% (table above) |
| `getv/port/pc_main.h` | `sm64ex/src/pc/pc_main.h` | 33% — the file's own header credits sm64ex |
| `getv/port/src/pc/controller/controller_keyboard.h` | `sm64-port/src/pc/controller/controller_keyboard.h` | 42% — three shared function declarations; the file is otherwise this project's own and says so |
| `getv/port/src/port_audio.c` | `pd-port/port/src/audio.c` | 29% — inspected; SDL boilerplate, not adapted code. The file is this project's own and its header explains what it replaces |

Everything else under `getv/port/src/` and `getv/port/mac/` scored below 25% and is this
project's own work.

`configfile.h` and `fs/fs.h` are unmodified sm64ex headers. They are still recognisable as such:
`configfile.h` defines `CONFIGFILE_DEFAULT "sm64config.txt"` and `fs.h` uses the header guard
`_SM64_FS_H_`.

This matters because it widens the question. **sm64ex ships no licence file and makes no licence
statement in its README** — verified by listing the checkout root and grepping the README; the
same is true of `sm64-port`. Emill's notice in
`src/pc/README-n64-fast32-engine.md` is the only licence text anywhere in the sm64ex tree, and
it covers only the Fast3D engine. The other two headers are inherited from an upstream that
states nothing at all, and sm64ex's own non-Fast3D code descends from the Super Mario 64
decompilation, which likewise carries no licence.

Both headers are small and neither is load-bearing. Rewriting them from the interfaces this port
actually uses would remove them from the question at low cost, and is worth considering
independently of whatever is decided about Fast3D.

### 4.4 Options and their consequences

**(a) Ship source only; users build their own binary.**
The pre-2021 notice, as written, addresses two cases: it permits redistribution *in source
form* provided the notice is retained, and states that redistribution *in binary form* is not
allowed. A source-only release with Emill's notice restored to `getv/port/fast3d/` is the case
the text addresses affirmatively. Consequence: no release binaries, no App Store or TestFlight
distribution, and every user needs a full toolchain. It is the cheapest option to execute and it
leaves every other option below still available.

**(b) Replace Fast3D with an unambiguously licensed renderer.**
Candidates are libultraship's fast3d (MIT, © 2022 kenix3) or a from-scratch F3D interpreter.
Consequence: the question disappears permanently and binaries become possible. But the measured
divergence above cuts the other way here — `gfx_pc.c` has grown from 1,832 to 5,515 lines with
GoldenEye-specific work (the F3D `G_TRI4` extension, sky RDP hooks, GoldenEye's LOD and texel
density behaviour, near-plane rejection tuned to the game's own microcode). All of that would
have to be re-landed on a different base. Large, and it puts working rendering at risk.

**(c) Ask Emill for clarification.**
A short, specific question: does the current `LICENSE.txt` apply retroactively to the code as it
stood in 2020, and is the asset carve-out intended to permit binaries containing no game assets?
Consequence: cheap, and a clear answer resolves the matter for everyone downstream. But it is
outside this project's control, may go unanswered, and an unfavourable answer forecloses
option (d) explicitly rather than leaving it merely uncertain.

**(d) Ship binaries and accept the risk.**
Consequence: of the four, this is the only one that proceeds contrary to the notice this
project's own upstream ships, rather than around it. Two further facts belong with the decision.
First, it is a decision about two separate things, not one: section 5 records that this port
compiles extracted game assets directly into the executable, so a binary of Goldeneye-Native
contains the game's data whatever the Fast3D question turns out to mean. Second, the 2021
carve-out — *"except in cases where the binary contains no assets you do not have the right to
distribute"* — is written for binaries that carry no such assets, which is not what this port
currently produces.

---

## 5. Bring your own ROM

**No game data is distributed by this project and none can be.** The chain is as follows.

### What the user must supply

A dump of their own legal NTSC (US) GoldenEye 007 cartridge:

- 12,582,912 bytes, big-endian z64 format (magic `80371240`, internal name `GOLDENEYE`)
- SHA-1 `abe01e4aeb033b6c0836819f549c791b26cfde83`

That hash matches `ge007.u.sha1` in the decompilation, so a dump with that value is
byte-identical to what a correct US build of the decompilation produces. A dump with a `.n64` or
`.v64` extension, or a header of `37804012`, is byte-swapped and must be converted to native
big-endian first.

The file goes at `vendor/ge-decomp/baserom.u.z64`. The convention used here is to keep dumps in
`roms/` at the repository root and symlink one into place; `.gitignore` blocks both locations.

### What the build does with it

The decompilation's own extraction step consumes the ROM. `vendor/ge-decomp/Makefile` target
`extract_u` requires `baserom.u.z64` to be present and invokes
`scripts/extract_baserom.u.sh`; `prerequisites` depends on `extractassets`. The result is
**1,842 C source files, 123 MB, under `vendor/ge-decomp/assets/`** — the game's models,
textures, level backgrounds, stan collision data, setup files, animation tables, text banks and
audio segments, transcribed from the cartridge into C.

Those files are then compiled and **linked into the executable**. On the N64 they lived in ROM
segments and were DMA'd in at runtime; compiled natively they are ordinary linked data with real
pointers, so there is no ROM loader at runtime and no offset-to-pointer translation.

### Why no game data can be redistributed

Two consequences follow, and they are worth separating:

1. **The extracted `assets/` tree is game data in a different file format.** Transcribing a
   texture into a C array does not change what it is. It cannot be committed, mirrored or
   attached to a release.
2. **A built binary of this port contains the entire game.** This is architecturally different
   from Perfect Dark's port, which reads its ROM at runtime from a `data/` directory and whose
   binary is therefore ROM-free. Ours is not. **A release binary of Goldeneye-Native would
   contain the copyrighted game in full**, and that is true irrespective of the Fast3D question
   in section 4.

Point 2 is the reason the "bring your own ROM" model here means *bring your own ROM and build
it yourself*, rather than *download our binary and supply a ROM at runtime*. Changing that would
mean re-architecting asset delivery to load from disk at runtime, which is a significant piece of
work and is not currently planned.

`.gitignore` enforces the first point mechanically. It blocks `roms/`, every `*.z64` / `*.n64` /
`*.v64` / `*.elf`, `**/base.zip`, `vendor/`, `deps/`, all `getv/build-*` and `build-mac-*` and
`build-sim-*` directories, every `*.o` / `*.a` / `*.dSYM` anywhere in the tree, `*.bmp` frame
captures, and `scratchpad/`. Each of those rules was verified against a real path that exists on
disk (section 1). **Do not defeat them.**

### The embedded ZX Spectrum emulator

`vendor/ge-decomp/src/game/spectrum.c` is **8,911 lines implementing a complete Z80 emulator**,
which the retail cartridge used to run ten Ultimate Play the Game titles as an unlockable extra.
It now compiles in this port.

The distinction here is the same one as for the base ROM, and it is worth stating separately
because the file's size invites the wrong assumption:

- **The emulator code is part of the decompilation.** It carries the same status as the rest of
  `vendor/ge-decomp/src/` — no licence file upstream, not distributed by this repository.
- **The emulator embeds no game data.** Verified by reading it: the only byte-array literals in
  the file are small Z80 and keyboard tables at lines 55–56, 75–85 and 113–116. The ten games are
  loaded at runtime from paths listed at `spectrum.c:99-108`: `em/data/sabre.seg.rz`,
  `atic`, `jetpac`, `jetman`, `alien8`, `gunfright`, `under`, `knightlore`, `pssst`, `cookie`.
- **Those ten files are not present anywhere in this repository or in the decompilation
  checkout** — confirmed by a whole-tree search for `*.seg.rz`, which returns nothing. They come
  out of the user's own ROM via the extraction step, exactly like every other asset.

The ten games are separate third-party works with their own rights holders, distinct from
GoldenEye 007 itself. **They are not this project's to distribute, and the same rule applies to
them as to the base ROM: bring your own.**

### The decompilation's own position

`n64decomp/007` **has no licence file** — verified by listing the checkout root. Its
`src/libultra/` sources carry Silicon Graphics proprietary notices reading, in part, that they
*"contain unpublished proprietary information of Silicon Graphics, Inc."* and may not be
disclosed or copied without written consent; **58 files under `src/` carry that header.**

That is upstream's situation, not something this project created or can resolve. It is recorded
because it is a fact about the base this port is built on, and because it bears on any decision
in section 4.4.

---

## 6. Attribution

Every adaptation from another project must record **repository, commit and file at the
adaptation site in the source, and in this document.** That is a standing rule, not a
formality — it is what makes the table in section 3 verifiable by someone who was not here.

### Currently carried

| what | from | where it lands |
|---|---|---|
| Fast3D renderer | sm64ex `src/pc/gfx/` (from `Emill/n64-fast3d-engine`) | `getv/port/fast3d/gfx_*.{c,h}`. **No notice file present — see 4.2.** |
| N64 audio microcode in software | sm64ex `src/pc/mixer.c` (Emill) | `getv/port/audio/ge_mixer.c`. The file header names its origin and lists its four changes. |
| RSP microcode reference | `perfect-dark-pc-port/perfect_dark`, `src/rsp/gsp.s` — an annotated copy of the same microcode GoldenEye runs | Read, not copied. Cited at `getv/port/fast3d/gfx_pc.c:2144`, `:3168`, `:4944` and `getv/port/fast3d/ge_sky_rdp.c:332`. |
| stb_image v2.19 | `nothings/stb`, Sean Barrett | `getv/port/include/stb/stb_image.h`, licence notice intact in-file. |

### Exact upstream revisions

Attribution has to name a commit, not just a repository. These are the revisions present in this
working tree at the time of writing, obtained with `git -C <dir> log -1`:

| checkout | upstream | branch or tag | commit | date |
|---|---|---|---|---|
| `vendor/pd-port` | `perfect-dark-pc-port/perfect_dark` | `port`, tag `ci-dev-build` | `514bf7affd3259b7919165201342ff81a026d92c` | 2026-05-29 |
| `vendor/pd-ext` | `perfect-dark-pc-port/perfect_dark` | `pr653` | `e5484dee23d1e8144d92b8b98f362869d9fd0d66` | 2025-12-02 |
| `vendor/ge-decomp` | `n64decomp/007` | `master` (grafted) | `c4356466796c697dfd298010b9bed261f9ed8c6a` | 2026-08-17 |
| `vendor/sm64ex` | `sm64pc/sm64ex` | `nightly` (grafted) | `d7ca2c04364a6dd0dac58b47151e04e26887e6f0` | 2024-12-17 |
| `vendor/sm64-port` | `sm64-port/sm64-port` | `master` (grafted) | `2b17d081c9798b31b91dc71f37994b0da28cffc9` | 2024-11-15 |

Both Perfect Dark checkouts carry the same two licence files: a root `LICENSE` reading MIT,
© 2022 Ryan Dwyer, and `port/fast3d/LICENSE.txt` reading MIT, © 2020 Emill, MaikelChan. The two
`fast3d/LICENSE.txt` files are byte-identical to each other.

Note that `vendor/ge-decomp`, `vendor/sm64ex` and `vendor/sm64-port` are **grafted shallow
clones**. Their local history is truncated, so a commit hash from them identifies the revision
but does not let anyone reconstruct its ancestry locally.

### Required for anything adopted in future

Perfect Dark (`vendor/pd-ext`, `vendor/pd-port`) and mgb64 are MIT and cleared for adaptation.
Their MIT notices must be reproduced, and each adaptation site must carry a comment naming the
upstream repository, the commit adapted from, and the upstream file. See `docs/PERFECT_DARK.md` for the specific
candidates, the exact commits to cite, and two attribution gaps inside Perfect Dark's own tree
(`port/fast3d/glad/` carries no notice at all) that need resolving before anything is taken from
those directories.

One adaptation is already in place and **currently unattributed**: the `__SSE4_1__` / `__ARM_NEON`
SIMD dispatch block at `getv/port/audio/ge_mixer.c:34-44` matches
`vendor/pd-port/port/src/mixer.c:12-25`. It should carry a comment naming
`perfect-dark-pc-port/perfect_dark @ 514bf7a`, `port/src/mixer.c`, MIT, © 2022 Ryan Dwyer.

### Not yet decided

**This repository has no root LICENSE file.** The platform layer, the sky RDP decoder and the
build tooling are this project's own work and no terms have been declared for them. That is a
separate decision from section 4 and can be made independently of it.
