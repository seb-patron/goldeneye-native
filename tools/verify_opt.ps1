<#
.SYNOPSIS
  Does changing the optimisation level change what the game DOES?

.DESCRIPTION
  A faster build that renders different pixels is not an archival build, and on a decompilation
  that risk is real rather than theoretical: this source reads the same memory through
  incompatible types, relies on layouts the original compiler happened to produce, and returns
  values from functions that fall off the end. -O1 tolerates a lot of that. -O2 and -O3 are
  entitled to assume none of it happens.

  So the level is never raised on the strength of a frame-rate number alone. This builds twice,
  runs both against an identical scripted workload, and compares the diagnostic output line for
  line. The port's synthetic clock makes gameplay frames deterministic on purpose (osGetCount in
  port_os.c: "gameplay frames stay byte-reproducible"), so two correct builds MUST agree.

  WHAT IS FILTERED, AND WHY THAT LIST IS SHORT ON PURPOSE. A few lines legitimately differ
  between any two runs of the SAME binary, so comparing them would report a difference every time
  and the check would be ignored within a day:

      %p pointers        heap addresses vary per process
      "@<n>ms" timings   wall-clock triggered, so a faster run polls fewer times
      fps / elapsed      the thing being changed

  Everything else must match exactly. It is tempting to widen this list until the check passes --
  that is how a test becomes a rubber stamp, and this file would rather fail loudly. If a new
  category shows up, look at whether it is genuinely non-deterministic before adding it.

.EXAMPLE
  tools\verify_opt.ps1 -Baseline -O1 -Candidate -O2
#>
param(
    [string] $Baseline  = '-O1',
    [string] $Candidate = '-O2',
    [switch] $CandidateLto,
    [int]    $Frames    = 900,
    [int]    $Stage     = 25
)

$ErrorActionPreference = "Stop"
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
$ge = "C:\ge\getv"

function Build([string] $opt, [switch] $lto, [string] $tag) {
    Write-Host "  building $tag ($opt$(if ($lto) { ' +lto' }))..." -NoNewline
    $log = "$env:TEMP\vopt_build_$tag.log"
    foreach ($t in @('lib', 'port', 'app')) {
        if ($lto) { & "$ge\build_windows.ps1" -Target $t -Opt $opt -Lto *> $log }
        else      { & "$ge\build_windows.ps1" -Target $t -Opt $opt      *> $log }
        if ($LASTEXITCODE -ne 0) { Write-Host " FAILED"; Get-Content $log | Select-Object -Last 15; throw "build $tag/$t failed" }
    }
    Write-Host " ok"
}

