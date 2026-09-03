<#
  One command, from a fresh clone to a binary you can run. Windows.

  The twin of tools/install.sh, which does the same job on macOS and Linux. Same steps, same
  order, same guarantees, and the two are meant to be read side by side.

  tools\fetch_deps_windows.ps1 already handled the toolchain and the optional libraries and
  stopped there, which was the honest place to stop when the rest was described as a few manual
  steps out of docs/SETUP.md. It is not a few steps. It is fourteen asset commands in a fixed
  order, six namespacing invocations with three different flag combinations, a patch that must
  go on afterwards and not before, and a build. Every one of them has a way to go wrong quietly.

      powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Rom C:\roms\ge.n64
      powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -NoBuild -Yes

  THREE THINGS IT WILL NOT DO

    It will not fetch a ROM. Not from a URL, not from an argument that looks like one, not
    ever. You supply your own copy of a game you own.

    It will not install anything system-wide on its own beyond what fetch_deps_windows.ps1
    already does, and that script unpacks into C:\mingw64 and %USERPROFILE%\.n64tvos rather
    than touching the registry or PATH.

    It will not redo a step that is already done. Re-running is safe and is the intended way to
    resume after fixing whatever stopped it. That matters more here than usual: the namespacing
    pass in step 7 corrupts the tree if it runs twice over an already-namespaced one
    (docs/SETUP.md 3.6), so rather than trusting a marker this script wrote, it reads the tree
    and asks whether the symbols are already prefixed.
