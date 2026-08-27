<#
.SYNOPSIS
  S5: does the simulation reproduce itself over thousands of ticks?

.DESCRIPTION
  Lockstep has exactly one correctness property: identical inputs must produce identical
  simulations. ge_net.c already CATCHES a divergence between two peers -- but only after it has
  happened, only during a live session, and it reports THAT two machines disagree rather than how
  long they agreed or where they parted.

  THE LONG-RUN TEST NEEDS NO SECOND MACHINE. Two peers fed identical inputs are, for the
  determinism question, the same thing as ONE binary run twice. Delivering identical inputs is the
  network's job and tools/netsim.py already models that half; whether the simulation is
  reproducible GIVEN them is a separate property, and nothing tested it.

  NECESSARY, NOT SUFFICIENT. A pass means the simulation reproduces itself from the same inputs.
  It says nothing about whether the transport delivers them, and it does not exercise two hosts, so
  it cannot see a divergence that only a different CPU or compiler would produce. A FAILURE is
  decisive though: a machine that cannot reproduce itself will never agree with another.

  AND THE PASS IS GUARDED AGAINST BEING VACUOUS. If the fingerprint never changed, two runs
  would match trivially and the test would report success while checking nothing. The distinct-value
  count is therefore reported and a run whose fingerprint barely moves is called out, because "the
  sequences agreed" and "there was a sequence" are different claims.

.EXAMPLE
  tools\audit_lockstep.ps1 -Frames 3000
#>
param(
    [int]    $Frames = 3000,
    [int]    $Stage  = 25,
    [string] $Exe    = "C:\ge\getv\build-windows\goldeneye.exe"
)

$ErrorActionPreference = "Stop"
# MinGW runtime. Without it the binary dies with STATUS_DLL_NOT_FOUND, produces no output and sets
# no exit code -- a run that never happened looks exactly like one that printed nothing.
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"

$env:GETV_STAGE = "$Stage"; $env:GETV_EXIT_FRAME = "$Frames"
$env:GETV_NO_AUDIO = "1";   $env:GETV_WINDOW = "1"
$env:GETV_FPS = "0";        $env:GETV_VSYNC = "0"
$env:GETV_MODS = "";        $env:GETV_PROF = ""; $env:GETV_LOADTRACE = ""
$env:GETV_GPUTIME = "";     $env:GETV_GLDEBUG = ""; $env:GETV_NODRAW = ""
$env:GETV_FPTRACE = "1"

if (-not (Test-Path $Exe)) { throw "no binary at $Exe" }
Write-Host ("lockstep determinism: 2 runs x {0} frames, stage {1}" -f $Frames, $Stage)

$seqs = @{}
foreach ($n in @("A", "B")) {
    $out = "$env:TEMP\lockstep_$n.out"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Exe -NoNewWindow -Wait -PassThru `
                       -RedirectStandardOutput $out -RedirectStandardError "$env:TEMP\lockstep.err"
    $sw.Stop()
    if ($p.ExitCode -ne 0) { throw "run $n exited $($p.ExitCode)" }
    $s = @(Select-String -Path $out -Pattern '^\[getv\]\[fp\] (\d+) ([0-9a-f]{8})' |
           ForEach-Object { $_.Matches[0].Groups[2].Value })
    $seqs[$n] = $s
    Write-Host ("  run {0}: {1,6:N1}s  {2} samples" -f $n, $sw.Elapsed.TotalSeconds, $s.Count)
}

$a = $seqs["A"]; $b = $seqs["B"]
if ($a.Count -eq 0) {
    Write-Host "  NO SAMPLES -- is GETV_FPTRACE wired in? A silent trace is not a pass." -ForegroundColor Red
    exit 1
}

# The anti-vacuity guard, checked BEFORE the comparison so a stuck fingerprint cannot be reported
# as agreement.
$distinct = ($a | Sort-Object -Unique).Count
$ratio = [double]$distinct / $a.Count
Write-Host ("  distinct fingerprints: {0} of {1} ({2:P0})" -f $distinct, $a.Count, $ratio)
if ($ratio -lt 0.5) {
    Write-Host "  THE FINGERPRINT BARELY MOVES. Two runs matching proves little here -- the" -ForegroundColor Yellow
    Write-Host "     signal is nearly constant, so agreement is close to automatic." -ForegroundColor Yellow
}

$n = [Math]::Min($a.Count, $b.Count)
$first = -1
for ($i = 0; $i -lt $n; $i++) { if ($a[$i] -ne $b[$i]) { $first = $i; break } }

if ($a.Count -ne $b.Count) {
    Write-Host ("  sample COUNTS differ ({0} vs {1}) -- the runs did not even simulate the same " -f $a.Count, $b.Count) -ForegroundColor Yellow
    Write-Host "     number of frames, which is a divergence in itself." -ForegroundColor Yellow
}

if ($first -lt 0) {
    Write-Host ("`n  IDENTICAL across all {0} frames." -f $n) -ForegroundColor Green
    exit 0
}
# Locating the FIRST divergence is the whole value over ge_net.c's desync report: it names the
# frame to go and look at, rather than telling you the machines disagree by now.
Write-Host ("`n  DIVERGES at frame index {0}: A={1} B={2}" -f $first, $a[$first], $b[$first]) -ForegroundColor Red
for ($j = [Math]::Max(0, $first - 2); $j -lt [Math]::Min($n, $first + 3); $j++) {
    $mark = if ($j -eq $first) { "  <-- first difference" } else { "" }
    Write-Host ("     idx {0,-6} A={1}  B={2}{3}" -f $j, $a[$j], $b[$j], $mark)
}
exit 1
