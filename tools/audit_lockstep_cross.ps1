<#
.SYNOPSIS
  S5, the gap it named itself: does the simulation agree across MACHINES, not just with itself.

.DESCRIPTION
  audit_lockstep.ps1 runs two passes on ONE binary and proved the Windows build reproduces
  itself: 3000 of 3000 frames identical, same result the Mac build already reported. That
  question is now answered twice and closed.

  Its own writeup says what it cannot see: "it does not exercise two hosts, so it cannot see
  a divergence that only a different CPU or compiler would produce." Two same-machine runs
  cannot find that by construction -- they are the same compiler, the same float unit, the
  same everything except wall-clock timing. Only two DIFFERENT builds, fed the same inputs,
  can.

  This runs ONE pass here and reduces the fingerprint sequence to a single SHA-256 digest,
  the same comparison shape the project already trusts for the commits-and-vendor 1:1 check
  in sync_surface.sh: a name (frame count) proves the run happened, a hash proves every frame
  in it agreed. Post the digest, not 3000 lines of hex.

  A matching digest from an independent build (different compiler, different OS, different
  FPU codegen) is real cross-platform evidence. A mismatch is decisive either way: it names
  the exact frame to compare by hand, which two summary numbers never could.

.EXAMPLE
  tools\audit_lockstep_cross.ps1 -Frames 3000 -Stage 25
  -- then hand the printed digest, frame count and args to whoever runs the other build.
#>
param(
    [int]    $Frames = 3000,
    [int]    $Stage  = 25,
    [string] $Exe    = "C:\ge\getv\build-windows\goldeneye.exe"
)

$ErrorActionPreference = "Stop"
# MinGW runtime. Without it the binary dies with STATUS_DLL_NOT_FOUND, produces no output and
# sets no exit code -- a run that never happened looks exactly like one that printed nothing.
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"

$env:GETV_STAGE = "$Stage"; $env:GETV_EXIT_FRAME = "$Frames"
$env:GETV_NO_AUDIO = "1";   $env:GETV_WINDOW = "1"
$env:GETV_FPS = "0";        $env:GETV_VSYNC = "0"
$env:GETV_MODS = "";        $env:GETV_PROF = ""; $env:GETV_LOADTRACE = ""
$env:GETV_GPUTIME = "";     $env:GETV_GLDEBUG = ""; $env:GETV_NODRAW = ""
$env:GETV_FPTRACE = "1"

if (-not (Test-Path $Exe)) { throw "no binary at $Exe" }
Write-Host ("cross-build lockstep digest: {0} frames, stage {1}" -f $Frames, $Stage)

$out = "$env:TEMP\lockstep_cross.out"
$p = Start-Process -FilePath $Exe -NoNewWindow -Wait -PassThru `
                   -RedirectStandardOutput $out -RedirectStandardError "$env:TEMP\lockstep_cross.err"
if ($p.ExitCode -ne 0) { throw "run exited $($p.ExitCode)" }

$seq = @(Select-String -Path $out -Pattern '^\[getv\]\[fp\] (\d+) ([0-9a-f]{8})' |
         ForEach-Object { $_.Matches[0].Groups[2].Value })

if ($seq.Count -eq 0) {
    Write-Host "  NO SAMPLES -- is GETV_FPTRACE wired in? A silent trace is not a pass." -ForegroundColor Red
    exit 1
}

# Same anti-vacuity guard as audit_lockstep.ps1: a digest of a constant sequence would match
# trivially across any two builds and prove nothing.
$distinct = ($seq | Sort-Object -Unique).Count
$ratio = [double]$distinct / $seq.Count
Write-Host ("  samples: {0}   distinct: {1} ({2:P0})" -f $seq.Count, $distinct, $ratio)
if ($ratio -lt 0.5) {
    Write-Host "  THE FINGERPRINT BARELY MOVES. A matching digest would prove little here." -ForegroundColor Yellow
}

# Order-dependent by construction: one frame's fingerprint landing in a different position, or
# reading differently, changes the digest. That is the property being tested for.
$joined = [string]::Join("", $seq)
$sha = [System.Security.Cryptography.SHA256]::Create()
$bytes = $sha.ComputeHash([System.Text.Encoding]::ASCII.GetBytes($joined))
$digest = -join ($bytes | ForEach-Object { $_.ToString("x2") })

Write-Host ("`n  digest: {0}" -f $digest) -ForegroundColor Cyan
Write-Host ("  repro:  tools\audit_lockstep_cross.ps1 -Frames {0} -Stage {1}" -f $Frames, $Stage)
