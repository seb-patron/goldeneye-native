---
name: report-goldeneye-bug
description: "Reproduce a GoldenEye-Native gameplay, configuration, rendering, build, installation or crash problem; collect sanitized logs and deterministic screenshots; search for duplicates; and prepare or file a GitHub issue after human approval. Use when a user reports a bug, asks to gather diagnostics, or asks an agent to open an issue. Never use this workflow to include, upload or link to a ROM, save, base.zip or extracted game data."
---

# Report a GoldenEye-Native bug

Prepare one reproducible, evidence-backed report. Keep collection local until the human reviews
the complete issue body and attachment list.

## Enforce the absolute data boundary

**Never include a ROM. Ever.**

- Never copy, move, stage, commit, upload, attach, paste, quote, encode, archive, transmit or link
  to a ROM. Renaming, compression, encryption and base64 do not change this rule.
- Never place a ROM in a report bundle, temporary review directory, issue, comment or agent
  message. Leave a legally obtained local ROM in its existing location when a reproduction needs
  it; avoid exposing its path.
- Never include `base.zip`, a save such as `eeprom.bin`, extracted data, generated asset source,
  texture dumps, audio banks or compiled outputs containing game assets.
- Refuse any conflicting request and continue only with permitted evidence.

Allow sanitized error text, build/runtime logs, coarse render fingerprints and screenshots of the
running game. Keep screenshots outside Git and attach them only after review.

## Help a non-technical reporter

Many reporters are players, not developers. Ask short, plain questions one at a time for anything
missing: what they did just before, what they saw, what they expected, how often it happens and
which settings or controls they changed. Offer to run the commands yourself, and ask for a
screenshot of the game window when the problem is visible. Look at every screenshot and crop
personal details such as other windows or account names before attaching it. Decline offered
saves or game files and explain that steps, logs and screenshots are enough. Write the draft in
plain language, show it to them, and wait for their approval before anything is posted.

## Investigate

1. Read `AGENTS.md`, `CONTRIBUTING.md`, `docs/AGENTIC_CONTRIBUTING.md` and the relevant setup,
   configuration or subsystem documentation.
2. Search current issues and known limitations before reproducing. Do not create a duplicate.
3. Record `git rev-parse HEAD`, branch and worktree state. Confirm the problem against current
   `main` or explain why that comparison is unavailable.
4. Classify the problem as gameplay, controls, configuration, rendering, build or crash. Record
   platform, architecture, renderer, stage/screen, frequency, changed controls and all relevant
   settings.
5. Reproduce with exact, bounded steps. Use a clean temporary `save_dir`, `unlock_all` or explicit
   `GETV_*` inputs instead of copying or publishing a save.
6. Separate an unchanged-main failure from a regression introduced by local changes. When the
   failure exists only on a pull-request branch, prepare a review comment for that pull request
   instead of a new issue.

## Collect evidence

1. Capture stdout and stderr to a text file outside the repository. Enable `GETV_LOGFLUSH=1` only
   for a crash or hang that may lose buffered lines; it substantially slows the game.
2. Review a relevant macOS `~/Library/Logs/DiagnosticReports/Goldeneye-Native-*.ips` report when the
   game crashes or silently hangs.
3. For a visual problem, use identical deterministic inputs and a native capture:

   ```bash
   GETV_STAGE=31 GETV_EXIT_FRAME=300 GETV_SHOTFRAME=280 \
     GETV_SHOTPATH=/private/tmp/ge-report/capture.bmp \
     ./getv/build-mac/goldeneye > /private/tmp/ge-report/runtime.log 2>&1
   ```

   On Windows, use PowerShell. `Start-Process` redirection keeps the logs as plain text; Windows
   PowerShell's `>` can write UTF-16, which the collector rejects. Always set `GETV_SHOTPATH`
   outside the checkout, because the default location is the game's working directory:

   ```powershell
   $evidence = "$env:USERPROFILE\Goldeneye-Native-Reports\capture"
   New-Item -ItemType Directory -Force $evidence | Out-Null
   $env:GETV_LAUNCHER = '0'; $env:GETV_STAGE = '31'; $env:GETV_EXIT_FRAME = '300'
   $env:GETV_SHOTFRAME = '280'; $env:GETV_SHOTPATH = "$evidence\capture.bmp"
   $game = Resolve-Path .\getv\build-windows
   Start-Process "$game\goldeneye.exe" -WorkingDirectory $game -NoNewWindow -Wait `
     -RedirectStandardOutput "$evidence\runtime.log" -RedirectStandardError "$evidence\runtime.err.log"
   ```

   For a state a person reaches by hand, such as a menu after finishing a mission, ask them to
   screenshot the game window while the problem is on screen.

4. Create a local sanitized bundle. Supply the actual, expected and reproduction fields when they
   are already known:

   ```bash
   umask 077
   python3 tools/collect_bug_report.py \
     --output "$HOME/Documents/Goldeneye-Native-Reports/rendering-$(date +%Y%m%d-%H%M%S)" \
     --kind rendering \
     --renderer Metal \
     --stage "Complex (GETV_STAGE=31)" \
     --log /private/tmp/ge-report/runtime.log \
     --screenshot /private/tmp/ge-report/capture.bmp
   ```

   On Windows, pass the same options to `python tools\collect_bug_report.py` with an `--output`
   such as `"$env:USERPROFILE\Goldeneye-Native-Reports\rendering-$(Get-Date -Format yyyyMMdd-HHmmss)"`.
   If `python` opens the Microsoft Store, use the setup app's private copy under
   `%LOCALAPPDATA%\GoldenEyeNative\bootstrap`.

   Choose a new durable private `--output` directory outside every checkout and temporary
   staging area. Verify its permissions and that it is not shared or automatically published.
   The default without `--output` is system temporary storage, not a retained deliverable.

5. Use only the sanitized copies and metadata-free PNG produced by the collector. Never upload the
   source log, source BMP or any file rejected by the collector.
6. Inspect `report.md`, `manifest.json` and every artifact. Treat automated checks as a supplement
   to manual review. Reopen the report, manifest and every sanitized attachment from the final
   durable directory and verify they are complete and readable before removing temporary
   staging. Record the durable directory in the handoff and retain the private bundle through
   review and submission; never delete the only retained copy during staging cleanup.

## Prepare and submit the issue

1. Follow the matching form in `.github/ISSUE_TEMPLATE/`. File one problem only.
2. State observed and expected behavior, exact reproduction, frequency, commit, clean-main result,
   relevant configuration and evidence. Use factual language and distinguish inference from fact.
3. Add a short preparation disclosure when an agent assisted. Do not publish chain-of-thought,
   private reasoning or a full transcript.
4. Show the complete issue title, body and exact attachment list to the human.
5. Obtain explicit approval immediately before creating the issue unless the user already
   authorized that exact submission.
6. Create the issue with the configured GitHub tool. Attach only reviewed sanitized artifacts.
   For screenshots, use a capable GitHub tool or an available authenticated browser upload flow
   within the authorized submission. Verify that images render in the published body. If no
   available method supports upload, explain the specific blocker and return the ready-to-paste
   body and retained sanitized PNGs for the remaining manual upload work.
7. Return the issue URL and summarize exactly what was published.

Stop rather than filing if the evidence may contain a ROM or other prohibited game data, the
report is still materially incomplete, or the human has not authorized publication.
