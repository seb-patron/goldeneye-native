# Installing and playing on Windows

This is the no-code path for someone who wants to try GoldenEye-Native on Windows. If you want to
edit the project or build from a source checkout, use [`BUILDING.md`](BUILDING.md) instead.

## Current status

The Windows setup app is an unsigned **test candidate**, not a published release yet. It supports
64-bit Windows 10 and 11. Obtain a candidate only from this repository's **Package Windows setup**
workflow; do not use an executable reposted elsewhere.

The downloaded setup package is not the game and cannot play without a ROM. It contains no ROM,
extracted game assets, decompiled game source, or playable `goldeneye.exe`. The playable program
is built only on your computer after you select your own supported cartridge dump.

That technical separation is not legal advice. You are responsible for using a ROM and the
resulting local build in a way permitted where you live. See [`LICENSING.md`](LICENSING.md) for
the project's unresolved source-licensing questions.

## What you need

- A Windows 10 or 11 x86-64 computer with internet access.
- Your own supported US GoldenEye 007 cartridge dump that you are permitted to use. The file may
  be in `.z64`, `.v64`, or `.n64` byte order.
- About 4 GB of free disk space and 10 to 40 minutes for the first build.

You do **not** need to install Git, Python, a compiler, or this source repository. Setup uses
private, checksum-verified portable tools under your Windows user profile and does not need
administrator access.

## Download the test candidate

1. Open this repository's **Actions** tab and select **Package Windows setup**.
2. Open the successful run for the commit or branch you were asked to test.
3. Under **Artifacts**, download `GoldenEye-Native-Windows-Setup-<commit>`.
4. Extract the downloaded ZIP into a new folder. Keep these four files together:
   `GoldenEye-Native-Setup.exe`, `SHA256SUMS.txt`, `README.txt`, and
   `THIRD_PARTY_NOTICES.txt`.

GitHub may require you to sign in before downloading a workflow artifact. A future reviewed
release can put the same package on the Releases page; until then, use the workflow artifact.

## Install and play

1. Double-click `GoldenEye-Native-Setup.exe`.
2. Choose a new, empty installation folder.
3. Select your ROM when the normal Windows file picker opens.
4. Confirm that setup recognizes and verifies the file. It normalizes v64/n64 byte order into a
   separate local copy; it does not modify the file you selected.
5. Leave setup open while it downloads the public source and build tools, extracts the required
   data locally, and builds the playable program.
6. When all build groups report `0 failed`, click **Launch GoldenEye**.

Afterward, launch the game from `getv\build-windows\goldeneye.exe --launcher` inside the folder
you chose. Re-running setup in that same folder is the supported way to resume a stopped build;
verified downloads and completed steps are reused.

## Windows SmartScreen

The test candidate is not Authenticode-signed, so SmartScreen may identify it as an unrecognized
app. Confirm that it came from the expected workflow run and compare its SHA-256 with
`SHA256SUMS.txt` before choosing **More info** and **Run anyway**. Do not bypass a warning for a
copy obtained from somewhere else.

## If setup fails

Use **Copy the log** in the setup window and include that text in a new issue. Never attach or
paste your ROM, its path, generated assets, save files, or the locally built `goldeneye.exe`.

The maintainer/tester checklist is in [`WINDOWS_PACKAGING.md`](WINDOWS_PACKAGING.md).
