# Developer tooling plan

Status: accepted architecture and active delivery plan. The console core, cross-renderer UI/input
ownership, solo pause policy, always-available toggle, initial read-only handlers, and the runtime
`gibs` mutation are implemented. Player/session mutations, the inspector, and native diagnostic
capture remain follow-ons.

Initial priority:

1. Modern in-game console
2. Player and entity inspector
3. One-click local diagnostic bundle

Reproduction recording/playback, same-process quickstates and portable semantic snapshots are
valuable follow-ons, but they are deliberately outside the first delivery milestone.

## Decision

Build a new port-owned developer-tools layer and keep Rare's original debug menu separate.

The original menu is useful historical code and some of its underlying game functions may still
be useful. It is not a suitable console foundation: availability and behavior depend on the
original build, several paths are absent or stubbed in the native port, and its UI was not
designed for searchable commands, structured results, modern input capture, diagnostics or
versioned automation.

The new console should therefore own its parser, registry, result log, input behavior and UI.
A console handler may call a verified original function through a narrow game-side adapter, but
the console must not expose arbitrary memory writes or depend on the original debug-menu UI.

The console UI should be available whenever the optional ImGui dependency is present. The
explicit developer-tools runtime setting controls the observational overlay, not whether the
console hotkey works. Builds without ImGui must continue to work; the command core, schema
validation and tests should not depend on ImGui.

## Why these three features belong together

The console creates controlled actions and a structured history. The inspector turns live game
state into bounded, frame-stable data. The diagnostic bundle captures both in a form the existing
bug-report workflow can validate and explain.

```text
ImGui console ----> command registry ----> game-thread command pump ----> game adapters
tests/headless ----/        |                         |
                            v                         v
                    structured results         settled-frame snapshot
                            |                         |
                            +----------+--------------+
                                       v
                              local diagnostic export
                                       |
                                       v
                         tools/collect_bug_report.py
                                       |
                                       v
                         reviewed bug-report issue draft
```

This is one architecture, not one pull request. Each independently testable layer should land in
a focused PR.

## Current seams to build on

| Existing seam | What it provides | Planning consequence |
| --- | --- | --- |
| `getv/port/src/ge_imgui.{h,cpp}` | An optional in-game ImGui overlay and SDL event feed | Extend this UI boundary; do not put command parsing or game mutations in the renderer file. |
| `getv/port/fast3d/gfx_metal.mm` | Metal-side ImGui device/pass/render helpers | Finish the shared overlay path for Metal rather than creating an OpenGL-only console. The current `ge_imgui.cpp` implementation compiles to stubs under `RAPI_METAL`. |
| `getv/port/src/ge_player_api.h` | Explicit-slot player state, input tick and RNG fingerprint | Use player slots and ticks in results and snapshots; never sample an implicit current-player cursor. |
| `getv/port/src/ge_enemy_api.h` | Live enemies with stable `chrnum` and field-presence masks | Use it as the first live-entity provider and preserve absent-versus-zero semantics. |
| `getv/port/src/ge_world_api.h` | Static objectives, routes, guards, props and doors | Label static world knowledge separately from live entity state. |
| `getv/port/src/ge_event.{h,c}` | A small event bus with derived level/player/room/guard events | Add a bounded event history without replacing authoritative game-side events that may be added later. |
| `getv/port/src/ge_gibs.{h,c}` | A tested policy model initialized from `GETV_GIBS` | Add an explicit runtime setter before exposing `gibs`; changing the environment after its cached first read is not sufficient. |
| `tools/collect_bug_report.py` | Sanitized logs, normalized screenshots, manifest/report generation and local-only output | Import a versioned game-session manifest rather than duplicating the reporting workflow. |
| `tools/check_no_game_data.py` | Publication guard against ROMs, saves, archives and unexpected binary data | Keep exports semantic and reviewable; never put a save, quickstate, raw memory dump or opaque archive in a bundle. |

The current overlay is observational: every SDL event still reaches gameplay. The console needs a
new input-capture contract. That behavior must be explicit and tested because an Enter, Space or
Tab typed into the console must not also fire, skip a cutscene or open the watch.

## Shared rules

These rules apply to all three prioritized workstreams.

### Thread and frame ownership

- ImGui may submit a command, but it must not directly mutate game state.
- A bounded queue carries parsed requests to one documented game-thread command pump.
- The pump runs once per simulation tick at a characterized boundary. Each result records the
  game tick and rendered frame it affected.
