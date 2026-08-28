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
  # need a PowerShell rewrite of it.
  $bash = Join-Path (Split-Path -Parent (Split-Path -Parent (Get-Command git).Source)) 'bin\bash.exe'
  if (-not (Test-Path $bash)) { $bash = 'bash' }
  & $bash -lc "cd '$($root -replace '\\','/')' && bash tools/fetch-thirdparty.sh fetch"
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
      & git apply --check $p.FullName 2>$null
      if ($LASTEXITCODE -eq 0) {
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

$romDest = Join-Path $root 'roms\ge007.u.z64'
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
      $hit = Get-ChildItem $d -File -Include *.z64,*.n64,*.v64 -ErrorAction SilentlyContinue |
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

Say "generating the asset sources"

# Order is not stylistic. It was established by building from a clean checkout and every entry
# has something downstream that fails without it, usually with an error naming a different file.
# docs/SETUP.md 3.5 is the long form. Each step is skipped if its own output is already there,
# so a run that stops halfway is resumed by running this again.
#
# Markers are a specific file each generator writes, never the directory it writes into: an empty
# directory left by a run that died halfway would otherwise read as "done".
function Invoke-AssetStep ($marker, $label, $exe, $argv) {
  $full = Join-Path $decomp $marker
  if ((Test-Path $full) -or (Get-ChildItem -Path $full -ErrorAction SilentlyContinue)) {
    Info "$label`: already done"; return
  }
  Info $label
  Push-Location $decomp
  try {
    & $exe @argv 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Die "$label failed" }
  } finally { Pop-Location }
}

# Must run BEFORE extraction. The decomp ships 25 of the 34 bg rows with their extract flag at 0
# because upstream builds those from checked-in .c files; this port compiles the blobs, so
# without it the link ends with 25 undefined symbols and nothing earlier hints at why. Running it
# twice is harmless, so it is not marker-guarded.
Info "enabling background extraction"
Push-Location $decomp
try { & python "$root\tools\enable_bg_extraction.py" 2>&1 | Out-Null } finally { Pop-Location }

Invoke-AssetStep 'assets\obseg\bg\bg_ame_all_p.bin'      'extracting from the ROM' 'bash' @('scripts/extract_baserom.u.sh')
Invoke-AssetStep 'assets\obseg\chr\*\Model.c'            'character models'        'python' @('scripts/generate_chr_c.py')
Invoke-AssetStep 'assets\obseg\gun\*\Model.c'            'weapon models'           'python' @('scripts/generate_gun_c.py')
Invoke-AssetStep 'assets\obseg\prop\*\Model.c'           'prop models'             'python' @('scripts/generate_prop_model_c.py')
Invoke-AssetStep 'assets\obseg\ge_obseg_blobs.c'         'obseg blobs'             'python' @("$root\tools\gen_obseg_blobs.py")
Invoke-AssetStep 'build\imagelist.csv'                   'image list'              'python' @('scripts/make/sync_imagelist_with_def.py','build/imagelist.csv')
Invoke-AssetStep 'assets\images\combined\combined.bin'   'combining images'        'bash' @('scripts/make/combine_images_named.sh','build/imagelist.csv','assets/images/combined')
# combined.bin becomes a C array rather than an object. Upstream turns it into one with
# `ld -r -b binary`, a GNU extension with no Mach-O equivalent, so the bytes are emitted as C.
Invoke-AssetStep 'assets\images\ge_images_segment.c'     'images segment'          'python' @("$root\tools\gen_images_segment.py")
Invoke-AssetStep 'assets\ge_animation_offsets.h'         'animation blobs'         'python' @("$root\tools\gen_anim_blobs.py")
Invoke-AssetStep 'src\ge_audio_segment.h'                'audio segment'           'python' @("$root\tools\gen_audio_segment.py")
Invoke-AssetStep 'src\ge_asset_fileview.h'               'asset file views'        'python' @("$root\tools\gen_asset_fileview.py")

# No marker on either of these, for opposite reasons.
#
# fix_asset_switchnodes retypes `u32 SwitchNodes[]` to real ModelNode pointers, and its own regex
# requires the `u32` spelling, so a converted file no longer matches and a second run is a no-op.
# It is idempotent by construction rather than by a guard.
Info "switch nodes"
Push-Location $decomp
try { & python "$root\tools\fix_asset_switchnodes.py" 2>&1 | Out-Null } finally { Pop-Location }

# gen_propdef_layout is a CHECK, not a generator, despite sitting in a list of generators. It
# compiles a throwaway translation unit, dumps the record layouts and asserts the N64 file layout
# still matches the native one. It writes nothing the build consumes, so it runs every time.
Info "propdef layout check"
Push-Location $decomp
try {
  $out = & python 'tools/gen_propdef_layout.py' 2>&1
  if ($LASTEXITCODE -ne 0) { $out | Write-Output; Die "propdef layout check failed. The output above is its report." }
} finally { Pop-Location }

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
$marker = Join-Path $decomp '.getv-assets-namespaced'
$setupC = Get-ChildItem "$decomp\assets\obseg\setup\*.c" -ErrorAction SilentlyContinue
$bare   = $false
foreach ($f in $setupC) {
  if (Select-String -Path $f.FullName -Pattern '(^|\s)padlist\[' -Quiet) { $bare = $true; break }
}
$namespacedNow = $false
if ((Test-Path $marker) -or (($setupC.Count -gt 0) -and -not $bare)) {
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
    & python $u 'assets/obseg/setup/u';          if ($LASTEXITCODE -ne 0) { Die "namespacing setup/u failed" }
    # stan is the easy one to leave out and the omission is silent: 29 definitions of _tile_0 in
    # a static archive is not an error, it just quietly binds 28 levels to the wrong collision
    # data. That is the exact fault this pass exists to prevent.
    & python $u 'assets/obseg/stan';             if ($LASTEXITCODE -ne 0) { Die "namespacing stan failed" }
  } finally { Pop-Location }
  New-Item -ItemType File -Force -Path $marker | Out-Null
  Info "done"
}

# And only now 0002, which carries the corrected font files. Over assets/font the tool
# double-prefixes an already-prefixed symbol while leaving the uses alone, so the patch supplies
# those two translation units instead of the tool producing them.
Push-Location $decomp
try {
  & git apply --reverse --check "$root\getv\patches\0002-assets.patch" 2>$null
  if ($LASTEXITCODE -eq 0) {
    Info "0002-assets.patch: already applied"
  } else {
    & git apply "$root\getv\patches\0002-assets.patch" 2>$null
    if ($LASTEXITCODE -eq 0) {
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
