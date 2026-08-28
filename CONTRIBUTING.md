# Contributing

Patches are welcome. This file covers the handful of things that are specific to this project and
easy to get wrong; everything else is ordinary.

Read [`docs/SETUP.md`](docs/SETUP.md) first - a working build is a prerequisite for almost any
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

Changes to the port layer itself - `getv/port/` - are ordinary tracked files and need none of this.

## 3. Say what you measured

The build prints four counts, and they are the cheapest evidence that a change did what you think:

```
mac game: 167 built, 0 failed
mac assets: 746 built, 0 failed
mac audio: 40 built, 0 failed
mac port layer: 64 built, 0 failed
```

**Every count reads `0 failed`.** Any name in a `FAILED:` line is a real regression. Older notes
said `167 built, 1 failed` was correct because `src/tlb_manage.c` programs a memory-management
unit that does not exist here; it and nine other N64-hardware and SGI-dev-host files are now
excluded by name instead, so nothing is expected to fail. See `docs/SETUP.md` for why that
changed.

For anything that changes rendering or gameplay, measure it rather than describing it. Boot
straight into a level with `GETV_STAGE=<n>` (the numbers are the `LEVELID_*` enum in
`bondconstants.h`; Dam is 33) and use `GETV_EXIT_FRAME` to make the run terminate on its own.

For renderer changes there is a gate: `tools/render_refs.py check` recaptures every stage and
compares it against `tools/refs/render.txt`, reporting any that drift. Run it before and after.
If your change is meant to affect one stage, that is what the output should say. If it is meant
to affect none, likewise.

One trap worth stating outright, because it has produced confident wrong conclusions here before:
**a level measured immediately after load with no input is showing you the intro camera, not
gameplay.** Frame counts and triangle counts taken there describe a cutscene. Drive past it before
believing anything.

## 4. Compiling one file

`./build_mac.sh lib` writes the exact flags it used to `getv/build-mac/flags.txt`, one per
line. Use that file rather than retyping a clang command:

```bash
cd vendor/ge-decomp
A=(); while IFS= read -r l; do A+=("$l"); done < ../../getv/build-mac/flags.txt
clang "${A[@]}" -c src/game/bg.c -o /tmp/t.o
```

Read it line by line into an array. The include paths contain spaces, so splatting the file
unquoted breaks them and clang reports a missing directory that is really a quoting bug.

Do not approximate the flags. The build passes `-Wno-everything`, and an approximation using
`-w` is not equivalent: recent clang treats an implicitly declared function as an error rather
than a warning, and `-w` does not undo that while `-Wno-everything` does. An approximated
command therefore reports errors in files that build perfectly well, which is a good way to
talk yourself into reverting something that was never broken.

Related: `vendor/ge-decomp` is not a scratch directory. `git checkout -- <file>` there discards
every port change to that file, not just yours, and `git status` will not warn you because the
whole tree is modified by design. Copy the file first if you need to compare against upstream.

## 5. Where code may and may not come from

Provenance is tracked deliberately, and the full table is in
[`docs/LICENSING.md`](docs/LICENSING.md) section 3. In short:

- **Perfect Dark** and **mgb64** are MIT and may be adapted, with attribution recorded at every
  site that uses them - repository, commit, and file.
- **GoldenRecomp**, `cblock85/GoldenEye64Recomp` and `chrissotraidis/goldenpad` are GPL-3.0 or
  carry a GPL obligation. **No code may be taken from them**, and this project cannot accept a
  contribution derived from one.
- `DeeStiz/007` has **no licence at all**. It may be read for understanding. Nothing may be
  copied or adapted from it.

If you are unsure whether something you are looking at is safe to draw from, ask in the pull
request before writing the code rather than after.

## Style

Match the file you are editing. The prevailing style is explanatory comments that say *why*
something is the way it is - particularly where the reason is a hardware quirk, a 32-to-64-bit
pointer problem, or an endianness difference - and no comment at all where the code already says
what it does.
