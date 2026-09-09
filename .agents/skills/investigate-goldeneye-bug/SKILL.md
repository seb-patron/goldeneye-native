---
name: investigate-goldeneye-bug
description: "Investigate a GoldenEye-Native gameplay, crash, rendering, configuration or build failure on Windows, macOS or Linux; reproduce it on a known build, capture bounded local evidence, compare hypotheses and hand off a sanitized finding. Use for diagnosis before fixing or reporting. Do not use to publish an issue or implement a fix."
---

# Investigate a GoldenEye-Native bug

Narrow one reported failure with reproducible local evidence. Stop at diagnosis: use
`$report-goldeneye-bug` afterward to prepare a public report, or `$prepare-goldeneye-pr` only when
the user separately asks for a fix.

## Protect game data and user intent

Never copy, move, stage, commit, upload, attach, paste, encode, archive, transmit or link a ROM,
`base.zip`, save, extracted data, generated asset source, texture dump, audio bank or playable
binary. Use a contributor's legally obtained ROM only in its existing local location and avoid
printing its path. Keep logs, reports and captures outside the checkout until sanitized and
reviewed. Investigation does not authorize publication or a code change.

## Establish the tested state

1. Read the repository policy and the relevant build, configuration and subsystem documents.
2. Search current issues and pull requests for the symptom before doing expensive work.
3. Record the full source commit, branch, worktree state, executable path and hash, platform,
   architecture, renderer, difficulty, stage, profile/mod settings and reproduction frequency.
4. Confirm against current clean community `main` when safe. Distinguish a clean-main failure from
   a local-build or modified-tree failure; do not overwrite local work to obtain the comparison.

## Reproduce and capture

Choose the native launch path for the host:

- **Windows:** use PowerShell and `getv/build-windows/goldeneye.exe`.
- **macOS:** use a POSIX shell and `getv/build-mac/goldeneye`, or the launcher-app executable when
  the report is app-specific.
- **Linux:** use a POSIX shell and `getv/build-linux/goldeneye`.

Launch from a terminal with standard output and standard error redirected to a new private text
log outside the repository. For a crash or hang, set `GETV_LOGFLUSH=1`; it can slow execution by
roughly sevenfold, so do not use it for timing comparisons. Preserve the process exit status.
Never print or place the ROM path in the command or log. On macOS, also inspect the matching
`~/Library/Logs/DiagnosticReports/Goldeneye-Native-*.ips` report. On Windows, preserve the native
exception code, fault address, fault PC, registers, stack frames and last runtime mark printed by
the built-in handler. On Linux/macOS, preserve the signal, fault address, fault PC/registers when
available, backtrace and last runtime mark.

Use `GETV_STAGE=<id>`, `GETV_INTROCAM=0`, `GETV_EXIT_FRAME=<n>` and deterministic inputs only when
they fit the actual reproduction. `GETV_KEYBOARD_IDLE=0` is required for manual input during a
bounded run. Do not claim that legacy `GETV_SCRIPT` moves a player; use the player API or a human
reproduction until that path is rebuilt.

## Test hypotheses economically

Start with the crash record, last good marker and top symbolized frames. Change one factor at a
time and repeat the same actions. Prefer the smallest comparisons suggested by evidence: Base
Game versus optional effects, renderer, audio, input device, difficulty or a focused diagnostic
gate. Record controls and negative results.

Prop telemetry is a scoped test, not general crash telemetry. The launcher’s **Developer Tools →
Record telemetry for this launch** or `GETV_PROP_TELEMETRY=1` can show PropRecord occupancy,
allocation/free activity, failures and accounting invariants. A partial file remains useful after
a crash. It does not measure room lists, models, textures, display lists, matrices, audio or
renderer pools; never use a clean report to clear those systems.

Symbolize addresses against the exact executable/build when possible. If evidence is insufficient,
propose the smallest bounded trace at the ownership boundary implicated by the stack; avoid broad
per-frame logging and avoid changing behavior while measuring it.

## Hand off the result

Produce a local investigation summary containing:

- exact tested identities and reproduction steps;
- observed exit/crash signature and symbolized frames;
- comparison matrix with controls and frequency;
- telemetry observations with explicit scope limits;
- facts, hypotheses and ruled-out explanations kept separate;
- one classification: root cause identified, subsystem narrowed, reproducible but missing
  instrumentation, not reproduced, or build/environment mismatch; and
- the smallest next action.

Sanitize with `tools/collect_bug_report.py` when preparing evidence for another person, then inspect
every output. Do not include private paths or raw crash/log files in Git. A later reporting workflow
must search duplicates again and obtain explicit approval before publishing.
