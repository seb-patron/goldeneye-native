# CI trust and proposed merge protection

This is issue #46's first local baseline: ownership, explicit job names, action pins and token
permissions, plus a settings proposal. **No GitHub settings are activated by this change.** Host
acceptance remains pending; this baseline does not complete issue #46.

## Observed repository settings

Live API observations on 2026-09-07, before this change:

- Public repository; default branch `main`.
- `main` unprotected: branch-protection GET returned 404; repository rulesets returned `[]`.
- Actions default workflow permissions: `read`. Workflows can explicitly request broader
  permissions; Pages does so.
- `can_approve_pull_request_reviews: false`: Actions cannot create approving PR reviews. This is
  an Actions review-permission setting, not an own-PR-only restriction.
- Fork approval policy: `first_time_contributors`. Returning contributors are not automatically
  subject to this approval gate; it is a contributor heuristic, not a review of executable code.
- GET for the `github-pages` environment returned 404. A main-only environment deployment
  restriction has not been verified and must not be assumed active.

Recheck these settings before activation. See GitHub's
[repository Actions settings](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/enabling-features-for-your-repository/managing-github-actions-settings-for-a-repository).

## Executed and fetched inputs

All workflows use pinned `actions/checkout` with `persist-credentials: false`. PR workflows
explicitly request only `contents: read`. The inventory below follows the currently invoked
scripts, rather than assuming every native test includes production code.