- Inspector providers copy state at one documented settled-frame boundary. ImGui and exporters
  read only the copy.
- Queues and snapshots have fixed bounds. Overflow is reported; it must not silently overwrite a
  request or pretend a snapshot is complete.

The first console PR must prove the chosen game-thread hook and write that proof next to the
hook. Renderer callbacks are not assumed to be safe mutation points merely because they happen
to run on one thread in a current build.

### Stable identity and absent data

- Player identity is the explicit slot number.
- Live enemy identity is `(stage_epoch, chrnum)`.
- A future live prop/door identity must include a stage epoch and a stable game identifier or
  generation. A raw address is never an identity.
- Every snapshot family has field-presence flags. Unsupported or unavailable data is absent, not
  serialized as a plausible zero.
- Static world records and live records are labeled by source so a user cannot mistake a spawn
  point for a guard's current position.

### Mutation and multiplayer safety

- Commands declare whether they are read-only or state-changing.
- State-changing commands declare mission, player, solo and determinism requirements.
- Mutating commands are refused during netplay by default. A later synchronized-cheat protocol
  would be a separate design and issue.
- Every player-affecting command takes an explicit slot. Convenience defaults may select slot 0
  only when there is exactly one local player, and the result must state the resolved slot.
- If a game-side adapter temporarily changes an original current-player cursor, it restores the
  prior value before returning, including on error.
- Developer-tool use is visible in session metadata so a diagnostic report never presents a
  modified run as an ordinary one.

### Publication and game-data safety

- Nothing uploads automatically. One click means one click to create a local review directory.
- Never export a ROM, `base.zip`, EEPROM/save file, quickstate, raw memory, extracted asset,
  texture/audio dump or compiled game output.
- Never serialize raw pointers, arbitrary memory ranges, arbitrary game strings or a shell
  command transcript.
- Console evidence is built from parsed command IDs and allowlisted typed arguments. Raw input
  history may contain paths or secrets and is not included by default.
- Outputs are ordinary JSON, JSON Lines, Markdown and a metadata-free PNG in a directory outside
  the repository. Do not create an opaque archive.
- Every bundle remains a local draft until a human reviews every file and approves the exact
  issue submission.

## Workstream 1: modern in-game console

### User experience

A configurable desktop hotkey opens a searchable console whenever the binary includes Dear ImGui;
it does not depend on the optional developer overlay. The default is backquote/grave, with the
launcher/configuration surface exposing the binding and an environment setting remaining useful
for automation.

Opening the console in a solo mission requests a developer-owned pause reason and restores the
previous state when it closes. It must not clear a watch/menu pause that it did not create.
Multiplayer and netplay do not silently pause; the UI explains the active policy, and mutation is
blocked according to command metadata.

The console provides:

- completion and searchable help from registry metadata;
- command history for the current process;
- a bounded scrollback of timestamped structured results;
- clear success, refusal, usage and internal-error states;
- the resolved player, stage and tick for a state-changing action; and
- copy support for a selected safe result, without persisting raw input by default.

The first useful command set is intentionally small:

| Command family | Initial behavior |
| --- | --- |
| `help`, `commands`, `build`, `status` | Registry help, current build compatibility facts and current session facts. |
| `player list`, `player show <slot>`, `where <slot>` | Read through explicit-slot player APIs. |
| `objective list` | Read objective status through a verified accessor; mark unavailable fields honestly. |
| `god <slot> <on|off>` | Apply through a narrow game-side adapter. |
| `give <slot> <weapon>` and `ammo <slot> <amount|full>` | Validate symbolic names/IDs and refuse unsupported mission states. |
| `gibs <off|explosions|high_damage|always>` | Use a new runtime policy setter; never mutate the cached environment indirectly. |
| `restart` and `level <stage>` | Use controlled game transition/relaunch paths, never raw global assignment. |

Free camera and cheat adapters may follow once their existing behavior is verified. A shell,
arbitrary scripting evaluator, memory poke, raw cvar setter and save/quickstate commands are not
part of console v1.

The initial read-only handlers are registered during native boot through a copied provider table.
The provider names all four player slots explicitly and reaches live objectives only through
`gePortObjectiveCount()` / `gePortObjectiveStatus()`; no handler reads `g_CurrentPlayer` or an
objective pointer. `objective list` captures at most ten entries, reports total versus captured,
marks failed provider reads unavailable, and encodes presence/status in bounded typed payloads.
Provider state is sampled only when a command is queued, so an idle closed console does not add a
per-tick player walk.