#>
[CmdletBinding()]
param(
  [string]$Rom    = '',
  [string]$Mingw  = 'C:\mingw64',
  [switch]$NoBuild,
  [switch]$Yes,
  [switch]$SkipDeps
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# Derived from git.exe's own location rather than found by bare name. System32 ships a bash.exe
# of its own -- the WSL launcher stub -- and it comes before Git's bin directory in the default
# PATH order, so a bare `& bash` runs WSL instead of git-bash and prints a UTF-16 "no installed
# distributions" line that reads as nothing having happened. Script-scoped because every bash
# call site below needs the same answer, and the two asset steps that passed the bare name
# instead are exactly how this got through the first time.
$Bash = Join-Path (Split-Path -Parent (Split-Path -Parent (Get-Command git).Source)) 'bin\bash.exe'
if (-not (Test-Path $Bash)) { $Bash = 'bash' }

# Python on Windows encodes stdout as cp1252 once it is redirected rather than attached to a
# console, and several of the decomp's generators print non-ASCII status glyphs. generate_chr_c.py
# raises UnicodeEncodeError on U+2298 and takes the whole step down with it -- a crash in the
# reporting, not in the work. The decomp is re-cloned by this script, so its scripts cannot be
# patched here; setting the encoding for the children is what survives a fresh clone.
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONUTF8       = '1'

$script:step = 0
function Say  ($m) { $script:step++; Write-Output ""; Write-Output "== $($script:step). $m" }
function Info ($m) { Write-Output "   $m" }
function Die  ($m) { Write-Output ""; Write-Error "stopped: $m"; exit 1 }

# Defaults to no, so a non-interactive run never takes an action nobody asked for.
function Confirm-Step ($m) {
  if ($Yes) { return $true }
  if (-not [Environment]::UserInteractive) { return $false }
  $r = Read-Host "   $m [y/N]"
  return ($r -match '^[yY]')
}

# ---------------------------------------------------------------- 1. toolchain

Say "toolchain"
$missing = @()
foreach ($t in 'git','python') {
  if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { $missing += $t }
}
if ($missing.Count -gt 0) {
  Info "missing: $($missing -join ', ')"
  Info "git:    https://git-scm.com/download/win"
  Info "python: https://www.python.org/downloads/windows/  (tick 'Add python.exe to PATH')"
  Die "install the above and run this again"
}
Info "git and python are present"

if ($SkipDeps) {
  Info "-SkipDeps given; not running fetch_deps_windows.ps1"
} elseif (Test-Path "$Mingw\bin\gcc.exe") {
  Info "mingw-w64 already at $Mingw"
} else {
  # Delegated rather than duplicated. That script pins the compiler by URL and explains why,
  # and two places deciding which GCC this port builds with is how they drift apart.
  Info "running tools\fetch_deps_windows.ps1 for the toolchain and libraries"
  & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\fetch_deps_windows.ps1" -Mingw $Mingw
  if (-not (Test-Path "$Mingw\bin\gcc.exe")) { Die "the toolchain is still not at $Mingw" }
}

# ---------------------------------------------------------------- 2. port-layer sources

Say "third-party port-layer sources"
if (Test-Path "$root\getv\port\fast3d\gfx_pc.c") {
  Info "already present"
} else {
  # The fetch script is bash. Git for Windows ships one, which is the only reason this does not
  # need a PowerShell rewrite of it. $Bash is derived once at the top of this script.
  & $Bash -lc "cd '$($root -replace '\\','/')' && bash tools/fetch-thirdparty.sh fetch"
  if (-not (Test-Path "$root\getv\port\fast3d\gfx_pc.c")) { Die "the port-layer fetch did not produce gfx_pc.c" }
}

# ---------------------------------------------------------------- 3. the decompilation

Say "the decompilation, and every patch in getv\patches"
$decomp = Join-Path $root 'vendor\ge-decomp'
$fresh  = $false
if (Test-Path "$decomp\.git") {
  Info "already cloned"
} else {
  & git clone --depth 1 https://github.com/n64decomp/007 $decomp
  if (-not (Test-Path "$decomp\.git")) { Die "the clone failed" }
  $fresh = $true
  Info "cloned"
}

# Applied by globbing the directory in numeric order rather than from a hand-written list. The
# list is how 0003 and then 0007 came to be committed and never applied by either setup script:
# the tree builds, nothing complains, and the change is simply absent. A patch dropped into that
# directory is now applied by definition.
#
# 0002 is skipped here. It carries generated asset sources that do not exist until step 6 has
# run, and applying it before the namespacing pass double-prefixes the font symbols. It goes on
# in step 7, which is the only correct moment.
#
# A fresh clone and an existing tree need opposite handling. On a fresh clone every patch must
# apply and a failure is real. On an existing tree `git apply --reverse --check` is NOT a test of
# whether a patch is applied: the patches layer, so 0003 and 0010 both edit objective_status.c
# after 0001 has, and 0001 stops reverse-applying the moment 0003 goes on even though it is
# applied and correct. So the other question gets asked instead, whether the patch applies
# cleanly going forward, which finds one added since that tree was set up without disturbing one
# already on it.
foreach ($p in (Get-ChildItem "$root\getv\patches\0*.patch" | Sort-Object Name)) {
  if ($p.Name -like '0002-*') { continue }
  Push-Location $decomp
  try {
    if ($fresh) {
      & git apply $p.FullName
      if ($LASTEXITCODE -ne 0) { Die "$($p.Name) failed to apply to a fresh checkout; see getv\patches\README.md" }
      Info "$($p.Name): applied"
    } else {
      # This check is a question, not a failure, and both halves of asking it needed care.
      #
      # It used to discard the error stream with 2>$null, which left nothing reading the pipe.
      # An already-applied 0001 rejects every hunk and writes far more than a pipe buffer holds,
      # so git blocked writing its own stderr and the re-run sat at zero CPU indefinitely --
      # against the one workflow the header above promises is safe to resume with.
      #
      # Merging into a consumer that drains fixes the block, but then git's stderr reaches
      # PowerShell, and under ErrorActionPreference Stop a native command's first stderr line
      # is a terminating NativeCommandError. So the preference is relaxed for the length of the
      # question and restored straight after; $LASTEXITCODE is the answer being read here.
      $eap = $ErrorActionPreference
      $ErrorActionPreference = 'Continue'
      & git apply --check $p.FullName 2>&1 | Out-Null
      $checkRc = $LASTEXITCODE
      $ErrorActionPreference = $eap
      if ($checkRc -eq 0) {
        & git apply $p.FullName
        Info "$($p.Name): was missing from this tree, applied now"
      } else {
        Info "$($p.Name): already applied, or drifted; left alone"
      }
    }
  } finally { Pop-Location }
}

# ---------------------------------------------------------------- 4. your copy of the game

Say "your copy of the game"

$romDest   = Join-Path $root 'roms\ge007.u.z64'
$decompRom = Join-Path $decomp 'baserom.u.z64'
$romSha  = 'ABE01E4AEB033B6C0836819F549C791B26CFDE83'

function Get-Sha1 ($path) { (Get-FileHash -Algorithm SHA1 -LiteralPath $path).Hash.ToUpper() }

# A dump in the wrong byte order is the normal case, not the exception, and the extension does
# not tell you which one you have. The header does.
#
#   80371240  z64  big endian, what the build wants
#   37804012  v64  byte-swapped in pairs
#   40123780  n64  word-reversed
#
# Worth automating because the failure it prevents is not a clear one: the wrong order gets past
# a size check and past a "yes that is 12 MB" glance, then fails later as garbage data inside the
# asset extractor.
function Convert-Rom ($src, $dst) {
  $b = [IO.File]::ReadAllBytes($src)
  if ($b.Length -lt 4) { return $null }
  $magic = ('{0:x2}{1:x2}{2:x2}{3:x2}' -f $b[0],$b[1],$b[2],$b[3])
  switch ($magic) {
    '80371240' { }                                              # already z64
    '37804012' { for ($i = 0; $i -lt $b.Length - 1; $i += 2) {   # v64, pairwise
                   $t = $b[$i]; $b[$i] = $b[$i+1]; $b[$i+1] = $t } }
    '40123780' { for ($i = 0; $i -lt $b.Length - 3; $i += 4) {   # n64, word-reversed
                   $t = $b[$i];   $b[$i]   = $b[$i+3]; $b[$i+3] = $t
                   $t = $b[$i+1]; $b[$i+1] = $b[$i+2]; $b[$i+2] = $t } }
    default    { Info "unrecognised ROM header $magic"; return $null }
  }
  [IO.File]::WriteAllBytes($dst, $b)
  return $magic
}

if ((Test-Path $romDest) -and (Get-Sha1 $romDest) -eq $romSha) {
  Info "$romDest verified"
} else {
  $cand = $Rom
  if (-not $cand) {
    # Only somewhere the person running this would obviously have put it. No walking the disk
    # looking for game data.
    foreach ($d in @("$root\roms", "$env:USERPROFILE\Desktop", "$env:USERPROFILE\Downloads")) {
      if (-not (Test-Path $d)) { continue }
      # The trailing \* is required: -Include is ignored unless the path itself ends in a
      # wildcard or -Recurse is passed, so against a bare directory this matched nothing at all
      # and every Windows run reported "no ROM" with the ROM sitting on the Desktop.
      $hit = Get-ChildItem "$d\*" -File -Include *.z64,*.n64,*.v64 -ErrorAction SilentlyContinue |
             Where-Object { $_.Length -eq 12582912 } | Select-Object -First 1
      if ($hit) { $cand = $hit.FullName; break }
    }
  }
  if (-not $cand) {
    Write-Output @'
   No ROM found, and nothing here will download one.

   Supply your own copy of GoldenEye 007 (USA), 12,582,912 bytes. Any byte order
   works; this converts it. Put it at roms\ge007.u.z64 or pass -Rom <path>, then
   run this again. docs/SETUP.md section 3 covers what a correct dump looks like.
'@
    Die "no ROM"
  }

  Info "candidate: $cand"
  if ($cand -ne $romDest -and -not (Confirm-Step "convert and copy this into roms\ ?")) {
    Die "declined. Pass -Rom <path> or put the file at $romDest yourself."
  }

  New-Item -ItemType Directory -Force -Path (Join-Path $root 'roms') | Out-Null
  $tmp = [IO.Path]::GetTempFileName()
  $magic = Convert-Rom $cand $tmp
  if (-not $magic) { Remove-Item $tmp -Force; Die "that file is not a recognisable N64 ROM" }
  $got = Get-Sha1 $tmp
  if ($got -ne $romSha) {
    Remove-Item $tmp -Force
    Info "sha1 after conversion: $got"
    Info "expected:              $romSha"
    Die "that is not the US retail dump this port builds from. docs/SETUP.md 3.4 covers what to do."
  }
  Move-Item -Force $tmp $romDest
  Info "$romDest written and verified ($(if ($magic -eq '80371240') { 'already z64' } else { "converted from $magic" }))"
}

# ---------------------------------------------------------------- 5. the asset pipeline

# The extractor reads baserom.u.z64 from inside the decomp, not roms\ge007.u.z64, and defaults
# that name with no way to pass another. A copy rather than a symlink: New-Item -ItemType
# SymbolicLink needs Developer Mode or an elevated shell on Windows, and a 12 MB copy is a
# smaller price than an installer that fails for anyone who has neither.
if (-not (Test-Path $decompRom)) {
  Copy-Item -LiteralPath $romDest -Destination $decompRom -Force
  Info "copied roms\ge007.u.z64 to vendor\ge-decomp\baserom.u.z64 for the extractor"
}

Say "generating the asset sources"

# Order is not stylistic. It was established by building from a clean checkout and every entry
# has something downstream that fails without it, usually with an error naming a different file.
# docs/SETUP.md 3.5 is the long form. Each step is skipped if its own output is already there,
# so a run that stops halfway is resumed by running this again.
#
# Markers are a specific file each generator writes, never the directory it writes into: an empty
# directory left by a run that died halfway would otherwise read as "done".
# Whether a step's output is present AND complete. Split out because the retry loop below asks
# the same question after every attempt, and because the two marker shapes answer it differently.
#
# A per-directory marker (assets\obseg\chr\*\Model.c) needs COUNTING, not existence. One match
# used to be enough to read as done, and generate_chr_c.py once died on its first file: every
# later run reported "already done" off that single Model.c, and the build then linked 667 assets
# with 0 failures and 79 of the 80 character models missing. Nothing in the compile, the archive
# or the link says a word about an asset that was never handed to it, so the count against the
# directories the generator walks is the only thing that can tell.
function Get-AssetMarkerState ($marker) {
  $full = Join-Path $decomp $marker
  if ($marker -match '\\\*\\') {
    $base = Join-Path $decomp ($marker -replace '\\\*\\.*$', '')
    $dirs = @(Get-ChildItem -Path $base -Directory -ErrorAction SilentlyContinue)
    $got  = @(Get-ChildItem -Path $full -ErrorAction SilentlyContinue)
    return [pscustomobject]@{
      Done = ($dirs.Count -gt 0 -and $got.Count -ge $dirs.Count)
      Have = $got.Count; Want = $dirs.Count; Glob = $true
    }
  }
  return [pscustomobject]@{ Done = (Test-Path $full); Have = 0; Want = 0; Glob = $false }
}

# Reads a validator's answer as a BOOLEAN rather than trusting what the block returned.
#
# `& $scriptblock` hands back everything the block wrote to the OUTPUT stream, not just the value
# it returned, and Info in this file is Write-Output. So $combinedComplete below, on the one path
# that matters, emits its "incomplete" line and then returns $false -- and the caller receives
# @("   combined.bin is ... incomplete", $false). A two-element array. PowerShell treats any
# non-empty array as true, so the check PASSED precisely when it had just found the file
# truncated, which is the silent corruption it exists to catch.
#
# Taking the last element and coercing means a validator can only ever answer yes or no, however
# chatty it is. An empty result is a no: a validator that returned nothing did not say yes.
function Test-Validator ($validate) {
  if (-not $validate) { return $true }
  $r = @(& $validate)
  if ($r.Count -eq 0) { return $false }
  return [bool]$r[-1]
}

function Invoke-AssetStep ($marker, $label, $exe, $argv, $validate) {
  $st = Get-AssetMarkerState $marker
  if ($st.Done -and (Test-Validator $validate)) { Info "$label`: already done"; return }
  if ($st.Glob -and $st.Have -gt 0) {
    Info "$label`: incomplete, $($st.Have) of $($st.Want) present; generating the rest"
  }

  # Tried up to three times. These bash steps fork hard, and MSYS on this platform intermittently
  # loses a child outright -- "cygheap read copy failed / forked process died unexpectedly" -- which
  # leaves extract_baserom.u.sh exiting 0 with whole sections of its work simply not done: on one
  # run every one of the 34 background rows was missing while the script reported success. It is
  # transient and a repeat clears it, so the installer repeats it rather than handing that to the
  # person running it, who has no way to tell a dropped fork from a bad ROM. Every one of these
  # steps skips what already exists, so a repeat costs time and nothing else.
  $out = $null
  for ($attempt = 1; $attempt -le 3; $attempt++) {
    Info $label
    Push-Location $decomp
    try {
      # Captured rather than discarded, and the preference relaxed while it runs. Under
      # ErrorActionPreference Stop the first stderr line out of one of these generators is a
      # terminating NativeCommandError, so a run died on the line "Traceback (most recent call
      # last):" and threw the traceback itself away. A step that fails now prints what the tool
      # said before it stops.
      $eap = $ErrorActionPreference
      $ErrorActionPreference = 'Continue'
      $out = & $exe @argv 2>&1
      $rc  = $LASTEXITCODE
      $ErrorActionPreference = $eap
    } finally { Pop-Location }

    # An exit code of 0 is not the same claim as having produced the output, which is the whole
    # reason this is checked separately. Nor is the output EXISTING the same claim as it being
    # complete, which is what $validate is for.
    $st = Get-AssetMarkerState $marker
    $valid = Test-Validator $validate
    if ($rc -eq 0 -and $st.Done -and $valid) { return }

    $why = if ($rc -ne 0) { "exit $rc" }
           elseif ($st.Glob) { "produced $($st.Have) of $($st.Want)" }
           elseif ($st.Done -and -not $valid) { "produced an incomplete $marker" }
           else { "exited 0 but produced no $marker" }
    if ($attempt -lt 3) {
      Info "$label`: $why -- retrying, attempt $($attempt + 1) of 3"
    } else {
      $out | Select-Object -Last 40 | ForEach-Object { Write-Output "      $_" }
      Die "$label failed after 3 attempts ($why)"
    }
  }
}

# scripts/extract_baserom.u.sh builds the C extractor with `make -C tools/extractor` and that
# makefile calls gcc. Neither is on git-bash's PATH: git-bash has a /mingw64/bin of its own, which
# is Git's, not the toolchain this port installs at $Mingw, so bash sees no compiler and no make
# even on a machine that just finished running fetch_deps_windows.ps1. The failure is a bare
# "make: command not found" eleven lines into a bash script, which does not name the toolchain.
#
# mingw-w64 ships make under the name mingw32-make, so a bare `make` misses it even once $Mingw
# is on PATH. A copy under the wanted name is the whole shim; it is not worth asking the vendored
# decomp scripts to know what a Windows toolchain calls its make.
# The shim is a shell script that execs the real binary where it already lives, not a renamed
# copy of it. Only bash resolves `make` here, so a script is enough -- and copying the exe was
# worse than unnecessary: a copy outside the toolchain directory ran and exited 0 while printing
# nothing at all, which would have turned a missing extractor into a silent success.
$toolshim = Join-Path $root 'build\toolshim'
if (-not (Get-Command make -ErrorAction SilentlyContinue)) {
  $mingwMake = Join-Path $Mingw 'bin\mingw32-make.exe'
  if (-not (Test-Path $mingwMake)) { Die "no make: neither make on PATH nor $mingwMake" }
  New-Item -ItemType Directory -Force -Path $toolshim | Out-Null
  $shimBody = "#!/bin/sh`nexec '$($mingwMake -replace '\\','/')' `"`$@`"`n"
  [IO.File]::WriteAllText((Join-Path $toolshim 'make'), $shimBody)
  $env:PATH = "$toolshim;$env:PATH"
  Info "make: shimmed to $mingwMake"
}
if (Test-Path "$Mingw\bin\gcc.exe") { $env:PATH = "$Mingw\bin;$env:PATH" }

# Must run BEFORE extraction. The decomp ships 25 of the 34 bg rows with their extract flag at 0
# because upstream builds those from checked-in .c files; this port compiles the blobs, so
# without it the link ends with 25 undefined symbols and nothing earlier hints at why. Running it
# twice is harmless, so it is not marker-guarded.
Info "enabling background extraction"
Push-Location $decomp
try {
  $eap = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  $out = & python "$root\tools\enable_bg_extraction.py" 2>&1
  $rc  = $LASTEXITCODE
  $ErrorActionPreference = $eap
  # The exit code was not read here at all, so a failure was silent -- and this step is what
  # keeps 25 of the 34 bg rows from going missing, which surfaces much later as undefined
  # symbols at link with nothing pointing back to here.
  if ($rc -ne 0) { $out | Select-Object -Last 20 | ForEach-Object { Write-Output "      $_" }; Die "enabling background extraction failed" }
} finally { Pop-Location }

Invoke-AssetStep 'assets\obseg\bg\bg_ame_all_p.bin'      'extracting from the ROM' $Bash @('scripts/extract_baserom.u.sh')
Invoke-AssetStep 'assets\obseg\chr\*\Model.c'            'character models'        'python' @('scripts/generate_chr_c.py')
Invoke-AssetStep 'assets\obseg\gun\*\Model.c'            'weapon models'           'python' @('scripts/generate_gun_c.py')
Invoke-AssetStep 'assets\obseg\prop\*\Model.c'           'prop models'             'python' @('scripts/generate_prop_model_c.py')
Invoke-AssetStep 'assets\obseg\ge_obseg_blobs.c'         'obseg blobs'             'python' @("$root\tools\gen_obseg_blobs.py")
Invoke-AssetStep 'build\imagelist.csv'                   'image list'              'python' @('scripts/make/sync_imagelist_with_def.py','build/imagelist.csv')
# combine_images_named.sh appends each listed .bin with `cat file >> combined.bin` and never reads
# cat's exit status -- its only guard is whether the file EXISTS. Under the MSYS fork failures this
# machine produces ("forked process died unexpectedly"), a cat dies, its bytes are never appended,
# and the script finishes reporting success. It cost a boot: combined.bin came out 14,072 bytes
# short of its own inputs with zero files missing and no warning printed, texInflateZlib was then
# handed bytes that are not valid deflate, inflate ran the output pointer off to 2 GB, and the game
# died at 0xC0000005 in texture loading with nothing pointing back here.
#
# The size is the only witness, so it is checked: the concatenation cannot be smaller than the
# files that went into it. Padding to the next 16 bytes makes the real file slightly larger, hence
# -lt rather than -ne. The script is the decomp's own and is re-cloned every install, so this
# belongs here rather than in it.
$combinedComplete = {
  $csv = Join-Path $decomp 'build\imagelist.csv'
  $bin = Join-Path $decomp 'assets\images\combined\combined.bin'
  if (-not (Test-Path $csv) -or -not (Test-Path $bin)) { return $false }
  $want = 0
  foreach ($line in [IO.File]::ReadAllLines($csv)) {
    $p = $line.Split(',')
    if ($p.Count -lt 3) { continue }
    $f = Join-Path $decomp ($p[2].Trim() -replace '/', '\')
    if (Test-Path $f) { $want += (Get-Item $f).Length }
  }
  $have = (Get-Item $bin).Length
  if ($have -lt $want) {
    Info "combined.bin is $have bytes against $want of input -- incomplete"
    return $false
  }
  return $true
}
Invoke-AssetStep 'assets\images\combined\combined.bin'   'combining images'        $Bash @('scripts/make/combine_images_named.sh','build/imagelist.csv','assets/images/combined') $combinedComplete
# combined.bin becomes a C array rather than an object. Upstream turns it into one with
# `ld -r -b binary`, a GNU extension with no Mach-O equivalent, so the bytes are emitted as C.
Invoke-AssetStep 'assets\images\ge_images_segment.c'     'images segment'          'python' @("$root\tools\gen_images_segment.py")
Invoke-AssetStep 'assets\ge_animation_offsets.h'         'animation blobs'         'python' @("$root\tools\gen_anim_blobs.py")
# The marker is the .c and not the .h, and that distinction is the whole point:
# 0001-source.patch already carries src/ge_audio_segment.h, and patches are applied five
# steps before this one. Marking on the header means the header is always present by the
# time we get here, the generator never runs, and the 1.3 MB array that actually DEFINES
# geAudioSegment is never written -- which surfaces as an undefined reference from music.c
# at the final link, long after the step that was silently skipped.
Invoke-AssetStep 'assets\music\ge_audio_segment.c'        'audio segment'           'python' @("$root\tools\gen_audio_segment.py")
Invoke-AssetStep 'src\ge_asset_fileview.h'               'asset file views'        'python' @("$root\tools\gen_asset_fileview.py")

# No marker on either of these, for opposite reasons.
#
# fix_asset_switchnodes retypes `u32 SwitchNodes[]` to real ModelNode pointers, and its own regex
# requires the `u32` spelling, so a converted file no longer matches and a second run is a no-op.
# It is idempotent by construction rather than by a guard.
Info "switch nodes"
Push-Location $decomp
try {
  $eap = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  $out = & python "$root\tools\fix_asset_switchnodes.py" 2>&1
  $rc  = $LASTEXITCODE
  $ErrorActionPreference = $eap
  if ($rc -ne 0) { $out | Select-Object -Last 20 | ForEach-Object { Write-Output "      $_" }; Die "switch nodes failed" }
} finally { Pop-Location }

# gen_propdef_layout is a CHECK, not a generator, despite sitting in a list of generators. It
# compiles a throwaway translation unit, dumps the record layouts and asserts the N64 file layout
# still matches the native one. It writes nothing the build consumes, so it runs every time.
# It reads clang's -fdump-record-layouts, which gcc has no equivalent for, so it cannot run on
# the toolchain fetch_deps_windows.ps1 installs. Skipped with its reason rather than failed: the
# comment above is the argument for it -- nothing the build consumes depends on it, and stopping
# an otherwise sound install over a check that was never able to run here would be wrong. Its
# probe files are also written to a hardcoded /tmp, which is not a path on Windows.
if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
  Info "propdef layout check: skipped, it needs clang and this host has none"
} else {
  Info "propdef layout check"
  Push-Location $decomp
  try {
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = & python 'tools/gen_propdef_layout.py' 2>&1
    $rc  = $LASTEXITCODE
    $ErrorActionPreference = $eap
    if ($rc -ne 0) { $out | Write-Output; Die "propdef layout check failed. The output above is its report." }
  } finally { Pop-Location }
}

# ---------------------------------------------------------------- 6. namespacing

Say "namespacing the asset symbols"

# THE ONE STEP THAT MUST NOT RUN TWICE.
#
# Getools emits every asset with generic global names and emits the SAME names in every level's
# file, so linked together all 29 stan files and all 50 setup files define the same symbols and
# each binds to whichever object the linker saw first. Before this pass existed, the Dam's stan
# header resolved to another level's tile_0 and its 368-entry pad list resolved to a 62-entry one
# belonging somewhere else. Every level ran on some other level's data, and it was not a link
# error, so nothing said so.
#
# Running it again over an already-patched directory double-prefixes symbols while leaving their
# uses alone, which breaks the files it previously fixed. The guard asks the tree rather than
# trusting a marker this script wrote, because a tree set up by hand from docs/SETUP.md predates
# any marker and is fully namespaced: a fresh checkout declares `PadRecord padlist[]` in every
# setup file, and after the pass each carries its own file stem.
# Two tells, one per shape the pass produces. Setup files take their file stem
# (`padlist` -> `Ump_setupameZ_padlist`); chr, gun and prop models take their directory
# (`GFX_PRIMARY_0x48e8` -> `armourguard_Model_GFX_PRIMARY_0x48e8`). Testing only the first would
# pass a tree whose several hundred models were never touched, and the marker is consulted only
# when there is no tree to ask -- a marker records that the pass ran, never that it did anything.
$marker = Join-Path $decomp '.getv-assets-namespaced'
$asked  = $false
$bare   = $false
$setupC = Get-ChildItem "$decomp\assets\obseg\setup\*.c" -ErrorAction SilentlyContinue
if ($setupC.Count -gt 0) {
  $asked = $true
  foreach ($f in $setupC) {
    if (Select-String -Path $f.FullName -Pattern '(^|\s)padlist\[' -Quiet) { $bare = $true; break }
  }
}
if (-not $bare) {
  foreach ($d in @('chr','gun','prop')) {
    $models = Get-ChildItem "$decomp\assets\obseg\$d\*\Model.c" -ErrorAction SilentlyContinue
    if ($models.Count -eq 0) { continue }
    $asked = $true
    foreach ($f in $models) {
      if (Select-String -Path $f.FullName -Pattern '(^|\s)GFX_PRIMARY_' -Quiet) { $bare = $true; break }
    }
    if ($bare) { break }
  }
}
# Third tell, for stan. Two were enough while the pass either ran to completion or not at all,
# but it runs in six calls and stan is the last of them, so an interrupted run leaves setup and
# the models namespaced and all 29 stan files still bare -- and the first two tells then report
# a finished pass. Skipping stan is silent by construction: 29 definitions of tile_0 in a static
# archive is not a link error, it just binds 28 levels to another level's collision data.
if (-not $bare) {
  $stanC = Get-ChildItem "$decomp\assets\obseg\stan\*.c" -ErrorAction SilentlyContinue
  if ($stanC.Count -gt 0) {
    $asked = $true
    foreach ($f in $stanC) {
      # Anchored to a declaration, not just the name. Every namespaced stan file opens with a
      # comment block that discusses tile_0 in prose, so a looser tell reports all 29 as bare
      # and re-runs the pass over a tree that is already done -- double-prefixing symbols while
      # leaving their uses alone, which is a worse failure than the one being guarded against.
      if (Select-String -Path $f.FullName -Pattern '^StandTile\s+tile_0\b' -Quiet) { $bare = $true; break }
    }
  }
}
$namespacedNow = $false
if (($asked -and -not $bare) -or ((-not $asked) -and (Test-Path $marker))) {
  Info "already namespaced; not running again, which would corrupt it"
  New-Item -ItemType File -Force -Path $marker | Out-Null
} else {
  $namespacedNow = $true
  Push-Location $decomp
  try {
    $u = "$root\tools\uniquify_asset_symbols.py"
    # chr, gun and prop need --recurse: their models are <dir>/<name>/Model.c, so a flat glob
    # finds only .inc.c files, which the tool skips, and it prints nothing and exits 0.
    & python $u 'assets/obseg/chr'  '--recurse'; if ($LASTEXITCODE -ne 0) { Die "namespacing chr failed" }
    & python $u 'assets/obseg/gun'  '--recurse'; if ($LASTEXITCODE -ne 0) { Die "namespacing gun failed" }
    & python $u 'assets/obseg/prop' '--recurse'; if ($LASTEXITCODE -ne 0) { Die "namespacing prop failed" }
    # setup must NOT be recursed: its level setups sit flat and take the file stem as prefix.
    & python $u 'assets/obseg/setup';            if ($LASTEXITCODE -ne 0) { Die "namespacing setup failed" }
    # setup/u passed directly, which collapses the prefix to the bare stem, matching the rest.
    #
    # The exit code is not the test here, and this is the only step where that is true.
    # UsetuplenZ.c stores a real CreditsEntry* in an `s32 intro[]` slot, which is not a
    # compile-time constant at 64-bit, so the pass cannot read its globals and exits non-zero on
    # every platform -- 0002 supplies a corrected copy a few lines below, which is the whole
    # reason 0002 is applied after this and not before. The skip LIST is the test instead:
    # anything in it other than UsetuplenZ.c is a setup file left silently colliding, and a
    # colliding setup file binds a level to another level's data while the build still succeeds.
    # install.sh has read it this way for a while; this side still treated the exit code as
    # fatal and stopped the install before 0002 could ever be reached.
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $nsout = & python $u 'assets/obseg/setup/u' 2>&1
    $nsrc  = $LASTEXITCODE
    $ErrorActionPreference = $eap
    $nslines = $nsout | ForEach-Object { $_.ToString() }
    $unexpected = $nslines | Where-Object { $_ -match '^SKIP' -and $_ -notmatch 'UsetuplenZ\.c' }
    if ($unexpected) {
      $nslines | Select-Object -Last 20 | ForEach-Object { Write-Output "      $_" }
      Die "namespacing setup/u skipped a file 0002 does not supply:`n$($unexpected -join [Environment]::NewLine)"
    }
    if ($nsrc -ne 0) { Info "setup/u: UsetuplenZ.c skipped, which is the known case 0002 supplies" }
    # stan is the easy one to leave out and the omission is silent: 29 definitions of _tile_0 in
    # a static archive is not an error, it just quietly binds 28 levels to the wrong collision
    # data. That is the exact fault this pass exists to prevent.
    & python $u 'assets/obseg/stan';             if ($LASTEXITCODE -ne 0) { Die "namespacing stan failed" }
  } finally { Pop-Location }
  New-Item -ItemType File -Force -Path $marker | Out-Null
  Info "done"
}

# Convert the generated logo words into an explicit byte stream locally. The script is
# path-confined, validates every expected declaration before writing, and is safe to rerun.
& python "$root\tools\transform_rarewarelogo.py"
if ($LASTEXITCODE -ne 0) { Die "local Rareware logo transform failed" }

# And only now 0002, which carries the corrected font files. Over assets/font the tool
# double-prefixes an already-prefixed symbol while leaving the uses alone, so the patch supplies
# those two translation units instead of the tool producing them.
Push-Location $decomp
try {
  # Drained and with the preference relaxed, for the same two reasons as the patch loop in
  # section 3: 2>$null leaves nothing reading the pipe, and 0002 carries generated asset sources,
  # so a rejection here writes far more than a pipe buffer holds. Left as it was, this is the
  # section 3 hang waiting to happen against a bigger patch.
  $eap = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  & git apply --reverse --check "$root\getv\patches\0002-assets.patch" 2>&1 | Out-Null
  $revRc = $LASTEXITCODE
  $ErrorActionPreference = $eap
  if ($revRc -eq 0) {
    Info "0002-assets.patch: already applied"
  } else {
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & git apply "$root\getv\patches\0002-assets.patch" 2>&1 | Out-Null
    $applyRc = $LASTEXITCODE
    $ErrorActionPreference = $eap
    if ($applyRc -eq 0) {
      Info "0002-assets.patch: applied"
    } elseif ($namespacedNow) {
      Die "0002-assets.patch failed to apply to a freshly generated tree. See getv\patches\README.md"
    } else {
      # An established tree is a different situation and must not be treated as a failure.
      # Assets here have been regenerated since 0002 was cut, so it applies in neither direction
      # while the tree itself builds and runs. Forcing it would be the damaging move.
      Info "0002-assets.patch does not apply in either direction against this tree."
      Info "That is drift between the patch and regenerated assets, not a broken checkout,"
      Info "and it is left alone deliberately. A fresh install is the case 0002 is for."
    }
  }
} finally { Pop-Location }

# getv\port\include\PR and platform_info.h are relative symlinks into vendor\ge-decomp\include.
# Git for Windows clones with core.symlinks=false unless told otherwise, and then materialises
# each one as an ORDINARY TEXT FILE whose contents are the link target. Nothing complains at
# checkout; the build simply cannot find <PR/gbi.h>, which reads as a missing header rather than
# as a clone that never made the link -- 16 port-layer files fail and the run ends on "the build
# reported success but goldeneye.exe is not there".
#
# build_linux.sh guards this with require_symlinks and stops. Stopping is not enough here: on
# Windows this is the DEFAULT clone behaviour, not a misconfiguration, so telling the person to
# re-clone with symlinks enabled would make it their problem for doing the normal thing. A
# junction stands in for the directory and a hard link for the file; neither needs the elevation
# a real symlink would, and both track the decomp rather than copying it stale.
function Repair-PortLink ($linkPath, $isDir) {
  $full = Join-Path $root $linkPath
  if (-not (Test-Path $full)) { return }
  # A real link resolves; only a materialised placeholder is a small file holding a relative path.
  $item = Get-Item -LiteralPath $full -Force
  if ($item.PSIsContainer -or $item.LinkType) { return }
  if ($item.Length -gt 512) { return }
  $target = (Get-Content -LiteralPath $full -Raw).Trim()
  if (-not $target -or $target -notmatch '^\.\.[\\/]') { return }
  $resolved = Join-Path (Split-Path -Parent $full) ($target -replace '/', '\')
  if (-not (Test-Path $resolved)) { Die "$linkPath points at $target, which does not exist" }
  Remove-Item -LiteralPath $full -Force
  if ($isDir) {
    New-Item -ItemType Junction -Path $full -Target (Resolve-Path $resolved) | Out-Null
  } else {
    New-Item -ItemType HardLink -Path $full -Target (Resolve-Path $resolved) | Out-Null
  }
  Info "$linkPath`: re-made as a $(if ($isDir) { 'junction' } else { 'hard link' }); this clone had no symlink support"
}
Repair-PortLink 'getv\port\include\PR' $true
Repair-PortLink 'getv\port\include\platform_info.h' $false

# ---------------------------------------------------------------- 7. build

if ($NoBuild) {
  Say "build skipped (-NoBuild)"
  Write-Output ""
  Write-Output "  assets are ready. Build with:"
  Write-Output "    powershell -NoProfile -ExecutionPolicy Bypass -File getv\build_windows.ps1 -Target all"
  exit 0
}

Say "building"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\getv\build_windows.ps1" -Target all -Mingw $Mingw
$bin = Join-Path $root 'getv\build-windows\goldeneye.exe'
if (-not (Test-Path $bin)) { Die "the build reported success but $bin is not there" }
Info "built $bin"

Write-Output ""
Write-Output "== done"
Write-Output ""
Write-Output "  run it:    $bin"
Write-Output "  launcher:  $bin --launcher"
Write-Output "  settings:  docs\CONFIGURATION.md"
Write-Output "  self-test: powershell -File getv\port\tests\run_tests.ps1"
