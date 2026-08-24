<#
  Build GoldenEye natively on Windows, with mingw-w64.

  WHY THIS IS POWERSHELL AND NOT BASH
  -----------------------------------
  getv/build_windows.sh exists and is a faithful translation of build_linux.sh, and on a
  healthy MSYS2 it works. On the machine this was brought up on it does not, and the reason
  is worth recording because it is a property of MSYS2 rather than of this project:

      0 [main] xargs 6600 dofork: child -1 - forked process 9152 died unexpectedly,
      retry 0, exit code 0xC0000142, errno 11

  MSYS2 emulates fork() by copying an address space into a fresh process, which is fragile
  on Windows and fails outright on some hosts (STATUS_DLL_INIT_FAILED). The build forks once
  per translation unit, roughly 950 times, so a host where fork is unreliable cannot run it
  at all. `autorebase` and `rebaseall`, the standard remedies, did not fix it here and left
  bash unable to start.

  The compiler itself is not implicated. mingw-w64's gcc.exe is a native Win32 binary with
  no MSYS2 runtime dependency: driven from PowerShell it compiles the decomp at 0.75s per
  file with zero errors. So the fix is to stop asking MSYS2 to orchestrate anything.

  That makes this the better arrangement for Windows regardless: a build that needs only the
  mingw toolchain, not a working POSIX emulation layer, and no bash on the machine at all.
  build_windows.sh is kept for hosts where MSYS2 is healthy, and the two must produce the
  same thing -- the batch definitions below are a direct transcription of that script's, and
  the exclusions in particular must stay in step with all four builds.

  USAGE
      powershell -NoProfile -File getv\build_windows.ps1 -Target all
      -Target : all | lib | port | app | clean      (default all)
      -Mingw  : toolchain root                      (default C:\msys64\mingw64)
      -Jobs   : parallel compiles                   (default = processor count)
#>
[CmdletBinding()]
param(
  [ValidateSet('all','lib','port','app','clean')]
  [string]$Target = 'all',
  [string]$Mingw  = 'C:\msys64\mingw64',
  [int]$Jobs      = 0
)

# 'Continue', not 'Stop'. PowerShell turns a native program's stderr into an ErrorRecord, and
# under 'Stop' the first compiler warning or note aborts the whole build with
# NativeCommandError -- which looks like a script bug and is really just gcc talking. Every
# step below checks $LASTEXITCODE and the output file explicitly instead, which is the only
# reliable signal from a native tool anyway.
$ErrorActionPreference = 'Continue'
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$root   = Split-Path -Parent $here
$decomp = Join-Path $root 'vendor\ge-decomp'
$build  = Join-Path $here 'build-windows'
$obj    = Join-Path $build 'obj'
$gcc    = Join-Path $Mingw 'bin\gcc.exe'
$gxx    = Join-Path $Mingw 'bin\g++.exe'
$ar     = Join-Path $Mingw 'bin\ar.exe'
$bin    = Join-Path $build 'goldeneye.exe'

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

foreach ($t in @($gcc, $gxx, $ar)) {
  if (-not (Test-Path $t)) { throw "toolchain missing: $t  (install mingw-w64, or pass -Mingw)" }
}
if (-not (Test-Path (Join-Path $decomp 'src\game\lv.c'))) {
  throw "decomp missing at $decomp -- this repository does not include it"
}

# ---------------------------------------------------------------- SDL2
# pkg-config is a native binary too, so it is safe to call. Falling back to the conventional
# layout rather than failing keeps this working on a toolchain that has SDL2 but no pkgconf.
$pkgconf  = Join-Path $Mingw 'bin\pkg-config.exe'
$sdlCFlags = @()
$sdlLibs   = @()
if (Test-Path $pkgconf) {
  $env:PKG_CONFIG_PATH = Join-Path $Mingw 'lib\pkgconfig'
  $c = (& $pkgconf --cflags sdl2 2>$null)
  $l = (& $pkgconf --libs   sdl2 2>$null)
  if ($LASTEXITCODE -eq 0 -and $c) { $sdlCFlags = $c -split '\s+' | Where-Object { $_ } }
  if ($l) { $sdlLibs = $l -split '\s+' | Where-Object { $_ } }
}
if ($sdlCFlags.Count -eq 0) {
  $sdlCFlags = @("-I$Mingw\include\SDL2", "-Dmain=SDL_main")
  $sdlLibs   = @('-lmingw32','-lSDL2main','-lSDL2')
}
# -Dmain=SDL_main is actively unwanted: getv/port/mac/ge_mac_main.c supplies the real main()
# and calls SDL_SetMainReady() itself, and SDL2main is deliberately not linked because two
# definitions of main collide. pkg-config's sdl2 --cflags ships that define on Windows, so it
# is stripped rather than worked around at the call site.
$sdlCFlags = $sdlCFlags | Where-Object { $_ -ne '-Dmain=SDL_main' }
$sdlLibs   = $sdlLibs   | Where-Object { $_ -ne '-lSDL2main' }