Useful forms are:

```text
help
help player show
commands
build
status
player list
player show 0
where 0
objective list
gibs <off|explosions|high_damage|always>
```

`gibs` is registered through a copied mutation-provider table and changes the cached port policy
through `gePortGibsSetMode()`; it never rewrites `GETV_GIBS`. Its enum argument and typed
previous/current-mode payload are diagnostic-safe and recordable. The command may set policy
before a mission or during local multiplayer, but the shared command pump refuses it during
netplay before the setter can run. Changing policy preserves existing character/hit bookkeeping
and consumes no gameplay RNG; the existing effect derives its private visual seed from tick and
character identity.

`build` currently reports platform, architecture, renderer, console schema, and command schema.
The authoritative source commit/build-compatibility identifier belongs to the versioned native
`session.json` contract in issue #14; until that exists, this command does not invent one from the
collector checkout or process environment.

### Command contract

The registry is a C-facing port subsystem independent of ImGui. A command definition includes:

- canonical name, aliases, summary and typed argument schema;
- flags such as `read_only`, `mutates_game`, `requires_mission`, `requires_player`, `solo_only`,
  `recordable` and `diagnostic_safe`;
- a handler identifier and optional completion provider; and
- a stable command/schema version for future reproduction playback.

A result includes a monotonically increasing sequence, command ID, status code, severity,
submission and execution tick, rendered frame, resolved target and an allowlisted message or
typed payload. Diagnostic export uses this structure, not the raw line typed by the user.

### Delivery slices

1. ROM-free parser/registry/result-ring library, bounded queue and unit tests.
2. Cross-renderer ImGui console, SDL capture and focus handling.
3. Owned solo pause behavior with explicit multiplayer and netplay policy.
4. Read-only session/player/objective commands through a bounded provider contract.
5. Runtime gibs setter and the initial mutation commands, each with focused tests or harnesses.

These may be separate PRs under one issue. A PR must not mix a renderer fix, several unrelated
game adapters and diagnostic export merely because all are eventually visible in the console.

### Acceptance gate

- The registry and parser have ROM-free tests for valid input, invalid input, quoting, bounds,
  completion metadata and stable result codes.
- Requests execute at the documented game-thread boundary and exactly once.
- Keyboard/mouse input is swallowed only while the console claims it; opening, typing and closing
  do not generate gameplay edges.
- Solo pause ownership restores the prior state; multiplayer/netplay behavior is explicit.
- OpenGL and Metal display and operate the same console, and a no-ImGui build still compiles.
- Initial read-only and mutation commands report resolved targets and refusals accurately.
- Mutating commands are refused in netplay and unsupported stages instead of partially applying.
- With the console closed and developer overlay disabled, no ImGui frame is built and existing
  automated measurement runs remain idle and deterministic.

## Workstream 2: player and entity inspector

### Scope

Inspector v1 is read-only and covers the state already available through honest APIs:

- session: stage, difficulty, game tick, render frame, pause and netplay state;
- all player slots: position, room, heading, health/armour, weapon/ammo and counters when present;
- live enemies: `chrnum`, position, health, alertness and target-belief fields when present;
- objectives and known world records, clearly labeled as static, derived or live; and
- recent structured events plus one selected record's full typed detail.

Props and doors should appear in v1 when the existing world API can identify them, but the UI
must not imply that static placement data is live door state. A future live prop/door adapter can
add fields without changing the inspector's identity and presence rules.

### Snapshot contract

Introduce a port-owned, versioned `GeDebugFrame`-style snapshot with:

- schema version, capture sequence, stage ID, stage epoch, game tick and render frame;
- effective renderer/difficulty/session flags;
- bounded arrays for players, enemies, objectives/world records and recent events;
- field-presence and source flags on every record family; and
- total, captured and truncated counts for every bounded array.

The producer copies at a settled-frame boundary. The UI never retains or dereferences a game
pointer. Switching levels increments the epoch and invalidates selections from the prior stage.
Selection follows stable IDs rather than list indices, which may be reordered or reused.

The JSON diagnostic representation is a projection of this same snapshot. The UI and exporter
must not maintain competing definitions of player/entity state.

### User experience

