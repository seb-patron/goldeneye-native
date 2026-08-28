<#
  Build the standalone setup wizard: getv/wizard/setup_wizard.exe.

  A separate, small build from build_windows.ps1 on purpose. The wizard has to run BEFORE the
  decomp is cloned, before a ROM exists, and before goldeneye.exe can be linked -- its entire
  job is to drive tools/setup-windows.sh, which does those things. So it cannot be a target of
  the build it bootstraps, and does not touch build_windows.ps1 or share output with it.

  This is a developer-facing script, not something an end user runs. A developer with the same
  toolchain build_windows.ps1 already needs (mingw, SDL2, GLEW, Dear ImGui -- all from
  tools/fetch_deps_windows.ps1) runs this once; the resulting setup_wizard.exe is what actually
  gets distributed. It contains no ROM-derived or decomp-derived code, which is what makes
  shipping the binary itself fine where shipping goldeneye.exe is not (docs/LICENSING.md
  section 5).

  USAGE
      powershell -NoProfile -File getv\build_wizard.ps1
      -Mingw : toolchain root (default C:\msys64\mingw64, matching build_windows.ps1)
#>
[CmdletBinding()]
param(
  [string]$Mingw = 'C:\msys64\mingw64'
)

$ErrorActionPreference = 'Continue'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path   # getv/
$root  = Split-Path -Parent $here                           # repo root
$wiz   = Join-Path $here 'wizard'
$build = Join-Path $wiz 'build'
$imgui = Join-Path $env:USERPROFILE '.n64tvos\imgui-win'

$gcc = Join-Path $Mingw 'bin\gcc.exe'
$gxx = Join-Path $Mingw 'bin\g++.exe'
if (-not (Test-Path $gcc)) { throw "no gcc at $gcc -- run tools\fetch_deps_windows.ps1 first" }
if (-not (Test-Path (Join-Path $imgui 'lib\libimgui.a'))) {
  throw "no Dear ImGui at $imgui -- run tools\fetch_deps_windows.ps1 first"
}

New-Item -ItemType Directory -Force -Path $build | Out-Null

$cflags = @(
  '-std=c++17', '-O1', '-w',
  "-I$wiz",
  "-I$root\getv\port\src",
  "-I$imgui\include",
  "-I$Mingw\include\SDL2"
)

Write-Output "== compiling =="
$objs = @()
$sources = @(
  @{ src = Join-Path $wiz 'setup_wizard.cpp';                    cxx = $true  },
  @{ src = Join-Path $wiz 'sha1.c';                               cxx = $false },
  @{ src = Join-Path $root 'getv\port\src\ge_icon_apply.c';       cxx = $false }
)
foreach ($s in $sources) {
  $o = Join-Path $build ((Split-Path -Leaf $s.src) + '.o')
  $cc = if ($s.cxx) { $gxx } else { $gcc }
  $out = & $cc @cflags -c $s.src -o $o 2>&1
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $o)) {
    $out | ForEach-Object { Write-Output $_ }
    throw "compile failed: $($s.src)"
  }
  Write-Output "  $(Split-Path -Leaf $s.src)"
  $objs += $o
}

Write-Output "== linking =="
$bin = Join-Path $build 'setup_wizard.exe'
Remove-Item $bin -Force -ErrorAction SilentlyContinue
$linkArgs = @('-o', $bin) + $objs + @(
  (Join-Path $imgui 'lib\libimgui.a'),
  '-lglew32', '-lmingw32', '-lSDL2',
  '-static-libgcc', '-static-libstdc++',
  '-lstdc++', '-lopengl32', '-lgdi32', '-limm32', '-ldbghelp', '-lcomdlg32', '-lole32', '-lm'
)
$out = & $gxx @linkArgs 2>&1
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bin)) {
  $out | Select-Object -First 40 | ForEach-Object { Write-Output $_ }
  throw "LINK FAILED (gcc exit $LASTEXITCODE)"
}

# Same set build_windows.ps1 copies beside goldeneye.exe, and for the same reason: a missing
# one of these exits with 0xC0000135 before main() runs, no message, nothing on stdout.
foreach ($d in @('SDL2.dll', 'libwinpthread-1.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll')) {
  $src = Join-Path $Mingw ('bin\' + $d)
  if (Test-Path $src) { Copy-Item $src (Join-Path $build $d) -Force }
}

Write-Output ("wizard binary: {0} ({1:N1} MB)" -f $bin, ((Get-Item $bin).Length / 1MB))