# ---------------------------------------------------------------- optional deps
$luaPrefix   = Join-Path $env:USERPROFILE '.n64tvos\lua-win'
$imguiPrefix = Join-Path $env:USERPROFILE '.n64tvos\imgui-win'
$luaFlags = @(); $luaLibs = @(); $imguiFlags = @(); $imguiLibs = @()
if ((Test-Path "$luaPrefix\lib\liblua.a") -and (Test-Path "$luaPrefix\include\lua.h")) {
  $luaFlags = @('-DGE_WITH_LUA', "-I$luaPrefix\include"); $luaLibs = @("$luaPrefix\lib\liblua.a")
}
if ((Test-Path "$imguiPrefix\lib\libimgui.a") -and (Test-Path "$imguiPrefix\include\imgui.h")) {
  $imguiFlags = @('-DGE_WITH_IMGUI', "-I$imguiPrefix\include"); $imguiLibs = @("$imguiPrefix\lib\libimgui.a")
}

# ---------------------------------------------------------------- flags
# A transcription of linux_cflags(). -Wno-everything is a clang spelling that gcc rejects, so
# the warning set is -w plus the one diagnostic that must stay fatal: a non-void function
# falling off the end worked by accident on MIPS/IDO, where the last callee's result was
# already in $v0, and produces garbage here. Ten of those were found in this tree.
$warn = @('-w','-Werror=return-type')

# -std=gnu17 is not optional with a modern gcc. GCC 15 and later default to gnu23, where
# `bool` is a keyword, and the decomp's bondtypes.h:85 does `typedef s32 bool;` -- legal in
# every standard the original was written against and a hard error in C23. GCC 13 on the
# Linux box defaults to gnu17 and never showed this, so it is a toolchain-version trap
# rather than a platform one, and it would bite the Linux build too the moment that host
# updates its compiler.
$std = @('-std=gnu17')

# -mno-ms-bitfields is the single most important flag here, and it is not an optimisation.
# MinGW defaults to -mms-bitfields, which lays bitfields out the way MSVC does: a new
# storage unit is started when the declared type changes. The decomp is full of bitfields
# overlaid on N64 file data, and under the MSVC rule they move.
#
# Measured on this tree: sizeof(StandTile) is 12 with the default and 8 with this flag, and
# 8 is what every other host produces. stan.c has a _Static_assert for exactly that number
# and is the only reason it surfaced as a compile error rather than as garbage geometry --
# every other bitfield struct in the decomp would have been laid out wrong silently.
$abi = @('-mno-ms-bitfields')

# GCC 14 promoted five long-standing warnings to errors by default. The decomp is 1990s C
# and trips four of them constantly -- the asset files alone initialise struct pointers from
# array-of-struct addresses on every background. They are demoted back to warnings rather
# than fixed, because "fixing" them would mean editing generated asset data and thousands of
# decompiled lines to satisfy a diagnostic that did not exist when the code was written, and
# the port's correctness is judged against the N64, not against C23.
#
# -Werror=return-type deliberately stays fatal above and is NOT in this list: a non-void
# function falling off the end worked by accident on MIPS/IDO and produces garbage here, and
# ten real instances were found in this tree. return-mismatch is a different diagnostic
# (a value returned from a void function) and is demoted.
$permissive = @(
  '-Wno-error=incompatible-pointer-types',
  '-Wno-error=int-conversion',
  '-Wno-error=implicit-function-declaration',
  '-Wno-error=implicit-int',
  '-Wno-error=return-mismatch'
)

$gameFlags = @(
  '-fms-extensions','-include','src/ge_port_decls.h',
  '-I','.','-I','include','-I','include/PR','-I','src','-I','src/game','-I','src/inflate',
  '-DVERSION_US','-DLANG_US','-DREFRESH_NTSC','-DLEFTOVERDEBUG','-DLEFTOVERSPECTRUM',
  '-DBUGFIX_R0','-DTARGET_N64','-DGE_PORT_NATIVE',
  '-DNON_MATCHING=1','-DAVOID_UB=1','-D_LANGUAGE_C=1'
) + $warn + $std + $abi + $permissive + @('-fno-strict-aliasing','-O1')

