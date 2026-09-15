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

The launcher profile regression compiles the production model and profile function without
SDL, ImGui or game data:

```bash
python3 tools/tests/test_launcher_model.py
```

Use `--cxx /path/to/g++` for a compiler outside PATH and `--source-root /path/to/checkout`
to check an unchanged baseline. Twelve checks cover Base Game restrictions, compatible display
settings, retained inactive choices and GoldenEye+ customization. The launcher-policy CI job
also runs the config group to check enforcement after command-line parsing. Launcher screenshots
still need a desktop build or a ROM-free harness linked to SDL/ImGui.

The save-directory regression compiles the production `geSavePathInit` initializer with
synthetic environment and filesystem helpers. It requires Python 3 and a C compiler, checks
11 scenarios with and without macOS diagnostics, and never opens a real save or reads the
host home directory:

```bash
python3 tools/tests/test_save_paths.py -v
```

The public-artifact-safety CI job runs it directly; missing compiler/source, compilation errors
and fewer than 11 successful scenarios per variant fail the check. This tests path selection
and directory-creation failure handling, not game execution or save serialization.

The desktop mouse-capture regression exercises the actual SDL event-handler and mouse-polling
source sections and the controller/no-controller polling branch with simulated device state
and real SDL2 headers. It covers simultaneous controller movement and mouse look/buttons,
controller detach/reconnect, player isolation and script priority, as well as capture recovery
with and without a controller. Discovery and script adapters are simulated; this does
not test physical USB hot-plug or device-specific mappings. It needs a C compiler, Python,
SDL2 headers and the reconstructed third-party window source, but no SDL library, game source,
ROM, generated assets or live window:

```bash
bash tools/fetch-thirdparty.sh   # fresh checkout only; do not overwrite local edits
python3 tools/tests/test_mouse_capture.py
```

Use `--sdl-include /path/to/SDL2` for headers outside the usual install prefixes. Missing source,
headers, compiler, skipped scenarios or zero checks are failures. The dedicated Linux CI job runs
this command; OS focus and windowed/fullscreen capture still need interactive platform checks.

The same ROM-free regression runs on Windows with the standalone MinGW toolchain and SDL2
headers. The runner supplies the Windows environment compatibility header and keeps SDL's
main wrapper disabled, so it needs no SDL library:

```powershell
python tools/tests/test_mouse_capture.py --cc C:\mingw64\bin\gcc.exe --sdl-include C:\mingw64\include\SDL2
```

The mouse runner also checks modern displacement, reversals, event batching, pause/focus
discard, controller coexistence and classic fallback. After applying the numbered source
patches, `python3 tools/tests/test_modern_mouse_game.py --cc gcc` checks the actual game-side
input gates, angle application, zoom and pitch limits with synthetic player state. It requires
patched game source but no ROM or generated assets. Both run in `mouse-capture-regression.yml`.

The game-header multi-ammo regression has a separate source-only runner. From a fresh
checkout, with Git, Python 3 and a C compiler installed:

```bash
python3 tools/test_multi_ammo.py --cc cc
```

On Windows use the supported standalone WinLibs MinGW toolchain from
`tools/fetch_deps_windows.ps1`, without running the game installer:

```powershell
python tools/test_multi_ammo.py --cc C:\mingw64\bin\gcc.exe
```

The runner fetches pinned decomp commit `c4356466796c697dfd298010b9bed261f9ed8c6a`
into a temporary directory, checks out `src`, `include`, and the source-controlled
`assets/images.def` enum definition list required by `bondconstants.h`, and applies the
source/header hunks of the numbered patches (excluding the generated-asset patch). It needs no ROM, SDL,
extracted assets, generated game-data source or reconstructed renderer files. For offline use,
pass `--source-repo /path/to/local/ge-decomp`; only the pinned committed source is used.
Temporary sources and the synthetic test executable are removed on exit.

The test includes the actual patched `bondtypes.h`. Invented host-endian setup words check
the empty sentinel with zero quantity, unequal nonempty fields and four-byte size/array stride.
To reproduce the negative control, run this same runner with
`--patch-root /path/to/unchanged-base-checkout`: before patch 0024, little-endian hosts report
four field mismatches, including empty-slot quantity 65535. Compilation failure is not a valid
negative control. The dedicated Linux/Windows CI workflow runs this narrow regression; it does
not establish crate pickup or full-game behavior. The whole-game forced declaration include is
intentionally omitted because it requires generated assets; native/endian declaration flags
and the Windows MinGW bitfield ABI flag are retained.

The model-slot lifecycle regression replays the source patches onto the same pinned decomp source
and compiles the production native slot metadata contract against synthetic storage:

```bash
python3 tools/test_model_slots.py --cc cc
```

It checks that ownership and rwdata backing remain outside the `Model` overlay, release preserves
rwdata eligibility, a released slot can be selected again, and pool lookup accepts only exact
elements. The dedicated Linux/Windows workflow requires all checks to execute without a ROM,
generated assets, a renderer or a live game process.

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

## macOS app renderer selection

`build_mac.sh app` and `bundle` compile a ROM-free bootstrap that immediately opens the existing
custom launcher. Renderer selection lives in **Video**, alongside the graphics controls.
OpenGL renders this settings UI when installed, independently of the selected game renderer.
A Metal-only installation uses its Metal launcher and reports the missing OpenGL option.
The only separate UI is an error recovery dialog after an unsuccessful process launch/exit.

Both generated apps discover the exact sibling `build-mac/goldeneye` and
`build-mac-metal/goldeneye-metal` pair; neither installs the other renderer. No PATH search or
fallback to an unrelated binary is used. Keep both sibling folders when moving the installation.
Availability is checked before opening the launcher and again before game handoff.

App-only override: `open GoldenEye.app --args --app-renderer=metal` (or `gl`). Precedence is
explicit app override, saved renderer, OpenGL default. Only explicitly changing the renderer in
Video writes the `org.goldeneyenative.renderer` preferences domain. Overrides, the unset default,
and recovery do not rewrite a saved choice. Invalid preferences require an available selection.

The app supplies private `GETV_MAC_APP_*` path/selection/capability values and
`GETV_MAC_RENDERER_APP` to its child, preserving other environment values and arguments.
`GETV_MAC_APP_CONFIG_DIR` keeps config lookup consistent across executables. Direct binary,
`--launcher`, and CLI/headless behavior is unchanged. The custom launcher's exec preserves
settings and switches to the selected executable only when starting a game.

Run `bash tools/tests/run_renderer_app_tests.sh` on macOS for native app policy, separate-process
preference persistence, unavailable builds/devices, overrides, handoff and shared-config tests.
Run `python3 tools/tests/test_macos_launcher_app.py` for portable packaging tests. Native UI,
both actual game executables and failed-initialization acceptance remain necessary before #52
closes; Metal qualification and the eventual default flip belong to #53.
