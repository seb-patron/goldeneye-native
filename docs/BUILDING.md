# Building from source

This guide is for developers and advanced users working from a source checkout. If you only want
to try the game on Windows, use the no-code [`WINDOWS_INSTALL.md`](WINDOWS_INSTALL.md) path.

A source build needs development tools, internet access, about 4 GB of free disk space, and your
own supported US GoldenEye 007 cartridge dump. The repository never supplies a ROM. Do not commit,
upload, or include a ROM, extracted asset, save, or locally built playable executable in an issue,
branch, pull request, or build artifact.

## Get the source

```bash
git clone https://github.com/seb-patron/goldeneye-native.git
cd goldeneye-native
```

Use the command for your platform below. Each source-build script fetches the public dependencies,
prepares the decompilation, imports the ROM locally, generates the assets, and builds the native
program. It is resumable: fix a reported prerequisite and run the same command again.

## Windows developer build

Install Git for Windows and Python 3 first, then open PowerShell in the checkout:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Rom C:\path\to\your-rom.z64
```

The script accepts `.z64`, `.v64`, and `.n64` byte orders and downloads the MinGW toolchain and
libraries used by this project. Subsequent builds can be run with:

```powershell
.\getv\build_windows.ps1 -Target all -Mingw C:\mingw64
```

The separate end-user setup app and its ROM-free packaging checks are documented in
[`WINDOWS_PACKAGING.md`](WINDOWS_PACKAGING.md).

## macOS or Linux developer build

Install the prerequisites reported for your platform, then run:

```bash
bash tools/install.sh --rom /path/to/your-rom.z64
```

Subsequent builds use one of:

```bash
./getv/build_mac.sh all       # macOS
./getv/build_linux.sh all     # Linux
```

The exhaustive manual/reference procedure in [`SETUP.md`](SETUP.md) currently follows the macOS
arm64 build in detail. The automated installer above is the authoritative entry point for Linux.

## Verify the build

A successful build reports `0 failed` for every build group. The generated executable is under
`getv/build-windows/`, `getv/build-mac/`, or `getv/build-linux/`, depending on the platform.

Before changing code, read [`CONTRIBUTING.md`](../CONTRIBUTING.md), the repository-level
`AGENTS.md` when using an agent, and the relevant subsystem documentation. The existing
[`DEVELOPMENT.md`](DEVELOPMENT.md) describes the project's cross-machine integration workflow;
it is not an installation guide.
