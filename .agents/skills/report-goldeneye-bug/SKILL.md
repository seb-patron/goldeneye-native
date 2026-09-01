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

## Investigate

1. Read `AGENTS.md`, `CONTRIBUTING.md`, `docs/AGENTIC_CONTRIBUTING.md` and the relevant setup,
   configuration or subsystem documentation.
2. Search current issues and known limitations before reproducing. Do not create a duplicate.
3. Record `git rev-parse HEAD`, branch and worktree state. Confirm the problem against current
   `main` or explain why that comparison is unavailable.
4. Classify the problem as gameplay, configuration, rendering, build or crash. Record platform,
   architecture, renderer, stage/screen, frequency and all relevant settings.
5. Reproduce with exact, bounded steps. Use a clean temporary `save_dir`, `unlock_all` or explicit
   `GETV_*` inputs instead of copying or publishing a save.
6. Separate an unchanged-main failure from a regression introduced by local changes.

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

4. Create a local sanitized bundle. Supply the actual, expected and reproduction fields when they
   are already known:

   ```bash
   python3 tools/collect_bug_report.py \
     --kind rendering \
     --renderer Metal \
     --stage "Complex (GETV_STAGE=31)" \
     --log /private/tmp/ge-report/runtime.log \
     --screenshot /private/tmp/ge-report/capture.bmp
   ```

5. Use only the sanitized copies and metadata-free PNG produced by the collector. Never upload the
   source log, source BMP or any file rejected by the collector.
6. Inspect `report.md`, `manifest.json` and every artifact. Treat automated checks as a supplement
   to manual review.

## Prepare and submit the issue

1. Follow the matching form in `.github/ISSUE_TEMPLATE/`. File one problem only.
2. State observed and expected behavior, exact reproduction, frequency, commit, clean-main result,
   relevant configuration and evidence. Use factual language and distinguish inference from fact.
3. Add a short preparation disclosure when an agent assisted. Do not publish chain-of-thought,
   private reasoning or a full transcript.
4. Show the complete issue title, body and exact attachment list to the human.
5. Obtain explicit approval immediately before creating the issue unless the user already
   authorized that exact submission.
6. Create the issue with the configured GitHub tool. Attach only reviewed sanitized artifacts. If
   image upload is unavailable, return the ready-to-paste body and staged PNG for manual upload.
7. Return the issue URL and summarize exactly what was published.

Stop rather than filing if the evidence may contain a ROM or other prohibited game data, the
report is still materially incomplete, or the human has not authorized publication.