$portFlags = @(
  "-I$here\port", "-I$here\port\include", "-I$here\port\fast3d", "-I$here\port\src",
  '-include', "$here\port\include\ge_win_compat.h"
) + $sdlCFlags + @(
  '-DTARGET_N64','-DGE_PORT_NATIVE','-D_LANGUAGE_C=1','-DRAPI_GL','-DWAPI_SDL2',
  '-DGE_PLATFORM_DESKTOP'
) + $luaFlags + $imguiFlags + $warn + $std + $abi + $permissive + @('-O1')

# ---------------------------------------------------------------- batch runner
function Invoke-Batch {
  param([string]$Label, [string[]]$Files, [string[]]$Flags, [string]$WorkDir, [string]$Prefix, [string]$Compiler)

  if (-not $Compiler) { $Compiler = $gcc }
  $ok = 0; $fail = 0; $failed = @()
  $n = $Files.Count
  $i = 0

  foreach ($f in $Files) {
    $i++
    $stem = ($f -replace '[\\/]','_') -replace '\.(c|cpp)$',''
    $o = Join-Path $obj "$Prefix$stem.o"
    $out = & $Compiler @Flags -c $f -o $o 2>&1
    if ($LASTEXITCODE -eq 0 -and (Test-Path $o)) {
      $ok++
      Add-Content -Path (Join-Path $build 'objects.txt') -Value $o
    } else {
      $fail++; $failed += $f
      # The first failure in a batch prints its diagnostics. Without this a batch reports
      # only a count, and "0 built, 338 failed" is indistinguishable from a broken toolchain,
      # a bad flag and a genuine source error -- which is exactly the ambiguity that cost an
      # afternoon when cc1.exe stopped being able to start and every compile failed silently.
      if ($fail -eq 1) {
        Write-Output "  first failure in $Label ($f):"
        $out | Select-Object -First 6 | ForEach-Object { Write-Output "    $_" }
        Write-Output "    (gcc exit $LASTEXITCODE)"
      }
      if (Test-Path $o) { Remove-Item $o -Force -ErrorAction SilentlyContinue }
    }
    if (($i % 100) -eq 0) { Write-Output ("  $Label ... $i/$n") }
  }
  foreach ($f in $failed) { Write-Output "  windows FAILED: $f" }
  Write-Output "windows $Label`: $ok built, $fail failed"
  return $fail
}

