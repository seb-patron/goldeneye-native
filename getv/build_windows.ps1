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
  [ValidateSet('all','lib','port','app','dist','clean')]
  [string]$Target = 'all',
  [string]$Mingw  = 'C:\msys64\mingw64',
  [int]$Jobs      = 0,

  # Optimisation level, and it is a REAL DIAL rather than a constant, which is why it is here.
  #
  # This tree was built at -O1 with no comment explaining it, in a file where every other flag
  # carries a paragraph of justification -- so it reads as inherited, not chosen. On a decomp that
  # is not automatically wrong: higher levels are more aggressive about code whose behaviour the
  # original compiler defined by accident, and this source is full of that. -fno-strict-aliasing
  # stays on at every level for exactly that reason and is NOT negotiable.
  #
  # Verified rather than assumed: the port's synthetic clock makes gameplay frames deterministic
  # (see osGetCount in port_os.c), so two builds fed the same input must emit byte-identical
  # diagnostics. tools/verify_opt.ps1 compares them. Any level that changes the output is wrong
  # for this project no matter how fast it is -- an archival build that renders different pixels
  # is not an archival build.
  # DEFAULTS TO -O1: THE BEHAVIOUR THIS TREE ALREADY HAD. Introducing the dial and changing the
  # setting in one step would mean every later measurement compared against a baseline nobody had
  # ever run. The default moves only when the determinism check has passed and the numbers are in.
  [ValidateSet('-O0','-O1','-O2','-O3','-Os')]
  [string]$Opt = '-O1',

  # Link-time optimisation. Off by default: it gives the linker licence to act on cross-module
  # assumptions that hold in standards C and not in decompiled MIPS output, and the failure mode
  # is a miscompile rather than an error. Available so it can be measured on demand.
  [switch]$Lto
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

# ---------------------------------------------------------------- GLEW
# Windows needs an extension loader and the renderer already expects one: gfx_opengl.c sets
# FOR_WINDOWS on __MINGW32__ and includes <GL/glew.h> with GLEW_STATIC. opengl32.dll exports
# only GL 1.1, so without a loader nothing past 1997 resolves. macOS gets its entry points
# from the OpenGL framework and Linux from libGL with GL_GLEXT_PROTOTYPES, which is why this
# is the one dependency neither of those builds needs.
$glewLibs = @()
if (Test-Path (Join-Path $Mingw 'lib\libglew32.a')) { $glewLibs = @('-lglew32') }

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

# LTO, when asked for. -ffat-lto-objects keeps a normal object alongside the IR so the static
# library stays usable by a non-LTO link and so `nm` still reports real symbols -- without it a
# missing-symbol hunt turns up nothing but IR stubs, which is a miserable way to lose an hour.
$ltoFlags = @()
if ($Lto) { $ltoFlags = @('-flto','-ffat-lto-objects') }

$gameFlags = @(
  '-fms-extensions','-include','src/ge_port_decls.h',
  '-I','.','-I','include','-I','include/PR','-I','src','-I','src/game','-I','src/inflate',
  '-DVERSION_US','-DLANG_US','-DREFRESH_NTSC','-DLEFTOVERDEBUG','-DLEFTOVERSPECTRUM',
  '-DBUGFIX_R0','-DTARGET_N64','-DGE_PORT_NATIVE',
  '-DNON_MATCHING=1','-DAVOID_UB=1','-D_LANGUAGE_C=1'
  # No compat header in the game batch, deliberately. It was added here to supply bcopy and
  # bzero, and that was the wrong fix twice over: those are now real functions in
  # ge_link_stubs.c, declared by the decomp's own bstring.h, so nothing is needed. Worse,
  # force-including it pulled the real <stdio.h> into every game translation unit, where the
  # decomp's own stdio shadow is what is supposed to win -- initactorpropstuff.c then failed
  # on `conflicting types for fflush`, because the decomp declares it taking void *.
  #
  # The port layer still gets it. The decomp's headers do not shadow anything there.
  # -fno-strict-aliasing is NOT part of the dial and never varies. The decomp reads the same
  # memory through incompatible types constantly -- that was well-defined on IDO/MIPS and is
  # undefined in standard C, so letting the optimiser assume it cannot happen miscompiles this
  # source in ways that surface as wrong pixels rather than as errors.
) + $warn + $std + $abi + $permissive + @('-fno-strict-aliasing', $Opt) + $ltoFlags

