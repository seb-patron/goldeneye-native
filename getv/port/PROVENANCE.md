# Provenance of `getv/port/`

**Status: unresolved.** This is a factual record, not legal advice, and not a decision. It
exists because the project intends to publish this port layer, and `getv/port/fast3d/`
currently carries no licence file, no provenance note and no attribution header of any kind
(`gfx_pc.c` opens on `#include <math.h>`).

Sources and full texts: `docs/research/MGB64_MINING.md` §5, `docs/research/GOLDENPAD_PRIOR_ART.md`.

---

## 1. What is in here, and where it came from

| path | origin | notes |
|---|---|---|
| `fast3d/gfx_pc.c`, `gfx_opengl.c`, `gfx_cc.c`, `gfx_sdl2.c`, headers | **sm64ex**, which took it from **`Emill/n64-fast3d-engine`** | licence contested — see §2 |
| `fast3d/ge_sky_rdp.{c,h}` | ours | decodes GE's hand-built RDP triangles from `sky.c`'s `G_RDPHALF_*` pairs |
| `audio/ge_mixer.c` | ours, built against libultra's AL semantics | `aPoleFilter` derived from GE's own `init_lpfilter` coefficients |
| `src/**`, `mac/**` | ours | tvOS/macOS harness, input, asset bridge, render loop |

Anything adapted from another project must record repo, commit and file both at the
adaptation site and in this document.

## 2. The Fast3D licence question — settled as to fact, open as to consequence

`Emill/n64-fast3d-engine` has never been MIT. This is established from its own history:

- `LICENSE.txt` has exactly four commits.
- In the initial commit `a99492dd` (2020-04-24), condition 2 read `Redistributions in binary
  form are not allowed.` — a flat ban.
- Commit `881eb68b` (2021-10-26, *"Updating license"*) changed one line only, adding the
  carve-out *"except in cases where the binary contains no assets you do not have the right
  to distribute"*.
- GitHub classifies the repository `NOASSERTION`.

Two downstream projects label the same lineage differently:

- **Perfect Dark** (`port/fast3d/LICENSE.txt`) says plain MIT. That label was applied in
  commit `9508b136` (2023-08-01, *"replace old fast3d with libultraship-fast3d"*), which
  deleted Emill's custom text entirely and inserted standard MIT while retaining the
  `Copyright (c) 2020 Emill, MaikelChan` line. libultraship's own root licence is MIT
  © 2022 kenix3.
- **mgb64** (MIT overall) explicitly retracts an earlier MIT claim about the Emill engine,
  reading it as custom BSD-2-Clause with a binary-redistribution restriction.

Our own lineage is the stricter one. We descend from sm64ex, which ships no root licence and
reproduces the Emill notice in exactly one place — `src/pc/README-n64-fast32-engine.md` — in
the pre-2021 form, with the flat binary ban and no asset carve-out. The strict reading is
therefore not merely mgb64's opinion; it is the notice our own upstream ships.

What this does not settle: whether our Fast3D is closer to Emill's original or to
libultraship's rewrite, and what either permits. That requires a human decision. Do not
publish `getv/port/fast3d/` on the assumption that it is MIT.

## 3. Quarantine — do not take code from these

| project | licence | status |
|---|---|---|
| GoldenRecomp | GPL-3.0 | quarantined; incompatible with permissive publication |
| `cblock85/GoldenEye64Recomp` | GPL-3.0 | quarantined |
| `DeeStiz/007` | none (`license: null`) | read for understanding only, never adapt |
| `chrissotraidis/goldenpad` | no top-level licence; notes an N64ModernRuntime GPL-3.0 obligation | do not adapt |

## 4. Cleared for adaptation, with attribution

| project | licence | local copy |
|---|---|---|
| mgb64 (`akratch/mgb64`) | MIT | `scratchpad/mgb64` (head `0d1d40b4`) |
| Perfect Dark (`perfect-dark-pc-port/perfect_dark`) | MIT | `vendor/pd-ext` (2025-12-02) and `vendor/pd-port` |

## 5. Never distributable, under any licence

The ROM, extracted assets, and anything derived from them. `.gitignore` blocks `*.z64`,
`*.n64`, `*.v64`, `*.o2r`, `*.otr` and `base.zip`, and also `getv/build-sim-*/` and
`getv/build-mac/`, which hold objects compiled from extracted ROM data. Without those last
two entries, a `git add -A` in `getv/` would stage derived game data.

The decomp source itself (`n64decomp/007`) has no licence file, and its libultra sources
carry SGI proprietary headers. That is upstream's situation rather than ours to resolve, but
it is a fact about the base this port is built on.