function Run([string] $tag) {
    $env:GETV_STAGE = "$Stage"; $env:GETV_EXIT_FRAME = "$Frames"
    $env:GETV_NO_AUDIO = "1";   $env:GETV_WINDOW = "1"
    $env:GETV_FPS = "0";        $env:GETV_VSYNC = "0"
    $env:GETV_MODS = "";        $env:GETV_PROF = ""
    # Load trace ON: it is the densest per-asset description of what the build actually decoded,
    # which is exactly the evidence an optimiser bug would corrupt. Verifying with it off would
    # compare almost nothing.
    $env:GETV_LOADTRACE = "1"
    $out = "$env:TEMP\vopt_run_$tag.out"
    $p = Start-Process -FilePath "$ge\build-windows\goldeneye.exe" -NoNewWindow -Wait -PassThru `
                       -RedirectStandardOutput $out -RedirectStandardError "$env:TEMP\vopt_run_$tag.err"
    if ($p.ExitCode -ne 0) { throw "run $tag exited $($p.ExitCode)" }
    return $out
}

function Normalise([string] $path) {
    Get-Content $path |
        # Wall-clock timings, in both spellings the tree uses. The second was missed on the first
        # run and produced 24 false differences -- "(159ms)" is a per-frame duration and cannot
        # possibly match between two builds, or between two runs of one build.
        Where-Object { $_ -notmatch '@\d+ms' -and $_ -notmatch '\(\d+ms\)' -and
                       $_ -notmatch 'start=\d+ms' -and $_ -notmatch '\bfps\b' -and
                       $_ -notmatch 'elapsed' } |
        ForEach-Object {
            # addresses, and anything derived from them. Two different binaries lay code out
            # differently, so any diagnostic printing a function address differs by construction
            # and says nothing about behaviour. [getv][fnptr] prints one and ALSO prints its low
            # 32 bits reinterpreted as a float, so the pointer and the float must both go.
            #
            # Established by CONTROL rather than assumed: the same binary run twice emits these
            # lines byte-identically, and both builds emit exactly 9 of them. The value moves only
            # when the binary does. Had the COUNT differed, that would have been real and this
            # filter would be hiding it -- so the count is asserted separately below.
            # order matters, and getting IT wrong corrupted the comparison. The
            # "pointer printed as a float" pair is collapsed as ONE unit FIRST, before any
            # general hex rule runs.
            #
            # The first version did it the other way round and mangled its own input: the
            # baseline prints -26504816079202425976135373291520.000000, whose digit run is 32
            # characters, and decimal digits are valid hex digits -- so the \b[0-9a-fA-F]{12,}\b
            # rule swallowed the float's mantissa and emitted "-<ptr>.000000". The float rule then
            # could not match, and the check reported 18 differences that did not exist. A
            # normaliser that rewrites the evidence is worse than none.
            #
            # Matched as pointer-immediately-followed-by-parenthesised-float rather than by
            # blanket-normalising every float: floats elsewhere are real computed values and
            # hiding those would gut the check.
            $s = $_ -replace '0x[0-9a-fA-F]{6,}\(-?[\d.]+\)', '<ptr-as-float>'
            $s = $s -replace '0x[0-9a-fA-F]{6,}', '<ptr>'
            $s -replace '\b[0-9a-fA-F]{12,}\b', '<ptr>'
        }
}

# The filters above hide VALUES, never OCCURRENCES. A category that appears a different number of
# times between builds is a real behavioural change and must survive normalisation, so it is
# counted separately -- otherwise a filter written to silence noise would also silence the signal
# it was standing next to.
function CategoryCounts([string] $path) {
    $h = @{}
    foreach ($l in Get-Content $path) {
        # the same line-LEVEL exclusions AS Normalise. Counting raw lines put wall-clock
        # timings back in through the side door: "frame 0: DONE start=1ms" and "start=0ms"
        # collapse to DIFFERENT keys, so 20 phantom category mismatches appeared from timing
        # jitter alone. A guard that fires on noise gets switched off, which is how the real
        # signal it was protecting goes with it.
        if ($l -match '@\d+ms' -or $l -match '\(\d+ms\)' -or $l -match 'start=\d+ms' -or
            $l -match '\bfps\b' -or $l -match 'elapsed') { continue }
        $k = ($l -replace '[-\dxXa-fA-F]{2,}', 'N')
        if ($h.ContainsKey($k)) { $h[$k]++ } else { $h[$k] = 1 }
    }
    return $h
}

Write-Host "verify_opt: $Baseline  vs  $Candidate$(if ($CandidateLto) { ' +lto' })"
Build $Baseline -tag "base"
$a = Run "base"
Build $Candidate -lto:$CandidateLto -tag "cand"
$b = Run "cand"

$na = Normalise $a
$nb = Normalise $b
Write-Host ""
Write-Host ("  baseline  {0} lines" -f $na.Count)
Write-Host ("  candidate {0} lines" -f $nb.Count)

# Occurrence check first: it is the one the value filters cannot weaken.
$ca = CategoryCounts $a
$cb = CategoryCounts $b
$countDiff = @()
foreach ($k in ($ca.Keys + $cb.Keys | Sort-Object -Unique)) {
    $x = if ($ca.ContainsKey($k)) { $ca[$k] } else { 0 }
    $y = if ($cb.ContainsKey($k)) { $cb[$k] } else { 0 }
    if ($x -ne $y) { $countDiff += ("    {0,4} vs {1,-4} {2}" -f $x, $y, $k.Trim().Substring(0, [Math]::Min(84, $k.Trim().Length))) }
}
if ($countDiff.Count) {
    Write-Host "  CATEGORY COUNTS DIFFER -- a real behavioural change:" -ForegroundColor Red
    $countDiff | Select-Object -First 15 | ForEach-Object { $_ }
    exit 1
}
Write-Host ("  category counts match across {0} distinct line shapes" -f $ca.Count)

$diff = Compare-Object $na $nb
if (-not $diff) {
    Write-Host "  IDENTICAL -- the candidate computes the same thing." -ForegroundColor Green
    exit 0
}
Write-Host ("  {0} DIFFERING LINE(S) -- the candidate does NOT compute the same thing:" -f $diff.Count) -ForegroundColor Red
$diff | Select-Object -First 20 | ForEach-Object {
    $side = if ($_.SideIndicator -eq '<=') { 'baseline ' } else { 'candidate' }
    "    $side | " + $_.InputObject.Trim().Substring(0, [Math]::Min(96, $_.InputObject.Trim().Length))
}
exit 1