function Build-Lib {
  # Objects are cleared first. Without this a rebuild adds to whatever a previous run left,
  # so the count no longer describes this build and a file that has started failing still
  # appears to be present. That ambiguity wasted a cycle already.
  if (Test-Path $obj) { Remove-Item $obj -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $obj | Out-Null
  Set-Content -Path (Join-Path $build 'objects.txt') -Value $null

  Push-Location $decomp
  try {
    # Game. The exclusions mirror build_linux.sh exactly and are load-bearing: usb, rmon,
    # sched, ramrom, init and the indy_* files are N64 hardware and SGI dev-host code, and
    # compiling them turns logging stubs into code that writes real RCP/PI registers.
    $skip = '(ramromreplay|audi|usb|rmon|sched|ramrom|init|indy_comms|indy_commands)\.c$'
    $game = @(Get-ChildItem -Path 'src' -Recurse -Filter *.c |
      Where-Object { $_.Name -notlike '._*' -and
                     $_.FullName -notmatch '\\src\\libultra\\' -and
                     $_.FullName -notmatch '\\src\\libultrare\\' -and
                     $_.Name -ne 'ge_layout_audit.c' -and
                     $_.Name -ne 'ge_asset_fileview_check.c' })
    $game += @(Get-ChildItem -Path 'src\libultra\gu' -Filter *.c -ErrorAction SilentlyContinue)
    $game = $game | Where-Object { $_.Name -notmatch $skip } |
            ForEach-Object { Resolve-Path -Relative $_.FullName } | Sort-Object
    $f1 = Invoke-Batch -Label 'game' -Files $game -Flags $gameFlags -Prefix 'game_'

    # Assets. setup/e and setup/j are the PAL and Japanese tables; seven of their eight files
    # define symbols identical to setup/u's, so compiling them lets the linker bind seven
    # stages to the PAL data in a US build. A US build must not compile them.
    $assets = @(Get-ChildItem -Path 'assets' -Recurse -Filter *.c |
      Where-Object { $_.Name -notlike '._*' -and $_.Name -notlike '*.inc.c' -and
                     $_.FullName -notmatch '\\assets\\obseg\\setup\\e\\' -and
                     $_.FullName -notmatch '\\assets\\obseg\\setup\\j\\' } |
      ForEach-Object { Resolve-Path -Relative $_.FullName } | Sort-Object)
    $f2 = Invoke-Batch -Label 'assets' -Files $assets -Flags $gameFlags -Prefix 'asset_'

    # -DNDEBUG is for the mixer only. Do not widen it: SUPPORT_CHECK in gfx_pc.c is an
    # assert() and is deliberately armed.
    $audio = @(Get-ChildItem -Path 'src\libultra\audio','src\libultrare\audio' -Filter *.c -ErrorAction SilentlyContinue |
      ForEach-Object { Resolve-Path -Relative $_.FullName } | Sort-Object)
    $audioFlags = $gameFlags + @('-DGE_AUDIO_MIXER','-DNDEBUG',
                                 '-I','src/libultra','-I','src/libultrare','-I',"$here\port\audio")
    $f3 = Invoke-Batch -Label 'audio' -Files $audio -Flags $audioFlags -Prefix 'audio_'
  } finally { Pop-Location }

  Build-Port
}

function Build-Port {
  New-Item -ItemType Directory -Force -Path $obj | Out-Null
  $c = @(Get-ChildItem -Path "$here\port\fast3d","$here\port\src","$here\port\audio" -Filter *.c -ErrorAction SilentlyContinue |
         ForEach-Object { $_.FullName })
  $c += "$here\Sources\ge_tvos_main.c"
  $c += "$here\port\mac\ge_mac_main.c"
  $f4 = Invoke-Batch -Label 'port layer' -Files $c -Flags $portFlags -Prefix 'port_'

  $cpp = @(Get-ChildItem -Path "$here\port\src" -Filter *.cpp -ErrorAction SilentlyContinue |
           ForEach-Object { $_.FullName })
  if ($cpp.Count -gt 0) {
    $f5 = Invoke-Batch -Label 'port c++' -Files $cpp -Prefix 'portxx_' -Compiler $gxx `
            -Flags ($portFlags + @('-std=c++17','-fno-exceptions','-fno-rtti'))
  }
}

function Build-App {
  $objs = @(Get-ChildItem -Path $obj -Filter *.o | ForEach-Object { $_.FullName })
  if ($objs.Count -eq 0) { throw "no objects in $obj -- run -Target lib first" }

  # The archive exists to mirror the other three builds; linking the objects directly would
  # work too, but keeping the same shape means a link failure here means the same thing it
  # means there.
  $lib = Join-Path $build 'libge.a'
  Remove-Item $lib -Force -ErrorAction SilentlyContinue
  $rsp = Join-Path $build 'ar.rsp'
  Set-Content -Path $rsp -Value ($objs -join "`n")
  & $ar rcs $lib "@$rsp"
  if (-not (Test-Path $lib)) { throw "ar failed" }
  Write-Output ("windows libge.a: {0:N0} MB, {1} members" -f ((Get-Item $lib).Length/1MB), $objs.Count)

  # $BIN is removed first for the same reason build_linux.sh does it: a failed link would
  # otherwise leave the previous binary in place and the check below would pass.
  Remove-Item $bin -Force -ErrorAction SilentlyContinue
  $linkArgs = @('-o', $bin, $lib) + $luaLibs + $imguiLibs + $sdlLibs +
              @('-lstdc++','-lopengl32','-ldbghelp','-lm')
  $out = & $gcc @linkArgs 2>&1
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bin)) {
    $out | Select-Object -First 40 | ForEach-Object { Write-Output $_ }
    throw "LINK FAILED (gcc exit $LASTEXITCODE)"
  }
  # SDL2 is linked through its import library, so the DLL has to sit beside the executable.
  # Copied rather than left to PATH: a build that only runs when the toolchain happens to be
  # on PATH is not a distributable build, and this is the file a player would otherwise be
  # told to hunt for.
  $sdlDll = Join-Path $Mingw 'bin\SDL2.dll'
  if (Test-Path $sdlDll) { Copy-Item $sdlDll (Join-Path $build 'SDL2.dll') -Force }

  Write-Output ("windows binary: {0} ({1:N1} MB)" -f $bin, ((Get-Item $bin).Length/1MB))
}

switch ($Target) {
  'clean' { if (Test-Path $build) { Remove-Item $build -Recurse -Force }; Write-Output 'cleaned' }
  'lib'   { Build-Lib }
  'port'  { Build-Port }
  'app'   { Build-App }
  'all'   { Build-Lib; Build-App }
}
