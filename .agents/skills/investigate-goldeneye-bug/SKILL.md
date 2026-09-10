---
name: investigate-goldeneye-bug
description: "Investigate a GoldenEye-Native gameplay, crash, rendering, controls or menu, configuration or build failure on Windows, macOS or Linux; reproduce it on a known build, capture bounded local evidence including screenshots, compare hypotheses and hand off a sanitized finding. Use for diagnosis before fixing or reporting, including when a non-technical player describes what they saw. Do not use to publish an issue or implement a fix."
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
reviewed. Investigation does not authorize publication or a code change. When someone offers a
save or game files, decline and continue with steps, logs and screenshots.

## Work with the person who saw it

A player does not need to know commands. Ask short, plain questions, one at a time and only for
what is missing: what they did just before, what they saw, what they expected, how often it
happens and which settings or controls they changed. Offer to run the commands yourself. When the
failure needs a state only a person can reach, give exact numbered steps, launch the game with the
diagnostics you need, and ask them to reproduce it and take a screenshot of the game window.

## Establish the tested state

1. Read the repository policy and the relevant build, configuration, controls and subsystem
   documents.
2. Search current issues and pull requests for the symptom before doing expensive work.
3. Record the full source commit, branch, worktree state, executable path and hash, platform,
   architecture, renderer, difficulty, stage or screen, profile/mod settings, changed controls and
   reproduction frequency.
4. Identify what the build came from: current `main`, a pull-request branch or local changes. A
   failure that appears only on a pull-request branch belongs in that pull request's review, not
   in a new issue.
5. Confirm against current clean community `main` when safe. Distinguish a clean-main failure from
   a local-build or modified-tree failure; do not overwrite local work to obtain the comparison.
6. On Windows, the setup app keeps private `git` and embeddable Python copies under
   `%LOCALAPPDATA%\GoldenEyeNative\bootstrap`, and `python` on `PATH` may be the Microsoft Store
   stub. A clone without symlink support lists `getv/port/include/PR` and
   `getv/port/include/platform_info.h` as modified after setup repairs those links; that alone is
   not a local code change.

## Reproduce and capture

Choose the native launch path for the host:

- **Windows:** use PowerShell and `getv/build-windows/goldeneye.exe`. Set `GETV_LAUNCHER=0` to
  start the game instead of the settings window.
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

On Windows, `Start-Process` redirection keeps logs as plain text; Windows PowerShell's `>` can
write UTF-16, which the bug-report collector rejects. A separate `save_dir` protects the player's
save:

```powershell
$evidence = "$env:USERPROFILE\Goldeneye-Native-Reports\intro-textures"
New-Item -ItemType Directory -Force $evidence | Out-Null
$env:GETV_LAUNCHER = '0'; $env:GETV_EXIT_FRAME = '805'
$env:GETV_SHOTFRAME = '800'; $env:GETV_SHOTPATH = "$evidence\frame-800.bmp"
$game = Resolve-Path .\getv\build-windows
$run = Start-Process "$game\goldeneye.exe" -WorkingDirectory $game -NoNewWindow -Wait -PassThru `
  -ArgumentList "--save_dir=$evidence\save" `
  -RedirectStandardOutput "$evidence\run.log" -RedirectStandardError "$evidence\run.err.log"
$run.ExitCode
```

Use `GETV_STAGE=<id>`, `GETV_INTROCAM=0`, `GETV_EXIT_FRAME=<n>` and deterministic inputs only when
they fit the actual reproduction. A bounded `GETV_EXIT_FRAME` run ignores the keyboard and mouse,
so also set `GETV_KEYBOARD_IDLE=0` whenever a person must press anything. Do not claim that legacy
`GETV_SCRIPT` moves a player; use the player API or a human reproduction until that path is rebuilt.

## Capture what the player sees

Screenshots are strong evidence for rendering, HUD, menu and soft-lock problems. They may be
attached to an issue or pull request after review, but never commit them.

- For a fixed moment, set `GETV_SHOTFRAME=<frame>` and `GETV_SHOTPATH` to a new file outside the
  checkout, with `GETV_EXIT_FRAME` a few frames later. Always set the path: without it the capture
  can land in the game's working directory inside the checkout. Step the frame to find the moment,
  and keep stage, inputs, resolution and quality settings identical across compared captures.
- For a state reached by hand, such as a menu after finishing a mission, ask the person to take a
  screenshot of the game window while the problem is on screen.
- Look at every capture yourself and describe what is visible before relying on it.
- Compare against a reference: the same frame from another renderer, build or platform when one
  exists, or a player-supplied retail capture labelled approximate when the frames differ. Do not
  quote pixel fingerprints between different frames.
- Convert native BMP captures with `tools/collect_bug_report.py`, which writes metadata-free PNGs.
  Review screenshots from other tools for personal details such as other windows or account names,
  and crop those before sharing.

## Controls, menus and input

When input stops working or a menu will not accept a choice:

1. Read the active configuration and the binding table printed at startup, and compare explicit
   bindings with the preset defaults; a saved rebind can remove a default key.
2. Reproduce with `GETV_INPUT_DEBUG=1` and `GETV_KEYBOARD_IDLE=0` while the person presses each
   candidate key, so the trace shows which inputs reach the game.
3. Compare once with default controls and once on current `main` before blaming the engine.

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

Produce a local investigation summary and retain it with the supporting evidence. It contains:

- exact tested identities, including any pull-request branch, and reproduction steps;
- observed exit/crash signature and symbolized frames, or what each screenshot shows;
- comparison matrix with controls and frequency;
- telemetry observations with explicit scope limits;
- facts, hypotheses and ruled-out explanations kept separate;
- one classification: root cause identified, subsystem narrowed, reproducible but missing
  instrumentation, not reproduced, or build/environment mismatch;
- where the finding belongs: a new issue, a review of the pull request whose branch introduced it,
  or a feature request; and
- the smallest next action.

Sanitize with `tools/collect_bug_report.py` when preparing evidence for another person, then inspect
every output. Do not include private paths or raw crash/log files in Git. A later reporting workflow
must search duplicates again and obtain explicit approval before publishing.
