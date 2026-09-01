# Codebase guide

GoldenEye-Native is a native port, not an emulator. The decompiled game is compiled for the host
CPU and connected to a platform layer that replaces the N64 operating system, input, audio,
storage, and graphics interfaces.

The repository intentionally does not contain the complete build tree. Setup combines three
pieces locally:

1. this repository's tracked port, patches, build scripts, tests, and documentation;
2. a fetched `n64decomp/007` checkout under the ignored `vendor/ge-decomp/`; and
3. fifteen fetched sm64ex/Fast3D files, reconstructed from a pinned revision and this project's
   tracked patch.

Your own ROM supplies assets during setup. The ROM, extracted assets, generated source, and build
outputs remain untracked and must never be committed.

## Repository map

| Path | Responsibility |
|---|---|
| `getv/Sources/ge_tvos_main.c` | Shared SDL/game harness and runtime boot sequence for desktop and Apple targets. |
| `getv/port/mac/ge_mac_main.c` | Small desktop `main()` wrapper. Despite the path, Linux and Windows use it too. |
| `getv/port/src/` | Native platform services: config, input, N64 OS shims, audio, saves, launcher, mods, netplay, bots, and diagnostics. |
| `getv/port/fast3d/` | Fast3D display-list frontend plus OpenGL/Metal rendering and SDL window integration. Some files are fetched and ignored; see below. |
| `getv/port/audio/` | Software audio backend inherited through the port layer. |
| `getv/port/tests/` | ROM-free unit tests. Each test includes the unit it exercises so static helpers can remain private. |
| `getv/patches/` | Versioned changes applied to the ignored decompilation checkout. |
| `getv/patches/thirdparty/` | Manifest and patch for the fifteen fetched sm64ex/Fast3D files. |
| `getv/build_*.sh`, `getv/build_windows.ps1` | Platform build drivers. |
| `tools/install.*` | End-to-end dependency, asset, patch, and build orchestration. |
| `tools/render_refs.py` | Deterministic renderer capture/reference checks. |
| `tools/playtest.py` | Runtime scenarios that need a built game and local assets. |
| `docs/` | Player, developer, subsystem, provenance, and maintenance documentation. |
| `vendor/ge-decomp/` | Ignored local game-source checkout. Never treat edits here as recorded until they are represented in `getv/patches/`. |

## Build pipeline

`tools/install.sh` and `tools/install.ps1` encode the full setup order:

```text
fetch pinned sources
        |
        v
apply source and third-party patches
        |
        v
validate and convert the user's local ROM
        |
        v
generate local assets and apply the asset-stage patch
        |
        v
compile game + assets + port layer
        |
        v
archive normal objects into libge.a
        |
        v
link the harness entry objects + renderer + libge.a into goldeneye
```

The entry objects stay outside `libge.a` because a static archive member containing `main()` would
not be pulled in to satisfy an existing undefined symbol. The archive is also intentional: it lets
the linker ignore N64-only archive members that no native path references.

Build outputs are isolated by platform and renderer, including `getv/build-mac/`,
`getv/build-mac-metal/`, `getv/build-linux/`, and `getv/build-windows/`.

## Runtime flow

On desktop platforms, startup follows this sequence:

1. `main()` in `getv/port/mac/ge_mac_main.c` calls `geConfigInit()` before any subsystem caches a
   setting.
2. The optional launcher reads those resolved settings, writes overrides, and restarts the process
   when necessary.
3. `SDL_main()` in `getv/Sources/ge_tvos_main.c` installs crash reporting, initializes port
   stubs, loads Lua mods, and registers game-side accessor functions.
4. `gfx_init()` creates the SDL window and selects OpenGL or Metal.
5. The harness initializes the game's main-thread data, graphics buffers, audio, and message
   queues, then enters `bossMainloop()`.
6. Game code produces N64 display lists. `port_render.c` hands them to Fast3D, which translates
   commands and submits them to the selected rendering backend.
7. SDL pumps window/input events and presents the rendered frame.

This explains an important ordering rule: config and launcher work must happen before the first
`getenv()` consumer. Many settings are cached on first use and cannot be changed reliably inside an
already-running process.

## Input flow

Input deliberately travels through the same path regardless of device:

```text
SDL keyboard / mouse / gamepad
        |
        v
GePadState in port_input.c
        |
        v
bindings + deadzone + dual-analog conversion in port_os.c
        |
        v
OSContPad (the N64 controller shape)
        |
        v
GoldenEye's original joy.c and control-style logic
```

Scripted and automated input is injected at the `GePadState` boundary. This keeps tests and
reproductions on the production decode path. See [`CONTROLS.md`](CONTROLS.md) for player-facing
behavior.

## Configuration flow

`getv/port/src/ge_config.c` is the source of truth for friendly config keys. It translates them
into the existing `GETV_*` environment gates used by subsystem consumers:

```text
command line > environment > goldeneye.cfg > built-in default
```

The launcher writes the same gates and restarts the executable. Raw `GETV_*` names remain an
escape hatch for diagnostics and experiments. See [`CONFIGURATION.md`](CONFIGURATION.md).

## Where a change belongs

| Change | Primary location | Special rule |
|---|---|---|
| Config parsing or public setting | `getv/port/src/ge_config.c` and usually `ge_launcher.cpp` | Update `CONFIGURATION.md` and tests. |
| Keyboard, mouse, controller discovery | `getv/port/src/port_input.c` | Keep device input independent of game logic. |
| Action mapping, N64 controller emulation, clock/OS shims | `getv/port/src/port_os.c` | Add a ROM-free focused test when possible. |
| Native service such as saves or audio | `getv/port/src/port_*.c` | Preserve platform-independent behavior and explicit diagnostics. |
| OpenGL/Metal output | `getv/port/fast3d/` | Determine whether the file is tracked or fetched before editing. Compare both backends. |
| GoldenEye gameplay logic | `vendor/ge-decomp/src/` locally | Record the final change in a numbered `getv/patches/` patch. |
| Generated asset source | `vendor/ge-decomp/assets/` locally | Never commit generated data; only the narrowly permitted asset-stage patch paths belong in `0002`. |
| Lua mod | `mods/<name>/` | Keep it independent of generated game data. |
| Build/install behavior | `getv/build_*` or `tools/install.*` | Keep macOS/Linux/Windows twins aligned where the behavior is shared. |

Before editing a file under `getv/port/fast3d/`, check whether Git tracks it:

```bash
git ls-files --error-unmatch getv/port/fast3d/path.c
```

If it is one of the fifteen ignored fetched files, regenerate its tracked patch with:

```bash
tools/fetch-thirdparty.sh regen
```

If you modify the ignored decompilation, read [`getv/patches/README.md`](../getv/patches/README.md)
before editing. That document explains patch ordering, safe regeneration, and the ROM-derived paths
that must remain excluded.

## Tests and diagnostics

- `getv/port/tests/` covers pure port behavior without a ROM or window.
- `tools/playtest.py` and the `GETV_SCRIPT`/`GETV_EXIT_FRAME` gates exercise deterministic runtime
  behavior with a local build.
- `tools/render_refs.py` captures stages and compares coarse render fingerprints.
- `GETV_INPUT_DEBUG`, `GETV_STATE`, and other raw gates expose machine-readable runtime state.
- Build scripts print per-phase object counts so missing sources cannot pass silently.

Use [`DEVELOPMENT.md`](DEVELOPMENT.md) for the day-to-day edit/build/test loop and
[`CONTRIBUTING.md`](../CONTRIBUTING.md) for contribution policy, provenance, and pull-request
expectations.