| Workflow | Executed or fetched inputs |
|---|---|
| `collision-hull.yml` | Checks out `n64decomp/007` at `c4356466796c697dfd298010b9bed261f9ed8c6a`; applies top-level `getv/patches/0*.patch` except `0002-*`; installs Clang and sanitizer runtime through apt. `tools/tests/test_collision_hull.py` extracts a function from patched `src/game/chrprop.c` and compiles it with synthetic declarations/stubs. |
| `cuff-switch-regression.yml` | `getv/port/tests/run_tests.sh cuff` compiles `test_cuff_switch.c`, whose includes are only standard-library headers. Then the workflow fetches the same pinned decomp and applies the same top-level patch selection. `tools/tests/test_cuff_switch.py --cc gcc` extracts the production function from patched `src/game/bondview2.c` and compiles it with synthetic declarations. |
| `joy-poll-handshake.yml` | Linux, macOS and Windows jobs check out `n64decomp/007` at `c4356466796c697dfd298010b9bed261f9ed8c6a`; apply the top-level source patches through `0030`, excluding generated-asset patch `0002-*`; retain a temporary unpatched `src/joy.c` negative control; and then apply `0031-native-joy-poll-handshake.patch`. `tools/tests/test_joy_poll_handshake.py` extracts the relevant production joy functions and native message-queue functions, then compiles them with a synthetic schedule harness using system GCC or Clang on Linux and macOS and the SHA-256-pinned WinLibs archive on Windows. The jobs reproduce the vulnerable native schedule and verify the repaired native and preserved non-native paths. They execute pull-request-controlled workflow, Python, C and patch content with `contents: read`; no ROM or generated assets are fetched. |
| `launcher-policy-regression.yml` | Runs the header-only startup policy and config groups, including Base Game enforcement after CLI parsing. `tools/tests/test_launcher_model.py` compiles the actual launcher model and profile function with synthetic settings and requires 12 successful checks. These tests use standard-library headers; they do not fetch a decompilation, ROM, generated assets, SDL, or ImGui. |
| `mouse-capture-regression.yml` | Installs SDL2 headers and GCC through apt. `tools/fetch-thirdparty.sh` fetches sm64ex at `d7ca2c04364a6dd0dac58b47151e04e26887e6f0` and reconstructs its fifteen manifest paths using `getv/patches/thirdparty/0001-getv-port-layer.patch`. `tools/tests/test_mouse_capture.py` extracts the production mouse and keyboard sections, controller/no-controller polling branch and SDL event handler, compiling `getv/port/tests/mouse_capture_harness.c` against those sections, real SDL headers, port input/accumulator headers and console ownership code. SDL device state, discovery, idle/script adapters and unrelated window callbacks are simulated. Modern mouse displacement and classic fallback run with and without a controller. The job also runs the config group, checks out the pinned decomp source, applies top-level source patches except `0002-*`, and runs `test_modern_mouse_game.py` against its input gates and angle application using synthetic player state. No SDL library, live window, ROM or generated assets are used. |
| `multi-ammo-regression.yml` | `tools/test_multi_ammo.py` fetches the pinned decomp's source, headers and committed `assets/images.def` identifier list; applies only source/header hunks from top-level `0*.patch`, excluding `0002-*`. It compiles `getv/port/tests/game/test_multi_ammo_layout.c` against patched `bondtypes.h` and its header dependencies. Windows downloads a pinned WinLibs archive and verifies its SHA-256. |
| `model-slot-regression.yml` | `tools/test_model_slots.py` fetches the same pinned decomp source, headers and committed image-ID list; applies the same source/header patch selection; and compiles `test_model_slot_lifecycle.c` against the production native slot metadata declarations. Synthetic storage checks ownership, rwdata persistence and eligibility, reuse, and exact pool membership. Windows downloads the same SHA-256-pinned WinLibs archive as multi-ammo CI. |
| `prop-allocator-telemetry.yml` | Requires and runs the ROM-free `test_prop_allocator_telemetry.c` (including synthetic local report I/O) and `test_developer_tools.c` (launcher recording/input policy) groups, which include the production telemetry module and fails on silent or zero-check execution. It checks out `n64decomp/007` at `c4356466796c697dfd298010b9bed261f9ed8c6a`; `tools/check_patches.sh` clones that committed source, applies every top-level source patch except generated-asset patch `0002-*`, and verifies registration in all setup scripts. Its Windows job uses the same SHA-256-pinned WinLibs archive as multi-ammo CI and runs the required report-I/O and launcher-policy tests, including Unicode filenames and collision handling. |
| `rgba16-byte-order-regression.yml` | Runs `tools/test_rgba16_byte_order.py --cc gcc` on Ubuntu 24.04. The script extracts the committed decoder byte-swap/default, renderer mode/default and RGBA16 texel read from the two patch files, compiles them with four invented texel checks, executes the temporary harness, and fails unless exactly four checks run. It uses the runner's Python and GCC; it does not fetch a decompilation, third-party renderer, ROM or generated assets. |
| `macos-renderer-app.yml` | Compiles the tracked AppKit bootstrap and native Objective-C test harness with system Clang/Cocoa/Metal on macOS. Tests synthetic executables and an isolated temporary preferences domain, plus the production config unit. No game build, ROM or extracted assets. |
| `public-artifact-safety.yml` | Runs `tools/check_no_game_data.py --tracked` and `tools/tests/test_agent_tools.py` via unittest discovery. Tests import `check_no_game_data`, `collect_bug_report`, and `compare_render_fingerprints`, which transitively imports `render_refs`. `tools/tests/test_check_patches.py` executes `tools/check_patches.sh` against synthetic complete and promisor Git repositories, redirects its upstream URL to a local synthetic repository, and permits only the local file protocol. Also runs `tools/tests/test_macos_launcher_app.py` directly: six required packaging tests execute `getv/build_mac.sh` and `tools/make_macos_launcher_app.py` in synthetic build folders with stub compiler commands and an inert executable. Also runs `tools/tests/test_save_paths.py`, compiling the production save-path initializer with synthetic environment, stat and directory helpers; no real saves are accessed. No game data, network access, compiler download or macOS window is needed. |
| `pages.yml` | Pinned configure/upload/deploy Pages actions publish `site/`. No game build or asset extraction runs. |
| `windows-setup-package.yml` | Pull requests and manual dispatches build and test the ROM-free setup wizard with `contents: read`. The workflow has no automatic tag trigger, uploads no artifact, and has no publishing job or write permission. A manual dispatch may target any selected ref but still only validates inside the runner workspace. `tools/fetch_deps_windows.ps1 -WizardOnly` downloads SHA-256-pinned WinLibs, SDL2, GLEW, and Dear ImGui archives; it does not fetch the decompilation or a ROM. Validation runs the tracked-game-data guard, byte-order self-test, embedded-bootstrap syntax check, import inspection, and forbidden-string scan. |

