# Contributing

Patches are welcome. This file covers the four things that are specific to this project and
easy to get wrong; everything else is ordinary.

Read [`docs/SETUP.md`](docs/SETUP.md) first — a working build is a prerequisite for almost any
useful change, and its troubleshooting section covers the failures that look alarming but are
expected.

## 1. Never commit game data

This is the one rule with no exceptions.

No ROM, no `base.zip`, and nothing derived from either: no extracted `.bin` blobs, no generated
asset `.c` files, no texture dumps, no audio banks. `.gitignore` blocks the known shapes of all
of these, but it cannot anticipate a new one, so check `git status` before committing rather than
trusting it.

The same applies to issues and pull requests. Do not attach that data, and do not paste it inline.
Error text, build logs and screenshots of the running game are fine and are what is actually
useful.

The reasoning is in [`docs/LICENSING.md`](docs/LICENSING.md): this project is publishable only
because it contains no game data. One commit changes that for everyone.

## 2. Changes to the decompilation live in patches

`vendor/ge-decomp` is gitignored. Anything you change in it is untracked and is lost the moment
that directory is re-cloned, so it has to be captured in `getv/patches/`.

There are two patches, split by *when* they can be applied rather than by subject:

| Patch | Covers | Applied |
|---|---|---|
| `0001-source.patch` | `src/`, `include/`, `tools/` | right after cloning the decomp |
| `0002-assets.patch` | eight generated asset files | at the end of the asset pipeline |

Regenerate each over its own paths. A bare `git diff` sweeps the whole extracted asset tree into
`0001`; see [`getv/patches/README.md`](getv/patches/README.md) for the exact commands.

Changes to the port layer itself — `getv/port/` — are ordinary tracked files and need none of this.

## 3. Say what you measured

The build prints four counts, and they are the cheapest evidence that a change did what you think:

```
mac game: 167 built, 1 failed
mac assets: 746 built, 0 failed
mac audio: 40 built, 0 failed
mac port layer: 23 built, 0 failed
```

`167 built, 1 failed` is correct. The one failure is always `src/tlb_manage.c`, which programs a
memory-management unit that does not exist here. A second name in that list is a real regression.

For anything that changes rendering or gameplay, measure it rather than describing it. Boot
straight into a level with `GETV_STAGE=<n>` (the numbers are the `LEVELID_*` enum in
`bondconstants.h`; Dam is 33) and use `GETV_EXIT_FRAME` to make the run terminate on its own.

One trap worth stating outright, because it has produced confident wrong conclusions here before:
**a level measured immediately after load with no input is showing you the intro camera, not
gameplay.** Frame counts and triangle counts taken there describe a cutscene. Drive past it before
believing anything.

## 4. Where code may and may not come from

Provenance is tracked deliberately, and the full table is in
[`docs/LICENSING.md`](docs/LICENSING.md) section 3. In short:

- **Perfect Dark** and **mgb64** are MIT and may be adapted, with attribution recorded at every
  site that uses them — repository, commit, and file.
- **GoldenRecomp**, `cblock85/GoldenEye64Recomp` and `chrissotraidis/goldenpad` are GPL-3.0 or
  carry a GPL obligation. **No code may be taken from them**, and this project cannot accept a
  contribution derived from one.
- `DeeStiz/007` has **no licence at all**. It may be read for understanding. Nothing may be
  copied or adapted from it.

If you are unsure whether something you are looking at is safe to draw from, ask in the pull
request before writing the code rather than after.

## Style

Match the file you are editing. The prevailing style is explanatory comments that say *why*
something is the way it is — particularly where the reason is a hardware quirk, a 32-to-64-bit
pointer problem, or an endianness difference — and no comment at all where the code already says
what it does.
