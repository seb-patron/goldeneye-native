# Documentation

Choose the path that matches what you are trying to do.

## Play and configure

| Guide | Use it for |
|---|---|
| [`GETTING_STARTED.md`](GETTING_STARTED.md) | Install, launch, configure, update, and run a first check on macOS, Linux, or Windows. |
| [`CONTROLS.md`](CONTROLS.md) | Complete keyboard/mouse map, gamepad defaults, rebinding, control styles, and live shortcuts. |
| [`CONFIGURATION.md`](CONFIGURATION.md) | Full config-file, command-line, environment-gate, and launcher reference. |
| [`SETUP.md`](SETUP.md) | Detailed manual macOS pipeline and deep build troubleshooting. |
| [`FAQ.md`](FAQ.md) | Common player and project questions. |
| [`CHEATS.md`](CHEATS.md) | GoldenEye's built-in named cheat system. |

## Contribute

| Guide | Use it for |
|---|---|
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | Contribution rules, game-data boundary, provenance, review scope, and evidence expectations. |
| [`AGENTIC_CONTRIBUTING.md`](AGENTIC_CONTRIBUTING.md) | Safe agent-assisted bug reports and pull requests. |
| [`CODEBASE.md`](CODEBASE.md) | Architecture, build/runtime/input flows, repository map, and where a change belongs. |
| [`DEVELOPMENT.md`](DEVELOPMENT.md) | Branch, edit, rebuild, test, validate, and review loop. |
| [`../getv/patches/README.md`](../getv/patches/README.md) | Safely record changes to the ignored decompilation. |
| [`HARNESS.md`](HARNESS.md) | Deterministic runtime harness and automated scenarios. |
| [`TASK_QUEUE.md`](TASK_QUEUE.md) | Current task inventory and evidence state. |

## Features and subsystems

| Guide | Subject |
|---|---|
| [`MODDING.md`](MODDING.md) | Lua mods, environment gates, asset seams, and HD texture packs. |
| [`MOUSE.md`](MOUSE.md) | Mouse-look design, measurements, and tests. |
| [`FRAME_TIMING.md`](FRAME_TIMING.md) | Decoupled simulation and rendering clocks. |
| [`PERFORMANCE.md`](PERFORMANCE.md) | Performance profiles and measurements. |
| [`BOTS.md`](BOTS.md) | Bot architecture and skill policies. |
| [`COOP.md`](COOP.md) | Cooperative mission behavior and limitations. |
| [`NETPLAY.md`](NETPLAY.md) | LAN implementation and known synchronization failures. |
| [`GIBS.md`](GIBS.md) | Opt-in enemy-gib implementation and policies. |
| [`STANCE.md`](STANCE.md) | Crouch, stance, jump, and lean design notes. |
| [`COLOUR_BUGS.md`](COLOUR_BUGS.md) | Texture/colour decoding investigations. |

## Internal interfaces and ports

| Guide | Subject |
|---|---|
| [`PLAYER_API.md`](PLAYER_API.md) | Player state and control API used by bots, netplay, and automation. |
| [`ENEMY_API.md`](ENEMY_API.md) | Live enemy accessor surface. |
| [`ASSET_LOADING.md`](ASSET_LOADING.md) | ROM asset conversion and runtime loading. |
| [`asset-converter-spec.md`](asset-converter-spec.md) | Asset converter contract. |
| [`PORTING.md`](PORTING.md) | Platform-port history, constraints, and build baselines. |
| [`WINDOWS_STAN_ORDERING.md`](WINDOWS_STAN_ORDERING.md) | Windows geometry-ordering root cause and fix. |
| [`PERFECT_DARK.md`](PERFECT_DARK.md) | Audited opportunities from the MIT-licensed Perfect Dark port. |

## Direction and maintenance

| Guide | Subject |
|---|---|
| [`ROADMAP.md`](ROADMAP.md) | Current state, known issues, and planned work. |
| [`VISION.md`](VISION.md) | Long-term project direction. |
| [`MAINTAINING.md`](MAINTAINING.md) | Community branch workflow and future upstream replay. |
| [`../PATCH_QUEUE.md`](../PATCH_QUEUE.md) | Independently replayable community fixes. |
| [`REUSE_AUDIT.md`](REUSE_AUDIT.md) | Evaluated reuse candidates and license boundaries. |

## Licensing and provenance

| Guide | Subject |
|---|---|
| [`LICENSING.md`](LICENSING.md) | Repository-wide provenance and permitted sources. |
| [`THIRD_PARTY.md`](THIRD_PARTY.md) | Fetched dependencies, versions, and redistribution constraints. |
| [`../getv/port/PROVENANCE.md`](../getv/port/PROVENANCE.md) | File-level port-layer origin record. |

If two documents disagree about a command or setting, treat the executable/script help and current
source as authoritative, then fix the stale document. Documentation changes are welcome.
