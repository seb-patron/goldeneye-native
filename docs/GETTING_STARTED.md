# Getting started

This is the shortest path from a fresh checkout to playing GoldenEye-Native. The installer fetches
the open-source dependencies, prepares the decompilation, extracts assets from your own ROM, and
builds the native executable. Re-running it is safe and resumes completed work.

You need:

- your own legally dumped US retail GoldenEye 007 ROM (`.z64`, `.n64`, or `.v64`);
- about 4 GB of free disk space; and
- an internet connection for source and dependency downloads during the first install.

No ROM or extracted game data is downloaded, bundled, or uploaded by this project.

## Install

### macOS

Download and unzip the repository, then double-click **Install on Mac**. If Gatekeeper blocks it,
right-click the file, choose **Open**, and confirm once.

The terminal equivalent, run from the repository root, is:

```bash
bash tools/install.sh --rom /path/to/your/rom.z64
```

The ROM path is optional. Without it, the installer searches common locations such as Desktop and
Downloads. The build supports Apple silicon and Intel Macs as separate native targets; see
[`SETUP.md`](SETUP.md) for the complete macOS toolchain and manual pipeline.

### Linux

Install the packages named by the script for your distribution, then rerun the same command:

```bash
bash tools/install.sh --rom /path/to/your/rom.z64
```

Add `--desktop` if you also want a per-user applications-menu entry:

```bash
bash tools/install.sh --rom /path/to/your/rom.z64 --desktop
```

The script never runs `sudo`; it prints the appropriate package-manager command when a system
dependency is missing.

### Windows

Install [Git for Windows](https://git-scm.com/download/win) and
[Python](https://www.python.org/downloads/windows/) first. Enable **Add python.exe to PATH** in the
Python installer. From PowerShell in the repository root, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Rom C:\path\to\rom.z64
```

Omit `-Rom` to let the installer search the usual locations. It downloads its build toolchain into
user-controlled directories and does not alter the system-wide `PATH`.

## Run

The normal OpenGL executables are:

```bash
./getv/build-mac/goldeneye --launcher       # macOS
./getv/build-linux/goldeneye --launcher     # Linux
```

```powershell
.\getv\build-windows\goldeneye.exe --launcher
```

On macOS you can instead double-click **Play GoldenEye**. `--launcher` opens the settings window;
omit it to boot directly with the current configuration.

The application logs its selected renderer, config path, save path, controllers, and resolved
bindings at startup. Those lines are useful when troubleshooting.

### Native Metal on macOS

The standard installer builds OpenGL. To build the separate Metal executable after installation:

```bash
GETV_RENDERER=metal ./getv/build_mac.sh all
./getv/build-mac-metal/goldeneye-metal --launcher
```

OpenGL and Metal use separate build directories, so the two builds do not overwrite each other.

## Configure

The launcher is the easiest way to change video, gameplay, mouse, and controller settings. It
restarts the game after applying changes because most settings are read once during startup.

For a one-off override, use a command-line option:

```bash
./getv/build-mac/goldeneye --resolution=1920x1080 --fullscreen=1
```

For persistent settings, edit `goldeneye.cfg`. The exact file used is printed at startup. You can
also generate a commented template at a path of your choice:

```bash
./getv/build-mac/goldeneye --write-config=/path/to/goldeneye.cfg
./getv/build-mac/goldeneye --config=/path/to/goldeneye.cfg
```

Precedence is:

```text
command line > environment > config file > built-in defaults
```

See [`CONTROLS.md`](CONTROLS.md) for the complete input map and rebinding options, and
[`CONFIGURATION.md`](CONFIGURATION.md) for every setting.

## Rebuild after updating

From the repository root:

```bash
./getv/build_mac.sh all        # macOS OpenGL
./getv/build_linux.sh all      # Linux
```

```powershell
.\getv\build_windows.ps1 -Target all
```

If an update adds or changes a patch or dependency, rerun the installer instead. It detects work
that is already complete and applies the remaining steps in the required order.

## Quick checks

Run the ROM-free port-layer tests:

```bash
bash getv/port/tests/run_tests.sh
```

On Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File getv\port\tests\run_tests.ps1
```

If the game built but will not launch, keep the first meaningful error and the build summary. A
successful build reports `0 failed` for every phase. See [`SETUP.md`](SETUP.md#7-troubleshooting)
for detailed troubleshooting and use the repository's guided issue forms if the problem remains.

Never attach a ROM, save file, `base.zip`, or extracted asset to an issue. Text logs and
screenshots of the running game are welcome.
