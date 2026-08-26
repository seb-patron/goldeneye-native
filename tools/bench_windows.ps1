<#
.SYNOPSIS
  Repeatable frame-rate benchmark for the Windows build.

.DESCRIPTION
  WHY THIS EXISTS. This machine's run-to-run spread is enormous: two identical 900-frame runs of
  the same binary measured 63 and 95 fps, a 50% difference from nothing but background load and
  thermals. Every performance claim made from a single run on this box is worthless, and several
  nearly were -- an A/B of GETV_LOADTRACE came out 44.96 fps with the gate ON and 53.61 with it
  OFF, noise pointing the wrong way, which would have been committed as a speedup by anyone
  reading one number.

  So: N runs, discard the first, report the MEDIAN and the SPREAD. A change is only real if the
  medians separate by more than the spread of either.

  It reports CPU TIME as the primary figure and wall time second, deliberately. Wall time on a
  laptop measures the laptop -- thermal throttling, Defender, whatever else is running. CPU time
  measures the program. For compute work they move together and CPU time is far quieter; when they
  DISAGREE that is itself the finding, because it means the process is blocked rather than
  computing (which is exactly how the stdout-flush stall was found).

  stdout goes to NUL by default. Redirecting it to a file costs ~24 ms per flushed line on this
  box and swamps everything else being measured. Use -KeepLog when the output is the point.

.EXAMPLE
  tools\bench_windows.ps1 -Runs 5
  tools\bench_windows.ps1 -Runs 5 -Label "O2+LTO"
#>
param(
    [int]    $Runs   = 5,
    [int]    $Frames = 900,
    [int]    $Stage  = 25,
    [string] $Label  = "",
    [switch] $KeepLog,
    [string] $Exe    = "C:\ge\getv\build-windows\goldeneye.exe"
)

$ErrorActionPreference = "Stop"

# MinGW's runtime DLLs. Without these the process dies with STATUS_DLL_NOT_FOUND (-1073741515)
# and -- because it is a GUI-subsystem binary -- produces no output and sets no exit code, so a
# run that never happened looks exactly like one that printed nothing. Cost an hour once.
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"

$env:GETV_STAGE      = "$Stage"
$env:GETV_EXIT_FRAME = "$Frames"
$env:GETV_NO_AUDIO   = "1"
$env:GETV_WINDOW     = "1"
$env:GETV_FPS        = "0"      # no frame cap: we are measuring the engine, not the limiter
$env:GETV_VSYNC      = "0"      # no swap blocking, same reason
$env:GETV_MODS       = ""
$env:GETV_PROF       = ""
$env:GETV_LOADTRACE  = ""

if (-not (Test-Path $Exe)) { throw "no binary at $Exe" }
$stamp = (Get-Item $Exe).LastWriteTime

$wall = @(); $cpu = @()
Write-Host ("benchmark: {0} runs x {1} frames, stage {2}{3}" -f $Runs, $Frames, $Stage,
            $(if ($Label) { "   [$Label]" } else { "" }))
Write-Host ("binary:    {0:N1} MB, built {1}" -f ((Get-Item $Exe).Length / 1MB), $stamp)

for ($i = 0; $i -lt ($Runs + 1); $i++) {
    $out = if ($KeepLog) { "C:\ge\bench_run$i.out" } else { "NUL" }
    $err = "C:\ge\bench_run.err"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Exe -NoNewWindow -Wait -PassThru `
                       -RedirectStandardOutput $out -RedirectStandardError $err
    $sw.Stop()
    if ($p.ExitCode -ne 0) { throw "run $i exited $($p.ExitCode)" }

    # THE FIRST RUN IS DISCARDED, not averaged in. It pays for cold file cache on the assets and
    # for shader/pipeline warm-up, and including it drags the mean toward a number no later run
    # will ever reproduce. Reported anyway, because a first run that is wildly slower is a real
    # fact about how this thing starts.
    $w = $sw.Elapsed.TotalSeconds
    $c = $p.TotalProcessorTime.TotalSeconds
    if ($i -eq 0) {
        Write-Host ("  warmup   {0,6:N1}s wall  {1,5:N1}s cpu  ->{2,7:N2} fps  (discarded)" -f $w, $c, ($Frames / $w))
    } else {
        $wall += $w; $cpu += $c
        Write-Host ("  run {0,-2}   {1,6:N1}s wall  {2,5:N1}s cpu  ->{3,7:N2} fps" -f $i, $w, $c, ($Frames / $w))
    }
}

function Median([double[]] $v) {
    $s = $v | Sort-Object
    $n = $s.Count
    if ($n % 2) { return $s[[int](($n - 1) / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2.0
}

$mw = Median $wall; $mc = Median $cpu
$sw_lo = ($wall | Measure-Object -Minimum).Minimum
$sw_hi = ($wall | Measure-Object -Maximum).Maximum
$spread = 100.0 * ($sw_hi - $sw_lo) / $mw

Write-Host ""
Write-Host ("  MEDIAN   {0,6:N1}s wall  {1,5:N1}s cpu  ->{2,7:N2} fps" -f $mw, $mc, ($Frames / $mw))
Write-Host ("  spread   {0,6:N1}s .. {1:N1}s wall  ({2:N0}% of median)" -f $sw_lo, $sw_hi, $spread)
Write-Host ("  cpu/frame{0,6:N2} ms   <- the quiet number; compare THIS across builds" -f (1000.0 * $mc / $Frames))
if ($spread -gt 15.0) {
    Write-Host ("  NOTE: spread is {0:N0}%. A change smaller than that is not measurable here." -f $spread)
}