$portFlags = @(
  "-I$here\port", "-I$here\port\include", "-I$here\port\fast3d", "-I$here\port\src",
  '-include', "$here\port\include\ge_win_compat.h"
) + $sdlCFlags + @(
  '-DTARGET_N64','-DGE_PORT_NATIVE','-D_LANGUAGE_C=1','-DRAPI_GL','-DWAPI_SDL2',
  '-DGE_PLATFORM_DESKTOP'
) + $luaFlags + $imguiFlags + $warn + $std + $abi + $permissive + @($Opt) + $ltoFlags

# ---------------------------------------------------------------- batch runner
function Invoke-Batch {
  param([string]$Label, [string[]]$Files, [string[]]$Flags, [string]$WorkDir, [string]$Prefix, [string]$Compiler)

  if (-not $Compiler) { $Compiler = $gcc }
  $ok = 0; $fail = 0; $failed = @()
  $n = $Files.Count
  $i = 0

  foreach ($f in $Files) {
    $i++
    # The drive colon has to go: these names are built from full paths for the port layer,
    # and "C:_ge_..." is not a legal filename, so Test-Path throws NotSupportedException and
    # every object silently looks absent.
    $stem = ($f -replace '[\\/]','_') -replace ':','' -replace '\.(c|cpp)$',''
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
        Write-Host "  first failure in $Label ($f):"
        $out | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }
        Write-Host "    (gcc exit $LASTEXITCODE)"
      }
      if (Test-Path $o) { Remove-Item $o -Force -ErrorAction SilentlyContinue }
    }
    if (($i % 100) -eq 0) { Write-Host ("  $Label ... $i/$n") }
  }
  foreach ($f in $failed) { Write-Host "  windows FAILED: $f" }
  Write-Host "windows $Label`: $ok built, $fail failed"
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
    # Game. The exclusions mirror build_linux.sh exactly and matter: usb, rmon,
    # sched, ramrom, init and the indy_* files are N64 hardware and SGI dev-host code, and
    # compiling them turns logging stubs into code that writes real RCP/PI registers.
    # crash.c, spectrum.c and tlb_manage.c are N64 hardware and dev-host stubs, and the port
    # replaces them with getv/port/src/ge_link_stubs.c. On macOS and Linux they are not
    # excluded by name because they simply FAIL to compile there, and that failure is the
    # guard -- the 167/1 split on those builds is intentional and must not be "fixed".
    #
    # This build's permissive flags (see $permissive above) let two of them through, and the
    # result was not a silent success: they compiled, went into the archive, and collided
    # with ge_link_stubs.o on multiple definitions. So the guard still worked, just at link
    # time instead of compile time. Excluded by name here, which is what the other scripts
    # would need too if their compilers ever stopped rejecting these files.
    $skip = '(ramromreplay|audi|usb|rmon|sched|ramrom|init|indy_comms|indy_commands|crash|spectrum|tlb_manage)\.c$'
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
    # The stan format is one flat contiguous run of tiles in declaration order, walked by
    # adding each tile's byte size, and tile pointers are computed as base + (link << 3).
    # GCC emits .data in REVERSE declaration order, which put tile_0 at the end of .data --
    # measured on Tbg_sev: tile_0 at 0x89f8, tile_1 at 0x89b8 -- so the walk ran off the
    # section end after one tile and every link resolved into padding. Clang emits in source
    # order, which is why macOS never showed this. The all-zero terminator tile also lands in
    # .bss by default, which breaks contiguity at the far end.
    $assetFlags = $gameFlags + @('-fno-toplevel-reorder','-fno-zero-initialized-in-bss')
    $f2 = Invoke-Batch -Label 'assets' -Files $assets -Flags $assetFlags -Prefix 'asset_'

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

  # The harness objects are linked directly and kept OUT of the archive, exactly as
  # build_linux.sh does. main() lives in ge_mac_main.o, and nothing in the archive refers to
  # it -- an archive member is only pulled in to satisfy an existing undefined symbol, so
  # burying main() in libge.a means the CRT never finds it. The symptom is not "undefined
  # main": mingw's startup falls through to the GUI path and reports `undefined reference to
  # WinMain', which sends you looking for a subsystem flag that is not the problem.
  # Matched on the source stem, not a fixed object name. Port-layer objects are named from
  # the full source path -- port_C_ge_getv_Sources_ge_tvos_main.o -- so an exact name never
  # matches, and the drive letter makes it host-specific besides.
  $rootStems = @('ge_tvos_main.o','ge_mac_main.o')
  $roots = @()
  foreach ($rn in $rootStems) {
    $cand = @($objs | Where-Object { (Split-Path $_ -Leaf).EndsWith($rn) })
    if ($cand.Count -eq 0) { throw "missing harness object *$rn -- run -Target port first" }
    $roots += $cand[0]
  }
  $objs = $objs | Where-Object { $roots -notcontains $_ }

  # The archive exists to mirror the other three builds; linking the objects directly would
  # work too, but keeping the same shape means a link failure here means the same thing it
  # means there.
  $lib = Join-Path $build 'libge.a'
  Remove-Item $lib -Force -ErrorAction SilentlyContinue
  # Forward slashes in the response file, not the native backslashes. GNU ar treats a
  # backslash as an escape character inside a response file, so "C:\ge\getv\..." arrives as
  # "C:getv..." and the member is silently dropped -- no error, no warning, just a smaller
  # archive. That is what produced a link failing on four symbols that nm could plainly see
  # inside the object file sitting on disk.
  $rsp = Join-Path $build 'ar.rsp'
  Set-Content -Path $rsp -Value (($objs | ForEach-Object { $_ -replace '\\','/' }) -join "`n")
  & $ar rcs $lib "@$rsp"
  if (-not (Test-Path $lib)) { throw "ar failed" }

  # Trust ar's own count, not the file list handed to it. The two disagreeing is exactly the
  # failure above, and checking is one command.
  $inAr = (& $ar t $lib | Measure-Object -Line).Lines
  if ($inAr -lt $objs.Count) {
    throw "ar archived $inAr of $($objs.Count) objects -- members were dropped"
  }
  Write-Output ("windows libge.a: {0:N1} MB, {1} members (+{2} roots)" -f ((Get-Item $lib).Length/1MB), $inAr, $roots.Count)

  # $BIN is removed first for the same reason build_linux.sh does it: a failed link would
  # otherwise leave the previous binary in place and the check below would pass.
  Remove-Item $bin -Force -ErrorAction SilentlyContinue
  # -lgdi32 is required by GLEW's WGL entry points and by SDL2's Windows video backend;
  # neither pulls it in implicitly.
  # -static-libgcc / -static-libstdc++ / -static: a mingw build otherwise depends on
  # libgcc_s_seh-1.dll, libstdc++-6.dll and libwinpthread-1.dll from the toolchain
  # directory. Those are not on a player's machine, and the failure is not a helpful one --
  # the process exits with 0xC0000135 (STATUS_DLL_NOT_FOUND) before main() runs, so there is
  # no message, no log line and nothing on stdout to explain it. Linking them in makes the
  # executable self-contained apart from SDL2.dll, which is copied beside it below.
  # -static-libgcc and -static-libstdc++ only. A full -static was tried and is wrong here: it
  # pulls the static CRT, which collides with the decomp's own str.c on multiple definitions,
  # and forces SDL2 to link statically too, which then needs its entire Windows dependency
  # set (imm32, ole32, setupapi, version, winmm...). These two cover the DLLs that actually
  # go missing on a machine without the toolchain; the rest are copied beside the exe below.
  $linkArgs = @('-o', $bin) + $roots + @($lib) + $luaLibs + $imguiLibs + $glewLibs + $sdlLibs +
              @('-static-libgcc','-static-libstdc++',
                '-lstdc++','-lopengl32','-lgdi32','-limm32','-ldbghelp','-lm')
  $out = & $gcc @linkArgs 2>&1
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bin)) {
    $out | Select-Object -First 40 | ForEach-Object { Write-Output $_ }
    throw "LINK FAILED (gcc exit $LASTEXITCODE)"
  }
  # SDL2 is linked through its import library, so the DLL has to sit beside the executable.
  # Copied rather than left to PATH: a build that only runs when the toolchain happens to be
  # on PATH is not a distributable build, and this is the file a player would otherwise be
  # told to hunt for.
  # SDL2.dll plus any mingw runtime DLL still referenced. A missing one of these exits
  # with 0xC0000135 before main() runs -- no message, no log, nothing on stdout -- so they
  # are copied rather than left to whatever happens to be on PATH.
  foreach ($d in @('SDL2.dll','libwinpthread-1.dll','libgcc_s_seh-1.dll','libstdc++-6.dll')) {
    $src = Join-Path $Mingw ('bin' + [char]92 + $d)
    if (Test-Path $src) { Copy-Item $src (Join-Path $build $d) -Force }
  }

  # The launcher's bundled font, next to the binary where ge_launcher.cpp looks for it first.
  # Roboto Condensed, SIL OFL 1.1; OFL.txt travels with it because the licence requires the
  # copyright notice to be distributed alongside the font.
  $fsrc = Join-Path $here ('port' + [char]92 + 'assets' + [char]92 + 'fonts')
  if (Test-Path $fsrc) {
    $fdst = Join-Path $build ('assets' + [char]92 + 'fonts')
    New-Item -ItemType Directory -Force -Path $fdst | Out-Null
    Copy-Item (Join-Path $fsrc '*') $fdst -Force
    Write-Output ("windows launcher font: {0}" -f $fdst)
  } else {
    Write-Output 'windows launcher font: NOT FOUND (launcher falls back to the bitmap font)'
  }

  Write-Output ("windows binary: {0} ({1:N1} MB)" -f $bin, ((Get-Item $bin).Length/1MB))
}

