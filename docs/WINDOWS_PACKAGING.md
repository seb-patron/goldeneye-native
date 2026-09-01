# Windows setup package

This work produces a **non-playable setup candidate**, not a prebuilt game. `goldeneye.exe`
contains the assets extracted from the builder's ROM, so this project does not package or upload
it. `GoldenEye-Native-Setup.exe` is deliberately different: it contains the setup UI and local ROM
verifier/importer, then builds the playable executable on the user's own Windows computer from a
ROM they supply.

The end-user download and first-run instructions are in
[`WINDOWS_INSTALL.md`](WINDOWS_INSTALL.md). This document is for maintainers producing and testing
that setup package.

That is an engineering boundary, not a legal conclusion. This repository has unresolved licensing
questions recorded in [`LICENSING.md`](LICENSING.md), including source with no declared licence.
Maintainer and, where appropriate, legal review are still required before publicly releasing any
artifact. The package checks below show what bytes are absent; they do not grant distribution
rights.

## What the user gets

The package contains four files:

- `GoldenEye-Native-Setup.exe` — a self-contained Windows x86-64 setup application;
- `SHA256SUMS.txt` — the executable's checksum;
- `README.txt` — the first-run instructions and data-safety boundary; and
- `THIRD_PARTY_NOTICES.txt` — the SDL2, Dear ImGui, and GLEW notices required alongside the
  statically linked executable.

The setup application:

1. downloads pinned PortableGit and embeddable Python archives into the current user's LocalAppData
   folder and verifies their upstream SHA-256 values before using them;
2. asks for an installation folder and clones the public source there;
3. opens a normal file picker for a user-supplied `.z64`, `.v64`, or `.n64` dump;
4. detects the dump's byte order, normalizes it to z64 in a temporary local file, and verifies the
   normalized US-ROM SHA-1;
5. replaces the app-managed ROM copy only after that verification succeeds;
6. downloads the remaining build tools, extracts assets, and builds on that computer; and
7. launches the game's settings window when setup completes.

The selected ROM is never uploaded. The source file is opened read-only, and a failed conversion
does not alter it or leave a partial file at the path trusted by the build.

The user does not install Git, Python, or a compiler and does not use a terminal. The private Git
and Python copies are not registered system-wide, do not need administrator privileges, and are
placed under `%LOCALAPPDATA%\GoldenEyeNative\bootstrap`. Existing verified downloads are reused.

Pinned bootstrap inputs (last verified 2026-09-01):

- [PortableGit 2.55.0.3 x64](https://github.com/git-for-windows/git/releases/tag/v2.55.0.windows.3),
  SHA-256 `ab00566336b5472120f9a52d34f2e79c5406535792acb0548001ffd0bd090e5d`;
- [Python 3.14.7 embeddable package x64](https://www.python.org/downloads/release/python-3147/),
  SHA-256 `d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15`.

An update changes both the versioned URL and expected digest in source. The installer never follows
a mutable `latest` URL for an executable archive.

## Build the package on Windows

From a clean source checkout in PowerShell:

```powershell
.\tools\package_windows_wizard.ps1
```

The script fetches only the wizard dependencies, builds a statically linked executable, runs its
ROM byte-order self-test, checks that it imports only Windows system DLLs, scans for representative
game/asset/renderer markers, and writes the package under `dist\windows\`.

To rebuild after the dependencies are already present:

```powershell
.\tools\package_windows_wizard.ps1 -SkipDeps
```

No ROM is used by either command.

## CI artifact

The **Package Windows setup** GitHub Actions workflow runs the same packaging command on a native
Windows runner. It can be started manually and also runs when its build, wizard, or packaging files
change. A successful run uploads `GoldenEye-Native-Windows-Setup-<commit>` as a 30-day artifact.
It does not create a release or publish an executable automatically; a tested artifact can be
promoted deliberately after review. The workflow embeds the source repository and branch that
triggered it, so a feature-branch artifact clones that feature branch instead of silently testing
the setup pipeline from `main`. A release candidate should use a reviewed, immutable tag.

The first artifact is intentionally described as a test package. It is not Authenticode-signed,
so SmartScreen may call it an unrecognized app. Testers should obtain it only from this repository's
workflow run and compare its SHA-256 with `SHA256SUMS.txt`. A broad public release needs an explicit
code-signing decision in addition to a successful functional test.

## First Windows test pass

Test the artifact on a Windows 10 or 11 x86-64 machine that does **not** have Git for Windows,
Python, MinGW, or this repository installed:

1. Run `GoldenEye-Native-Setup.exe` from a folder containing no other DLLs.
2. Choose a new, empty destination outside the source checkout.
3. Confirm setup downloads and verifies its private PortableGit and Python copies without an admin
   prompt or a system-wide install.
4. Complete one install using a supported US dump the tester is permitted to use. Record whether
   it was z64, v64, or n64; do not record its path or attach it anywhere.
5. Confirm the wizard says the format was recognized and, for v64/n64, converted locally.
6. Confirm all four build groups report `0 failed`, **Launch GoldenEye** opens the launcher, and a
   mission starts.
7. Close and reopen `getv\build-windows\goldeneye.exe --launcher` to verify the built result does
   not depend on the setup executable remaining open.
8. Rerun setup into the same completed checkout to verify the resumable path and cached tool reuse.

If it fails, use **Copy the log** and keep only build output in the report. Never attach a ROM,
generated asset source, extracted asset, save, or the locally built `goldeneye.exe`.