- Filter by kind, ID and near-player distance.
- Select a player/entity and keep selection while that stable ID remains valid.
- Show absent fields as unavailable and display the provider/source.
- Highlight snapshot truncation and age.
- Copy a safe semantic row or attach the selected record to the next diagnostic capture.
- No edit fields, kill buttons, teleports or arbitrary property writes in inspector v1. Those are
  commands, with command metadata and an audit result, if they are added later.

### Delivery slices

1. Versioned snapshot structures, provider interfaces and fake-provider tests.
2. Player/enemy/objective capture at the settled-frame boundary.
3. ImGui tables, filtering, stable selection and event history.
4. Safe JSON projection consumed later by diagnostics.

### Acceptance gate

- The UI reads only port-owned snapshots and has no retained raw game pointers.
- All four player slots are explicit; no current-player cursor is sampled.
- Enemy selection survives list reordering and is invalidated on stage epoch change.
- Absent fields, static data, derived data and live data are visibly distinct.
- Bounds and truncation are tested and visible.
- OpenGL and Metal show the same data, and capture cost is measured with the inspector closed and
  open.
- A ROM-free fake snapshot drives the complete table/filter/selection model in tests.
- The safe JSON projection rejects raw pointers and unregistered free-form fields.

## Workstream 3: one-click local diagnostic bundle

### Meaning of one click

An in-game **Capture diagnostics** action, plus a console alias, creates one new local directory,
shows its path and states that nothing was uploaded. The directory is ready for human review and
for import by `tools/collect_bug_report.py`.

Publishing an issue remains a separate, explicit approval step. The game must not call GitHub,
open a browser submission or silently attach files.

### Authoritative session manifest

Add a versioned native `session.json`. It is authoritative for the binary and session that were
actually tested:

- diagnostic schema version and capture timestamp;
- binary build commit and build compatibility ID;
- platform, architecture and renderer reported by the running binary;
- stage, difficulty, stage epoch, game tick and rendered frame;
- effective runtime configuration, from an allowlist of non-secret settings;
- developer-tool state and whether mutation commands were used;
- artifact type, filename, byte count and SHA-256; and
- truncation, unavailable-provider and capture-failure reasons.

This corrects two limitations in the current collector. Its repository metadata describes the
checkout running the Python tool, which may not be the checkout that built the game. Its
`GETV_*` scan describes the collector process, which may not have inherited the game's effective
configuration. After session import, the report should name these separately as **tested binary
and session** versus **collector checkout and environment**.

Unknown schema versions fail closed with a useful error. New optional fields may be added within
a version, but changing meaning or accepting a new artifact family requires a schema bump and
tests.

### Bundle contents

The one-click directory may contain only registered, bounded artifacts:

| File | Contents |
| --- | --- |
| `report.md` | Local draft with session facts, capture contents and spaces for actual/expected/reproduction. |
| `manifest.json` | Safety state and hashes for every staged artifact. |
| `session.json` | Authoritative tested-binary and effective-session facts. |
| `commands.jsonl` | Recent diagnostic-safe parsed commands and results; no raw input lines. |
| `events.jsonl` | Recent registered events with tick/frame and typed integer fields. |
| `inspector.json` | One bounded snapshot plus selected record, using the inspector schema. |
| `screenshot.png` | Optional metadata-free scene capture taken before the developer overlay is drawn. |

Missing optional artifacts are represented by a reason in the manifest. General stdout/stderr
logs and crash reports remain optional inputs to the Python collector, which already sanitizes
them. The native exporter should not copy arbitrary files.

If the current native renderer has no safe metadata-free PNG path, add or adapt a separately
reviewed permissive encoder with its notice, or make screenshot completion a focused prerequisite.
Do not call a platform screenshot service that may capture other windows, and do not ship a BMP
as a supposedly publication-ready attachment.

### Collector and skill integration

Extend `tools/collect_bug_report.py` with a session-import option that:

- validates the schema, file set, bounds, hashes, UTF-8/JSON shape and artifact types;
- refuses symlinks, traversal, unknown files, saves, archives and binary payloads;
- treats the running game's manifest as authoritative for tested binary/renderer/stage/settings;
- records collector checkout/system information under a separate key;
- re-runs the publication guard over every imported artifact;
- preserves existing manual log/crash/screenshot inputs; and
- creates a fresh output directory rather than editing the source capture in place.

Update `tools/tests/test_agent_tools.py`, `docs/AGENTIC_CONTRIBUTING.md` and
`.agents/skills/report-goldeneye-bug/SKILL.md` in the focused PR that changes this evidence
contract. The skill should tell a reporter to capture locally, inspect every file, search for a
duplicate and obtain explicit approval for the exact issue body and attachments.