# ---------------------------------------------------------------- dist
#
# One folder that plays. The build leaves goldeneye.exe beside four DLLs, a font directory,
# a 30 MB static library and a pile of object files, and "which of these do I copy?" is not a
# question anyone should have to answer. This stages exactly the runtime set and nothing else.
#
# The config is generated by the binary's own --write-config rather than kept as a checked-in
# sample. A sample file drifts the moment a key is added or a default changes, and a stale
# config is worse than none because it looks authoritative.
function Build-Dist {
  if (-not (Test-Path $bin)) { throw "no binary at $bin -- run -Target all first" }

  $dist = Join-Path $build 'dist'
  if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $dist | Out-Null

  Copy-Item $bin (Join-Path $dist 'goldeneye.exe') -Force

  # Every DLL the binary actually needs. A missing one exits with 0xC0000135 before main()
  # runs: no message, no log, nothing on stdout. That failure is silent enough that it must
  # be an error here rather than a warning at play time.
  $missing = @()
  foreach ($d in @('SDL2.dll','libwinpthread-1.dll','libgcc_s_seh-1.dll','libstdc++-6.dll')) {
    $src = Join-Path $build $d
    if (Test-Path $src) { Copy-Item $src (Join-Path $dist $d) -Force } else { $missing += $d }
  }
  if ($missing.Count -gt 0) {
    throw ("dist would be unrunnable: missing " + ($missing -join ', ') +
           " -- these are copied by -Target app, so run that first")
  }

  # The launcher font, with its licence. OFL 1.1 requires the copyright notice to travel with
  # the font, so OFL.txt is not optional decoration in a redistributable folder.
  $fsrc = Join-Path $build 'assets\fonts'
  if (Test-Path $fsrc) {
    $fdst = Join-Path $dist 'assets\fonts'
    New-Item -ItemType Directory -Force -Path $fdst | Out-Null
    Copy-Item (Join-Path $fsrc '*') $fdst -Force
  } else {
    Write-Output 'dist: WARNING no assets/fonts -- the launcher will fall back to its bitmap font'
  }

  foreach ($f in @('LICENSE','NOTICE')) {
    $src = Join-Path $root $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $dist $f) -Force }
  }

  # An example mod, so the launcher's Mods page has something in it on a fresh copy and the
  # folder layout is self-explanatory.
  $msrc = Join-Path $root 'mods'
  if (Test-Path $msrc) { Copy-Item $msrc (Join-Path $dist 'mods') -Recurse -Force }

  # Generated, not authored. Runs the binary that was just staged, so the defaults in the file
  # are that binary's defaults by construction.
  $cfg = Join-Path $dist 'goldeneye.cfg'
  & (Join-Path $dist 'goldeneye.exe') "--write-config=$cfg" *> $null
  if (-not (Test-Path $cfg)) {
    Write-Output 'dist: WARNING --write-config produced nothing; shipping without a default config'
  }

  $readme = @(
    'GoldenEye 007 - native port'
    '==========================='
    ''
    'Run goldeneye.exe.'
    ''
    'Everything in this folder is needed. The four DLLs sit beside the executable on'
    'purpose: if one is missing Windows exits the process before main() runs, with no'
    'message and nothing on stdout.'
    ''
    '  goldeneye.exe        the game'
    '  goldeneye.cfg        settings, commented -- edit it or use the launcher'
    '  assets/fonts/        the launcher typeface (Roboto Condensed, SIL OFL 1.1)'
    '  mods/                Lua mods; one folder each, containing mod.lua'
    '  *.dll                runtime libraries'
    ''
    'Launcher'
    '--------'
    'goldeneye.exe --launcher   picks the mission, rules, cheats, video and mods, then'
    'starts the game with them. It restarts the process to apply the settings, which is'
    'why the window closes and reopens.'
    ''
    'Config'
    '------'
    'goldeneye.cfg beside the executable is read at startup. Every key is commented in'
    'the file itself. Regenerate a fresh one with:'
    ''
    '  goldeneye.exe --write-config=goldeneye.cfg'
    ''
    'Mods'
    '----'
    'Drop a folder containing a mod.lua into mods/. It loads at startup. Turn individual'
    'mods off from the launcher, or with mods_off = name1,name2 in the config.'
    ''
    'Licensing'
    '---------'
    'See LICENSE and NOTICE. The bundled font is Roboto Condensed under the SIL Open'
    'Font License 1.1; its licence text is assets/fonts/OFL.txt.'
  )
  Set-Content -Path (Join-Path $dist 'README.txt') -Value $readme -Encoding ASCII

  $bytes = (Get-ChildItem $dist -Recurse -File | Measure-Object -Property Length -Sum).Sum
  $count = (Get-ChildItem $dist -Recurse -File).Count
  Write-Output ("windows dist: {0} ({1} files, {2:N1} MB)" -f $dist, $count, ($bytes/1MB))
}

switch ($Target) {
  'clean' { if (Test-Path $build) { Remove-Item $build -Recurse -Force }; Write-Output 'cleaned' }
  'lib'   { Build-Lib }
  'port'  { Build-Port }
  'app'   { Build-App }
  'dist'  { Build-Dist }
  'all'   { Build-Lib; Build-App }
}
