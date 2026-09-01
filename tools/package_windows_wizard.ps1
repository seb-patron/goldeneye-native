<#
  Produce the non-playable Windows setup candidate without touching a ROM or the decompilation.

  The output is intentionally setup_wizard.exe, not goldeneye.exe. The latter contains the
  user's extracted game assets after a local build and cannot be distributed. The wizard is a
  small bootstrapper made only from project-owned setup/icon code plus SDL2, GLEW and Dear ImGui.

      powershell -NoProfile -ExecutionPolicy Bypass -File tools\package_windows_wizard.ps1
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\package_windows_wizard.ps1 -SkipDeps
#>
[CmdletBinding()]
param(
  [string]$Mingw = 'C:\mingw64',
  [string]$OutputDirectory = '',
  [string]$RepoUrl = 'https://github.com/seb-patron/goldeneye-native.git',
  [string]$RepoRef = 'main',
  [switch]$SkipDeps
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'dist\windows' }

if (-not $SkipDeps) {
  & (Join-Path $PSScriptRoot 'fetch_deps_windows.ps1') -Mingw $Mingw -WizardOnly
}

& (Join-Path $root 'getv\build_wizard.ps1') -Mingw $Mingw -RepoUrl $RepoUrl -RepoRef $RepoRef

$built = Join-Path $root 'getv\wizard\build\setup_wizard.exe'
if (-not (Test-Path $built)) { throw "wizard build did not produce $built" }

# Exercise the byte-order detector and normalizer in the actual Windows executable being shipped.
$selfTest = @(& $built --self-test 2>&1)
$selfTest | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0 -or ($selfTest -join "`n") -notmatch 'self-test passed') {
  throw "setup_wizard.exe ROM import self-test failed (exit $LASTEXITCODE)"
}

# The portable-tool script is embedded in the executable so the user still downloads one file.
# Ask the built artifact to emit those exact bytes, then have Windows PowerShell parse them. This
# catches a C++ raw-string edit that compiles cleanly but would fail only after a user clicked
# Continue. Parsing does not download or execute anything from the script.
$bootstrapProbe = Join-Path $env:TEMP 'goldeneye-native-bootstrap-parse-test.ps1'
try {
  & $built --write-bootstrap-script $bootstrapProbe | ForEach-Object { Write-Output $_ }
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bootstrapProbe)) {
    throw "setup_wizard.exe could not emit its portable-tool bootstrap script"
  }
  [void][scriptblock]::Create((Get-Content -LiteralPath $bootstrapProbe -Raw))
  Write-Output "portable-tool bootstrap script: PowerShell syntax ok"
} finally {
  Remove-Item -LiteralPath $bootstrapProbe -Force -ErrorAction SilentlyContinue
}

# A sudden size jump is the quickest signal that a game/archive library was linked accidentally.
# The known static wizard is about 3-4 MB; 8 MB leaves toolchain headroom while remaining smaller
# than the 12 MB ROM by itself.
$size = (Get-Item $built).Length
if ($size -ge 8MB) {
  throw "setup_wizard.exe is unexpectedly large ($([math]::Round($size / 1MB, 1)) MB); refusing to package it"
}

# Scan printable strings for representative symbols unique to the decompilation, generated assets,
# and Fast3D. build_wizard.ps1 also has a fixed three-source allowlist and checks DLL imports; these
# checks make an accidental redistribution fail in CI instead of relying on a reviewer noticing it.
$stringsExe = Join-Path $Mingw 'bin\strings.exe'
if (-not (Test-Path $stringsExe)) { throw "no strings.exe at $stringsExe" }
$binaryStrings = @(& $stringsExe -a $built)
$forbidden = @(
  'file_resource_table',
  '__codeSegmentRomStart',
  'bg_sev_all_p_seg',
  'GfxRenderingAPI',
  'gfx_pc.c',
  'gfx_opengl.c'
)
foreach ($needle in $forbidden) {
  if ($binaryStrings -contains $needle -or ($binaryStrings -match [regex]::Escape($needle))) {
    throw "setup_wizard.exe contains forbidden game/renderer marker: $needle"
  }
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$packageExe = Join-Path $OutputDirectory 'GoldenEye-Native-Setup.exe'
Copy-Item -LiteralPath $built -Destination $packageExe -Force
$notices = Join-Path $root 'getv\wizard\THIRD_PARTY_NOTICES.txt'
if (-not (Test-Path $notices)) { throw "missing wizard third-party notices: $notices" }
Copy-Item -LiteralPath $notices -Destination (Join-Path $OutputDirectory 'THIRD_PARTY_NOTICES.txt') -Force

$readme = @"
GoldenEye-Native setup for Windows
==================================

1. Double-click GoldenEye-Native-Setup.exe.
2. Choose an empty installation folder.
3. Select your own supported US GoldenEye 007 cartridge dump that you are permitted to use.
   .z64, .v64, and .n64 byte orders are accepted.
4. Leave the setup window open while it builds, then click Launch GoldenEye.

You do not need to install Git, Python, or a compiler. Setup downloads private portable copies,
verifies their upstream SHA-256 values, and keeps them under your Windows user profile.

This package installs source from:
$RepoUrl
ref: $RepoRef

The ROM is checked, normalized, and copied locally. It is never uploaded. This package contains
no ROM, extracted game assets, decompiled game code, or playable game binary; the playable binary
is built only on your computer from the ROM you select.

This technical separation is not legal advice or a conclusion that public distribution is
permitted. Review docs\LICENSING.md and resolve the recorded licensing questions before release.

This test package is not code-signed yet, so Windows SmartScreen may identify it as an unrecognized
app. Verify that it came from the Actions run for the repository above and that its SHA-256 matches
SHA256SUMS.txt. Broad release should wait for the Windows test pass and code-signing plan.

If setup fails, use Copy the log and report it at:
https://github.com/seb-patron/goldeneye-native/issues
Never attach your ROM, extracted assets, or save files.
The notices for libraries linked into the setup app are in THIRD_PARTY_NOTICES.txt.
"@
Set-Content -LiteralPath (Join-Path $OutputDirectory 'README.txt') -Value $readme -Encoding ASCII

$hash = (Get-FileHash -LiteralPath $packageExe -Algorithm SHA256).Hash.ToLower()
Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') `
  -Value "$hash  GoldenEye-Native-Setup.exe" -Encoding ASCII

Write-Output ""
Write-Output "Windows setup package: $OutputDirectory"
Write-Output "  GoldenEye-Native-Setup.exe  $([math]::Round($size / 1MB, 1)) MB"
Write-Output "  SHA-256: $hash"