### Acceptance gate

- One in-game action creates a new review directory outside the repository and clearly states
  that it was not uploaded.
- The tested binary commit, renderer, stage/difficulty and effective settings come from the
  running session, not the later collector process.
- Structured command/event/inspector artifacts are bounded, schema-validated and safe by
  construction.
- The screenshot excludes the overlay and is a metadata-free PNG, or the manifest states why no
  screenshot was captured.
- The exporter never includes a ROM, save, quickstate, `base.zip`, extracted data, raw memory,
  arbitrary file or archive.
- Collector tests cover valid import, unknown schema, tampered hash, traversal/symlink, unexpected
  artifact, oversized data, forbidden filename/content and internally inconsistent build/session
  identity fields. A collector-checkout mismatch is allowed and reported, not treated as a bad
  capture.
- The existing manual collector workflow remains compatible.
- The checked-in bug-report skill and contributing documentation match the implemented schema.

## Optional high-value follow-ons

Do not schedule these as implementation issues until the three prioritized workstreams have real
use and measurements.

### Reproduction recording and playback

This becomes useful, rather than speculative, once commands have stable IDs, state has stable
identity and diagnostics know the tested build. A shareable recording should contain only
per-tick controller intent, frame delta, synchronized recordable commands, RNG fingerprints and
versioned metadata. It must not contain a save, raw memory or extracted game data.

The go/no-go question is practical: after console, inspector and bundles are used on real bugs,
are important reports still hard to reproduce from their structured evidence? If yes, prototype
recording against deterministic test runs and require a playback divergence report. If no, keep
the smaller tools.

The current publication checker rejects unexpected binary formats and archives. A future
shareable recording needs an explicitly validated text/JSON representation or a deliberate,
tested safety-policy update. Do not create an opaque `.gerepro` and bypass the checker.

### Same-process quickstates

These can speed local iteration but should initially be ephemeral, same-process and same-build.
They require a subsystem save/restore registry, explicit invalidation and proof that pointers,
renderer resources, audio state and external handles are not restored as stale bytes. They are
never a bug-report attachment.

### Portable semantic snapshots

A portable snapshot would restore named semantic state through versioned adapters rather than
copying RAM. It is much more expensive and can still fail to recreate AI, scripts, timers and
renderer state. Consider it only if deterministic playback and local quickstates leave a measured
diagnostic gap. A read-only semantic snapshot used as evidence is already provided by the
inspector; restoration is a separate problem.

## Delivery and coordination

Use one tracking issue, three child issues and a proposed `Developer tooling v1` milestone. The
repository currently has the standard `enhancement` and `documentation` labels but no dedicated
developer-tools label; use existing labels initially rather than creating taxonomy for four
issues.

Recommended order:

1. Land console registry/queue and cross-renderer UI/input behavior.
2. Land verified read-only and mutation adapters.
3. Land the read-only snapshot model and inspector.
4. Land native diagnostic export, then collector/schema/skill integration.

Before starting each PR, recheck current issues, PRs and local platform work. Console UI/input is
especially likely to overlap launcher, SDL input and renderer changes. Rebase onto current `main`
and keep each PR at one abstraction boundary.

The existing bot-focused `docs/ROADMAP.md` should receive only a short link after this plan is
accepted; duplicating this architecture there would create two sources of truth.

## Review decisions

Approve this plan only if the maintainer agrees with all of the following:

- the modern console is port-owned and separate from Rare's original debug-menu UI;
- initial mutation commands are narrow, explicit-slot and blocked in netplay;
- console opening uses owned pause semantics and never leaks typed input into gameplay;
- OpenGL and Metal are both part of console/inspector acceptance, while no-ImGui builds remain
  supported;
- the inspector is read-only and snapshot-based, with stable IDs and honest absent fields;
- one-click diagnostics means local capture, not automatic publication;
- the running session is authoritative for tested build/configuration metadata;
- diagnostic exports never contain saves, quickstates, raw memory, archives or arbitrary files;
- the three workstreams may use several focused PRs instead of one large implementation PR; and
- reproduction playback and state restoration remain optional until real usage demonstrates the
  additional value.

If any of those decisions should change, revise this document and the corresponding issue draft
before creating GitHub issues. Once accepted, create the milestone and tracking issue first, add
the assigned issue numbers to the child drafts, then publish the three child issues.
