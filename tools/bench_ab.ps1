<#
.SYNOPSIS
  Interleaved A/B benchmark of two binaries.

.DESCRIPTION
  WHY INTERLEAVED, AND WHY THE OBVIOUS METHOD IS WRONG. Benchmarking A five times, then B five
  times, is confounded on this machine and the confound is SYSTEMATIC rather than random. Frame
  rate decays monotonically within every run of five as the Surface heats: 105 -> 85 fps on one,
  111 -> 91 on the next. Whichever build is measured SECOND is penalised by a thermal state the
  first one created, and no amount of repetition fixes it because it is a bias, not noise.

  So the runs alternate A B A B A B. Both builds see the same thermal ramp, the bias applies
  equally, and the paired differences can be compared. That is the only way a sub-20% effect is
  visible on hardware whose spread is 20%.

  Reported as a PAIRED comparison: each A is differenced against the B beside it, and the median
  of those differences is the answer. A median difference smaller than the spread of the
  differences means "no measurable effect" -- which is a real and useful result, not a failure.

  ⚠️ It reports CPU time per frame as the primary figure. Wall time includes everything the
  laptop is doing; CPU time is the program. When a change is meant to affect COMPUTATION, CPU
  time is the number that should move, and if it does not then the change did not do what it
  claimed regardless of what the wall clock says.
#>
param(
    [Parameter(Mandatory)] [string] $ExeA,
    [Parameter(Mandatory)] [string] $ExeB,
    [string] $LabelA = "A",
    [string] $LabelB = "B",
    [int]    $Pairs  = 5,
    [int]    $Frames = 900,
    [int]    $Stage  = 25
)

$ErrorActionPreference = "Stop"
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
$env:GETV_STAGE = "$Stage"; $env:GETV_EXIT_FRAME = "$Frames"
$env:GETV_NO_AUDIO = "1";   $env:GETV_WINDOW = "1"
$env:GETV_FPS = "0";        $env:GETV_VSYNC = "0"
$env:GETV_MODS = "";        $env:GETV_PROF = ""; $env:GETV_LOADTRACE = ""

foreach ($e in @($ExeA, $ExeB)) { if (-not (Test-Path $e)) { throw "missing binary: $e" } }

function RunOne([string] $exe) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -NoNewWindow -Wait -PassThru `
                       -RedirectStandardOutput "NUL" -RedirectStandardError "C:\ge\bench_ab.err"
    $sw.Stop()
    if ($p.ExitCode -ne 0) { throw "$exe exited $($p.ExitCode)" }
    return @{ wall = $sw.Elapsed.TotalSeconds; cpu = $p.TotalProcessorTime.TotalSeconds }
}

Write-Host ("interleaved A/B: {0} pairs x {1} frames" -f $Pairs, $Frames)
Write-Host ("  A = {0}  ({1})" -f $LabelA, $ExeA)
Write-Host ("  B = {0}  ({1})" -f $LabelB, $ExeB)
Write-Host ""

# One discarded pair first, for cold file cache. Both builds pay it, so discarding it costs
# nothing and keeping it would drag both medians toward a number neither reproduces.
RunOne $ExeA | Out-Null
RunOne $ExeB | Out-Null
Write-Host "  (warm-up pair discarded)"

$dCpu = @(); $dWall = @(); $aCpu = @(); $bCpu = @()
for ($i = 1; $i -le $Pairs; $i++) {
    # Alternate which build goes first within the pair, so that even the small heating that
    # happens BETWEEN the two members of a pair does not always fall on the same one.
    if ($i % 2) { $a = RunOne $ExeA; $b = RunOne $ExeB }
    else        { $b = RunOne $ExeB; $a = RunOne $ExeA }

    $ca = 1000.0 * $a.cpu / $Frames
    $cb = 1000.0 * $b.cpu / $Frames
    $aCpu += $ca; $bCpu += $cb
    $dCpu += ($cb - $ca)
    $dWall += ($b.wall - $a.wall)
    # The sign is written in rather than requested from the formatter: PowerShell's alignment
    # field takes only a digit, so "{5,+6:N2}" is a parse error at runtime -- and it fires AFTER
    # the warm-up pair has already run, wasting the whole setup.
    $delta = $cb - $ca
    $sign = if ($delta -ge 0) { "+" } else { "-" }
    Write-Host ("  pair {0,-2}  A {1,6:N2} ms/f  {2,5:N1}s   |   B {3,6:N2} ms/f  {4,5:N1}s   |  dB-A {5}{6:N2} ms/f" `
                -f $i, $ca, $a.wall, $cb, $b.wall, $sign, [Math]::Abs($delta))
}

function Median([double[]] $v) {
    $s = $v | Sort-Object; $n = $s.Count
    if ($n % 2) { return $s[[int](($n - 1) / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2.0
}

$mA = Median $aCpu; $mB = Median $bCpu
$mD = Median $dCpu
$lo = ($dCpu | Measure-Object -Minimum).Minimum
$hi = ($dCpu | Measure-Object -Maximum).Maximum

Write-Host ""
Write-Host ("  {0,-22} {1,6:N2} ms cpu/frame" -f $LabelA, $mA)
Write-Host ("  {0,-22} {1,6:N2} ms cpu/frame" -f $LabelB, $mB)
Write-Host ("  paired difference     {0,6:N2} ms/frame  (range {1:N2} .. {2:N2})" -f $mD, $lo, $hi)

# The verdict, stated so it cannot be misread as a win. A difference whose sign is not consistent
# across pairs has not been demonstrated, however attractive the median looks.
$sameSign = ($dCpu | Where-Object { $_ -gt 0 }).Count
if ($sameSign -eq $Pairs -or $sameSign -eq 0) {
    $pct = 100.0 * $mD / $mA
    Write-Host ("  VERDICT: consistent across all {0} pairs -- {1} is {2:N1}% {3}" -f `
                $Pairs, $LabelB, [Math]::Abs($pct), $(if ($mD -lt 0) { "FASTER" } else { "SLOWER" }))
} else {
    Write-Host ("  VERDICT: sign flips ({0} of {1} pairs positive) -- NO MEASURABLE DIFFERENCE." -f $sameSign, $Pairs)
    Write-Host  "           Do not report this as a speedup in either direction."
}