Only the mouse-capture regression applies `getv/patches/thirdparty/*`. The ROM-free Windows
packaging workflow invokes `tools/fetch_deps_windows.ps1` in `-WizardOnly` mode; the separate
multi-ammo Windows regression shares its WinLibs release pin.

The public-artifact-safety job also runs `tools/tests/test_skill_eval.py`: required, ROM-free
unit tests for the simulated screenshot workflow, grading, trace provenance, and local MCP stdio
protocol. The test entrypoint fails on zero tests or skips. It starts only the local Python
simulator, with synthetic artifact IDs; it does not invoke Codex, use model credentials, upload
images, or publish to GitHub. Live model evaluations are opt-in local runs described in
[`SKILL_EVALS.md`](SKILL_EVALS.md); a green CI harness test is not a new model evaluation.

The same workflow runs `tools/tests/test_skill_eval_evidence.py` against temporary synthetic Git
repositories. Its separate `skill-eval-evidence-linux` job checks out the exact PR head (or push
commit), with full history and no persisted credentials, and runs
`tools/check_skill_eval_evidence.py`. This requires new committed result records/reports/manifests
for skill changes, verifies changed skill fingerprints and relevant scenario coverage, and
replays the submitted records with the current committed evaluator. It does not invoke a model
or execute a historical evaluator. The original safety job still tests the GitHub merge checkout;
the separate evidence job uses the PR head to avoid comparing merge-checkout bytes to head blobs.
If a squash merge leaves an evaluated commit outside the checked-out history, the gate fetches
that exact validated 40-character commit ID from the fixed `origin` remote before replay. It
never obtains a remote URL or command from an evidence record. No branch-protection or GitHub
ruleset settings are changed by adding this check.

There are currently no local composite/reusable actions, PR artifact handoffs to a privileged
job, shared PR/deployment caches, or `pull_request_target` workflows.

`.github/CODEOWNERS` names `@seb-patron` as default owner and explicitly covers CI, tools, patches,
tests, port source/headers, site content and policy documents. Port source/header and third-party
patch coverage is conservative maintenance coverage, not a claim that these jobs execute all of
those paths. CODEOWNERS routes review; enforcement requires the proposed review rule. See
[GitHub code owners](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/about-code-owners).

## Proposed settings — not active

In repository **Settings → Rules → Rulesets**, target `main` with both rulesets below.

| Ruleset | Rules | Bypass |
|---|---|---|
| A: main checks and history | Require status checks, with the branch up to date before merge; block force pushes; restrict deletions. | Empty: applies to everyone, including admins. |
| B: main review | Require a PR, one approval, Code Owner approval, dismissal of stale approvals after new commits, and resolved review conversations. | Repository Admin role, **For pull requests only**. |

Ruleset B's bypass is an explicit solo-maintainer exception. It bypasses **B's rules**, including
review/conversation requirements; it is not a second review. PR-only mode still requires a PR and
does not permit direct pushes. Ruleset A independently preserves checks and history protections.
Record the reason and validation in a bypassed PR, and inspect its PR record and rule history.
Do not put required checks in the bypassable ruleset. See GitHub's
[ruleset creation and bypass settings](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/creating-rulesets-for-a-repository)
and [available rules](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/available-rules-for-rulesets).

Require these exact check-run names in **A**, with the expected **GitHub Actions** app
(`github-actions`) selected as source:

- `collision-hull-linux`
- `cuff-switch-linux`
- `launcher-policy-linux`
- `multi-ammo-linux`
- `multi-ammo-windows`
- `public-artifact-safety-linux`

