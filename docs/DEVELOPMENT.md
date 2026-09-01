# Development workflow

This guide covers the local edit/build/test loop. Read [`CONTRIBUTING.md`](../CONTRIBUTING.md)
before preparing a contribution and [`CODEBASE.md`](CODEBASE.md) for the architecture and path
ownership model.

## Prepare a development checkout

Start from current community `main` and create one branch for one logical change:

```bash
git switch main
git pull --ff-only origin main
git switch -c fix/short-description
```

For documentation or maintenance work, use an equally descriptive prefix such as `docs/` or
`chore/`.

Complete the one-time setup before changing code:

```bash
bash tools/install.sh
```

On Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1
```

The installer is resumable. It fetches ignored dependencies, applies patches, generates local
assets from your own ROM, and builds. Never add any of those ignored outputs to Git.

## Establish a baseline

Before a behavioral change, reproduce the problem on unchanged `main` and record:

- the exact commit (`git rev-parse --short HEAD`);
- OS, architecture, renderer, and relevant hardware;
- the full launch command and any `GETV_*` settings;
- expected and actual behavior; and
- the relevant test/build result.

Run the ROM-free unit suite before editing:

```bash
bash getv/port/tests/run_tests.sh
```

Use the PowerShell runner on Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File getv\port\tests\run_tests.ps1
```

A pre-existing failure belongs in the record; do not weaken a threshold or attribute it to your
change.

## Choose the correct edit surface

There are three source categories, and confusing them is the most expensive project-specific
mistake:

1. **Tracked port code** under `getv/port/` is edited and committed normally.
2. **Ignored fetched port files** are reconstructed from sm64ex plus
   `getv/patches/thirdparty/0001-getv-port-layer.patch`. After editing one, run
   `tools/fetch-thirdparty.sh regen` so the tracked patch records the change.
3. **Ignored game code** under `vendor/ge-decomp/` must be represented in a numbered patch under
   `getv/patches/`. An edit left only in `vendor/` disappears on a fresh setup.

Use this before editing a suspicious path:

```bash
git ls-files --error-unmatch path/to/file
```

A zero exit status means the file is tracked. Read
[`getv/patches/README.md`](../getv/patches/README.md) before modifying game code or generated asset
sources. Never regenerate a broad patch over the entire decompilation; that can capture hundreds of
megabytes of ROM-derived data.

## Build the smallest relevant target

### macOS

```bash
./getv/build_mac.sh port       # compile the port layer
./getv/build_mac.sh app        # archive and link
./getv/build_mac.sh lib        # compile game, assets, audio, and port objects
./getv/build_mac.sh all        # lib + app
```

Build Metal into its separate output directory with:

```bash
GETV_RENDERER=metal ./getv/build_mac.sh all
```

### Linux

```bash
./getv/build_linux.sh port
./getv/build_linux.sh app
./getv/build_linux.sh all
```

### Windows

```powershell
.\getv\build_windows.ps1 -Target port
.\getv\build_windows.ps1 -Target app
.\getv\build_windows.ps1 -Target all
```

Every build phase must report `0 failed`. A changed built-object count can also indicate that a
source stopped participating, so compare counts with the baseline instead of checking only the
link result.

## Add focused coverage

Prefer a test in `getv/port/tests/test_<subject>.c` when the root cause can be isolated from the
game. These tests run without a ROM, window, or generated assets and can include the implementation
file directly to exercise private helpers.

Run one group while iterating:

```bash
bash getv/port/tests/run_tests.sh config
bash getv/port/tests/run_tests.sh mouse
```

Then run the complete suite before handoff.

When the behavior requires the running game, use a bounded deterministic scenario. The common
shape is:

```bash
GETV_STAGE=34 GETV_INTROCAM=0 GETV_EXIT_FRAME=301 \
  ./getv/build-mac/goldeneye
```

Add `GETV_SCRIPT`, `GETV_STATE`, or a subsystem-specific self-test gate when useful. A level loaded
without input may still be showing its intro camera, so do not mistake intro measurements for
gameplay.

## Renderer changes

Establish the OpenGL/reference result before deciding a backend is wrong. Keep the stage, scripted
input, window, supersampling, antialiasing, and capture frame identical between comparisons.

Run the render-reference workflow before and after:

```bash
python3 tools/render_refs.py check
```

For a deterministic local screenshot, use `GETV_SHOTFRAME` with `GETV_SHOTPATH` outside the
repository. Never commit captures. If Metal is involved, build and run both renderer directories;
do not reuse stale objects across them.

## Validation matrix

Choose checks based on the paths changed, then run all applicable rows:

| Change | Required validation |
|---|---|
| Documentation only | Inspect rendered Markdown, verify commands/anchors, `git diff --check` |
| Port code | Focused test, full port test suite, relevant platform build |
| Game/decomp patch | Relevant runtime test, full port suite, platform build, `bash tools/check_patches.sh` |
| Fetched Fast3D file | Focused renderer test, `tools/fetch-thirdparty.sh regen`/`verify`, platform build |
| Renderer behavior | OpenGL/Metal or reference comparison plus `python3 tools/render_refs.py check` |
| Cross-platform build code | Run the affected platform or clearly state which platform was not available |

Useful final checks:

```bash
bash getv/port/tests/run_tests.sh
bash tools/fetch-thirdparty.sh verify
git diff --check
git status --short --branch
git diff --stat
git diff
```

Do not claim a platform, renderer, or runtime scenario that you did not run.

## Before committing

Review every changed and untracked path. In particular, reject:

- ROMs and renamed or archived ROMs;
- `base.zip`, extracted assets, generated asset source, texture/audio dumps, and saves;
- screenshots or runtime captures;
- generated binaries and build directories;
- local absolute paths, credentials, and private logs; and
- unrelated cleanup.

Keep the source change and focused test replayable. If a community change also updates
`PATCH_QUEUE.md`, keep that bookkeeping separate from the source-and-test commit. See
[`MAINTAINING.md`](MAINTAINING.md) for the community branch and future-upstream replay model.

## Handoff and review

A useful change report contains:

- problem and root cause;
- smallest implemented change;
- exact commands and results;
- before/after evidence when behavior or rendering changed;
- pre-existing failures and untested platforms; and
- the complete diff scope.

The pull request template mirrors this structure. One issue and one logical fix per pull request
keeps reviews small and preserves the ability to replay community fixes independently.
