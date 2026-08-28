<#
  Unit tests for the port layer.

  These run WITHOUT the game. That is the whole point: the bugs this directory exists for were
  invisible from outside a running session -- input was accepted, the bot moved, and the only
  symptom was the direction it moved in. Anything that needs a level loaded belongs in
  tools/playtest.py instead.

  Each test is a standalone .c that includes the unit under test directly, so it can reach static
  functions without those being made non-static purely to suit a test.

  Usage:
    .\run_tests.ps1                     # build and run every test_*.c here
    .\run_tests.ps1 -Filter intent      # just the ones whose name contains "intent"
#>
param(
  [string]$Mingw  = 'C:\msys64\mingw64',
  [string]$Filter = ''
)

# 'Continue', not 'Stop', for the same reason build_windows.ps1 says so: PowerShell turns a native
# program's stderr into an ErrorRecord, and under 'Stop' the first compiler note aborts the run and
# looks like a script bug. Exit codes and output files are the only trustworthy signal here.
$ErrorActionPreference = 'Continue'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$getv = Split-Path -Parent (Split-Path -Parent $here)
$gcc  = Join-Path $Mingw 'bin\gcc.exe'
$out  = Join-Path $here '_bin'

if (-not (Test-Path $gcc)) { throw "gcc not found at $gcc -- pass -Mingw <prefix>" }

# THE MINGW BIN DIRECTORY MUST BE ON PATH, even though gcc is invoked by full path. gcc.exe is a
# driver: it spawns cc1.exe, and it finds cc1 via PATH. Without this, `gcc --version` works, every
# real compile exits 1, and NOTHING is printed -- a valid file fails exactly like a broken one.
#
# That signature cost three separate wrong conclusions on this machine, including "this
# environment cannot compile" written down twice before the real cause was found. An empty
# failure is the symptom; this line is the cure.
if ($env:PATH -notlike "*$Mingw\bin*") { $env:PATH = "$Mingw\bin;$env:PATH" }

New-Item -ItemType Directory -Force -Path $out | Out-Null

# Mirrors $portFlags in build_windows.ps1. The forced-include of ge_win_compat.h is required:
# without it the decomp's PR headers do not supply s32/u16/OSContPad and every file fails on the
# base types, which reads as a broken toolchain rather than a missing flag.
$flags = @(
  "-I$getv\port", "-I$getv\port\include", "-I$getv\port\fast3d", "-I$getv\port\src",
  '-include', "$getv\port\include\ge_win_compat.h",
  "-I$Mingw\include\SDL2",
  '-DTARGET_N64','-DGE_PORT_NATIVE','-D_LANGUAGE_C=1','-DRAPI_GL','-DWAPI_SDL2',
  '-DGE_PLATFORM_DESKTOP',
  '-std=gnu17','-Wall','-O1'
)

$tests = @(Get-ChildItem -Path $here -Filter 'test_*.c' | Sort-Object Name)
if ($Filter) { $tests = @($tests | Where-Object { $_.Name -like "*$Filter*" }) }
if ($tests.Count -eq 0) { Write-Host 'no tests matched'; exit 0 }

$pass = 0; $fail = 0; $failed = @()

foreach ($t in $tests) {
  $exe = Join-Path $out ($t.BaseName + '.exe')
  $log = & $gcc @flags -o $exe $t.FullName 2>&1

  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
    Write-Host "  BUILD FAIL  $($t.Name)  (gcc exit $LASTEXITCODE)"
    # Only the lines that name a problem. The PR/os.h bcopy/bcmp/bzero warnings fire on every
    # translation unit in this tree and would bury the real error.
    $diag = @($log | Where-Object { $_ -match 'error|undefined reference' })
    if ($diag.Count -gt 0) {
      $diag | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }
    } elseif (@($log).Count -gt 0) {
      $log | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }
    } else {
      # SAY SO when there is nothing to say. A failure that prints nothing is the hardest kind to
      # read, and the temptation is to blame the environment; the usual cause is that gcc could
      # not spawn cc1, which the PATH line above prevents.
      Write-Host '    (gcc produced NO output -- it usually means cc1 could not be spawned)'
    }
    $fail++; $failed += $t.Name
    continue
  }

  $out = & $exe 2>&1
  $rc = $LASTEXITCODE
  $out | ForEach-Object { Write-Host $_ }
  $n = @($out | Where-Object { $_ -match '^\s*(ok|OK|PASS|FAIL)' }).Count

  # A test that exits 0 having reported nothing is not a passing test, it is a test that did not
  # run. Three of these printed only on failure, so they scored zero checks and read as green
  # beside twelve doing real work; emptying one of their main() bodies would have looked the
  # same. The exit code cannot tell those apart, so the count is held to as well. Same rule as
  # run_tests.sh, deliberately, since these are the same tests.
  if ($rc -eq 0 -and $n -eq 0) {
    Write-Host "  SILENT  $($t.Name) -- exited 0 but reported no checks: does main() still assert anything?"
    $fail++; $failed += $t.Name
    continue
  }
  if ($rc -eq 0) { $pass++ } else { $fail++; $failed += $t.Name }
}

Write-Host ''
Write-Host "port tests: $pass passed, $fail failed"
foreach ($f in $failed) { Write-Host "  FAILED: $f" }
exit ($(if ($fail -gt 0) { 1 } else { 0 }))