These are job names alone, without a workflow-name prefix. Live check-runs at base commit
`4fab6d45ddd9d5e62e9bc0b86be6f2fcace1f908` reported `collision-hull`, `windows`, `linux`,
`no-game-data`, and another `linux`, all from `github-actions`. The new names must be observed on
actual runs, with their app and commit SHA checked, **before activating A's check requirements**.
`pages-deploy` is excluded: it is a deployment job, not a PR test.

For initial activation, land this baseline under the existing process, observe successful renamed
jobs, then configure A with those observed names/source and B as above. For future renames,
coordinate the rule replacement with an administrator: keep valid required checks passing,
observe the replacement jobs on the relevant revision, and replace obsolete contexts without
leaving an impossible requirement or removing unrelated checks. If overlap is needed, retain the
old job temporarily until its replacement is observed. A rerun cannot manufacture an old context
that the workflow no longer emits. Keep job names unique across workflows.

In **Settings → Actions → General**, retain read-only defaults and disabled Actions PR approvals.
Do not treat either setting as an execution-admission control. Dismissing stale approvals resets
review after a changed diff; it does not scan code for malicious behavior.

## Pages boundary

The workflow retains its path-filtered push-to-`main` and manual-dispatch triggers. Its deploy job
requires `github.ref == 'refs/heads/main'`, so a manual dispatch on a non-main ref is refused by
skipping the deploy job. Workflow permissions default to `contents: read`; only the deploy job
requests `contents: read`, `pages: write` and `id-token: write`. Its checkout does not persist
credentials. See [workflow conditions and permissions](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax).

The YAML guard is editable and is not tamperproof. Before relying on hosted deployment isolation,
verify or activate a `github-pages` environment restriction allowing only the `main` branch,
then test it. This remains a proposed settings action, given the observed environment 404. See
[environment deployment restrictions](https://docs.github.com/en/actions/how-tos/deploy/configure-and-manage-deployments/manage-environments).

## Deferred host acceptance

After explicit authorization for settings changes and a small, safe set of host checks, record
commit SHAs, check names/apps, expected/actual outcomes and PR/rule-history links for these cases:

- A missing or failing required check blocks even a solo admin's PR merge.
- A solo-maintainer PR with passing checks can use B's bypass; the exception is visible in the
  PR/rule history.
- An ordinary contributor PR requires owner review; a new commit invalidates stale approval and
  unresolved conversations prevent an ordinary merge.
- Direct push, force push and branch deletion are rejected. Use safe disposable equivalent-rule
  targets for potentially destructive probes; do not risk `main` to demonstrate a rejection.
- Real runs report all six exact required names from `github-actions` on the expected revision.
- An ordinary returning-contributor fork PR automatically executes the existing tests, documenting
  the residual exposure under the current approval policy.
- Manual Pages dispatch on a non-main branch produces no deployment; verify the YAML guard and
  the separately configured environment boundary.

These checks have **not** run as part of this local baseline. No flood, quota experiment or
malicious payload is needed.

## Updating dependencies and remaining limits

For action updates, verify the full commit SHA against the official action repository and review
its release/diff; update the SHA and version comment together across affected workflows. For a
decomp pin or WinLibs update, review upstream source/release changes, update the revision or
URL/checksum and all duplicated pins, and run affected ROM-free tests on their supported CI
platforms. Check reported names/source before merging; use the coordinated migration above for
name changes. No dependency upgrade is included here.

Pinned action/source commits and a verified archive digest reduce drift, but do not establish
that their contents are safe. Hosted runner images and apt-provided compilers/runtime packages
are not immutable; record actual versions when diagnosing changes. Revisit this inventory and
ownership when adding actions, scripts, imports, compiler inputs or pin files.

Merge review does not approve execution before it happens. Returning-contributor workflows and
PR-edited tests/workflows can execute before merge under the observed policy. An expected app
source narrows who can report a required check; it does not prove that PR-edited workflow/test
code is trusted or prevent every same-app substitution. This baseline adds no trusted admission
gate, hard resource quota, routing, timeout or runner redesign. Those remain separate work.
