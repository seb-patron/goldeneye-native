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

# Same reason as the ImGui block in tools/fetch_deps_windows.ps1: assert and __FILE__ strings
# otherwise carry the absolute path this was built from, and the resulting binary is published
# for other people to download. $root is wherever the developer happened to clone, so it is
# mapped to a fixed token rather than shipped.
$cflags = @(
  '-std=c++17', '-O1', '-w',
  "-ffile-prefix-map=$root=goldeneye-native",
  "-fmacro-prefix-map=$root=goldeneye-native",
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
# -s strips the symbol table. Nothing here is debugged from a shipped binary, and a symbol
# table is another place build paths survive.
# -static, not just -static-libgcc/-static-libstdc++. README.md step 4 tells a Windows user to
# download setup_wizard.exe and double-click it -- one file, nothing else. The first build of
# this did not honour that: it imported SDL2.dll, libstdc++-6.dll and libwinpthread-1.dll, and a
# Windows program that cannot find a DLL does not say so. It exits with 0xC0000135, prints
# nothing, and leaves someone staring at a file that appears to do nothing when double-clicked.
#
# Measured while fixing it: run from a directory holding only the .exe, the dynamic build printed
# no output at all and still exited 0 through cmd's ERRORLEVEL, so a test that checked only the
# exit code called it a pass. Check for real output, not rc.
#
# The extra -l flags after SDL2 are what libSDL2.a itself needs once it is no longer a DLL
# (setupapi/version/uuid/cfgmgr32/hid for device enumeration, ole32/oleaut32/shell32 for COM and
# drag-drop, winmm for timers). The DLL copy that used to sit below this is gone with them.
$linkArgs = @('-o', $bin, '-s', '-static') + $objs + @(
  (Join-Path $imgui 'lib\libimgui.a'),
  '-lglew32', '-lmingw32', '-lSDL2',
  '-lopengl32', '-lgdi32', '-limm32', '-ldbghelp', '-lcomdlg32', '-lole32',
  '-loleaut32', '-lshell32', '-lsetupapi', '-lversion', '-luuid', '-ladvapi32',
  '-lcfgmgr32', '-lhid', '-lwinmm', '-lm'
)
$out = & $gxx @linkArgs 2>&1
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bin)) {
  $out | Select-Object -First 40 | ForEach-Object { Write-Output $_ }
  throw "LINK FAILED (gcc exit $LASTEXITCODE)"
}

# The point of -static is that this file stands alone, and the way that silently regresses is a
# library quietly going back to its import-library form. So ask the binary rather than trusting
# the flags: anything imported that is not a Windows system DLL means the download is broken for
# everyone who does not already have a mingw toolchain on their PATH.
$objdump = Join-Path $Mingw 'bin\objdump.exe'
if (Test-Path $objdump) {
  $sys = @('kernel32','user32','gdi32','advapi32','shell32','ole32','oleaut32','opengl32',
           'comdlg32','imm32','setupapi','version','winmm','uuid','cfgmgr32','hid','dbghelp',
           'msvcrt','ucrtbase','ws2_32','shlwapi','rpcrt4','crypt32','bcrypt','iphlpapi')
  $bad = @()
  foreach ($line in (& $objdump -p $bin | Select-String 'DLL Name:')) {
    $name = ($line -replace '.*DLL Name:\s*','').Trim()
    $stem = [IO.Path]::GetFileNameWithoutExtension($name).ToLower()
    if ($stem -like 'api-ms-*') { continue }
    if ($sys -notcontains $stem) { $bad += $name }
  }
  if ($bad.Count -gt 0) {
    Write-Output ("NOT STANDALONE -- still imports: " + ($bad -join ', '))
    throw "setup_wizard.exe is not self-contained; README step 4 promises a single file"
  }
  Write-Output "imports: Windows system DLLs only, so the .exe ships on its own"
}

Write-Output ("wizard binary: {0} ({1:N1} MB)" -f $bin, ((Get-Item $bin).Length / 1MB))
