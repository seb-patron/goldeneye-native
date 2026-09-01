<#
  Windows dependency setup.

  One script for everything the Windows build needs, because the alternative is a README
  section that says "download six things and put them in the right places" and is wrong
  within a month. The toolchain lands at -Mingw (C:\mingw64 for a manual developer run; a
  user-writable LocalAppData path when launched by the setup app) and optional libraries land
  under %USERPROFILE%\.n64tvos.

  Deliberately does NOT use MSYS2. Its fork emulation is unreliable -- see the header of
  getv/build_windows.ps1 for the measured failure -- and none of this needs a POSIX layer:
  mingw-w64 from WinLibs, SDL2's own mingw package and three small source builds.

      powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_deps_windows.ps1

  Each step is skipped if its result is already present, so re-running is cheap and is the
  right way to add a dependency later.
#>
[CmdletBinding()]
param(
  [string]$Mingw   = 'C:\mingw64',
  [string]$Prefix  = (Join-Path $env:USERPROFILE '.n64tvos'),
  [switch]$SkipToolchain,
  # setup_wizard.exe needs SDL2, GLEW and Dear ImGui, but not the two libraries linked only
  # into goldeneye.exe. CI packaging uses this to avoid downloading and compiling Lua + Tracy
  # before it can produce the small, ROM-free bootstrapper.
  [switch]$WizardOnly
)

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'
$tmp = $env:TEMP

function Step($msg) { Write-Output "==> $msg" }

# ---------------------------------------------------------------- 1. toolchain
# WinLibs is a plain zip of mingw-w64: no installer, no registry, no runtime. Pinned by URL
# rather than tracking "latest", because a compiler version change is exactly the kind of
# thing that should be a deliberate edit -- GCC 15 moving the default to C23 broke this tree
# once already.
$gccUrl = 'https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-14.0.0-ucrt-r1/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r1.zip'
if (-not $SkipToolchain -and -not (Test-Path "$Mingw\bin\gcc.exe")) {
  Step "mingw-w64 (gcc 16.2, ~260 MB)"
  Invoke-WebRequest -Uri $gccUrl -OutFile "$tmp\winlibs.zip" -UseBasicParsing
  $toolchainStage = Join-Path $tmp 'goldeneye-native-winlibs'
  Remove-Item $toolchainStage -Recurse -Force -ErrorAction SilentlyContinue
  Expand-Archive -Path "$tmp\winlibs.zip" -DestinationPath $toolchainStage -Force
  $toolchainSource = Join-Path $toolchainStage 'mingw64'
  if (-not (Test-Path (Join-Path $toolchainSource 'bin\gcc.exe'))) {
    throw "WinLibs archive did not contain mingw64\bin\gcc.exe"
  }
  New-Item -ItemType Directory -Force -Path $Mingw | Out-Null
  Copy-Item (Join-Path $toolchainSource '*') $Mingw -Recurse -Force
  Remove-Item "$tmp\winlibs.zip",$toolchainStage -Recurse -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path "$Mingw\bin\gcc.exe")) { throw "no gcc at $Mingw\bin -- toolchain step failed" }
$gcc = "$Mingw\bin\gcc.exe"; $gxx = "$Mingw\bin\g++.exe"; $ar = "$Mingw\bin\ar.exe"

# WinLibs ships GNU make as mingw32-make.exe and nothing called "make". The decomp's
# scripts/extract_baserom.u.sh builds its ROM extractor with a bare `make -C tools/extractor`,
# and when that is not on PATH the failure is silent rather than fatal: the extraction below it
# is guarded on the extractor binary existing, so the whole pass prints "skip" for every asset
# and returns 0. The install then runs for another twenty minutes and dies at the link with 40
# undefined C<name>Z symbols. Measured on a fresh Windows install: 232 asset objects built
# instead of 746, 514 short.
#
# A copy rather than a hardlink or a .cmd shim: those need either privilege or a shell that
# expands them, and this has to work from git-bash, which runs .exe files directly.
$mingwMake = Join-Path $Mingw 'bin\mingw32-make.exe'
$plainMake = Join-Path $Mingw 'bin\make.exe'
if ((Test-Path $mingwMake) -and (-not (Test-Path $plainMake))) {
  Copy-Item $mingwMake $plainMake -Force
  Write-Output "  make.exe: copied from mingw32-make.exe, which the decomp's extractor calls by that name"
}

# ---------------------------------------------------------------- 2. SDL2
# The official mingw development package. Its x86_64-w64-mingw32 subtree has exactly the
# include/lib/bin layout the toolchain expects, so it merges straight in.
if (-not (Test-Path "$Mingw\include\SDL2\SDL.h")) {
  Step "SDL2 2.30.9"
  $u = 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.zip'
  Invoke-WebRequest -Uri $u -OutFile "$tmp\sdl2.zip" -UseBasicParsing
  Expand-Archive -Path "$tmp\sdl2.zip" -DestinationPath "$tmp\sdl2" -Force
  $s = Join-Path "$tmp\sdl2" 'SDL2-2.30.9\x86_64-w64-mingw32'
  Copy-Item "$s\include\*" "$Mingw\include\" -Recurse -Force
  Copy-Item "$s\lib\*"     "$Mingw\lib\"     -Recurse -Force
  Copy-Item "$s\bin\*"     "$Mingw\bin\"     -Recurse -Force
  Remove-Item "$tmp\sdl2.zip","$tmp\sdl2" -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- 3. GLEW
# Windows-only, and not optional: opengl32.dll exports GL 1.1 and nothing later, so without
# a loader every modern entry point is a null pointer. gfx_opengl.c already expects GLEW --
# it sets FOR_WINDOWS on __MINGW32__ and includes <GL/glew.h>. macOS gets its entry points
# from the OpenGL framework and Linux from libGL, which is why neither needs this.
if (-not (Test-Path "$Mingw\lib\libglew32.a")) {
  Step "GLEW 2.2.0 (built from source)"
  $u = 'https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.zip'
  Invoke-WebRequest -Uri $u -OutFile "$tmp\glew.zip" -UseBasicParsing
  Expand-Archive -Path "$tmp\glew.zip" -DestinationPath "$tmp\glew" -Force
  $g = "$tmp\glew\glew-2.2.0"
  & $gcc -DGLEW_STATIC -DGLEW_NO_GLU -I"$g\include" -O2 -w -c "$g\src\glew.c" -o "$tmp\glew.o"
  if (Test-Path "$tmp\glew.o") {
    & $ar rcs "$Mingw\lib\libglew32.a" "$tmp\glew.o"
    New-Item -ItemType Directory -Force -Path "$Mingw\include\GL" | Out-Null
    Copy-Item "$g\include\GL\*" "$Mingw\include\GL\" -Force
  }
  Remove-Item "$tmp\glew.zip","$tmp\glew","$tmp\glew.o" -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- 4. Lua (optional)
# Mod scripting. Optional at every level: without it the build omits -DGE_WITH_LUA and the
# hooks compile to empty functions. The checksum is checked because this is compiled into
# the game binary.
$luaPrefix = Join-Path $Prefix 'lua-win'
if ($WizardOnly) {
  Step "Lua 5.4.7 skipped (not linked into the setup wizard)"
} elseif (-not (Test-Path "$luaPrefix\lib\liblua.a")) {
  Step "Lua 5.4.7 (mod scripting)"
  $sha = '9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30'
  Invoke-WebRequest -Uri 'https://www.lua.org/ftp/lua-5.4.7.tar.gz' -OutFile "$tmp\lua.tgz" -UseBasicParsing
  $got = (Get-FileHash "$tmp\lua.tgz" -Algorithm SHA256).Hash.ToLower()
  if ($got -ne $sha) { throw "lua checksum mismatch`n  expected $sha`n  got      $got" }
  New-Item -ItemType Directory -Force -Path "$tmp\luasrc" | Out-Null
  tar.exe -xzf "$tmp\lua.tgz" -C "$tmp\luasrc"
  $ls = "$tmp\luasrc\lua-5.4.7\src"
  New-Item -ItemType Directory -Force -Path "$luaPrefix\lib","$luaPrefix\include" | Out-Null
  $objs = @()
  # lua.c and luac.c are the standalone interpreter and compiler; both define main().
  Get-ChildItem $ls -Filter *.c | Where-Object { $_.Name -notin @('lua.c','luac.c') } | ForEach-Object {
    $o = Join-Path $tmp ("lua_" + $_.BaseName + ".o")
    & $gcc -O2 -w -c $_.FullName -o $o
    if (Test-Path $o) { $objs += $o }
  }
  $rsp = "$tmp\lua.rsp"
  # Forward slashes: GNU ar treats backslash as an escape inside a response file and drops
  # the member without a word of complaint.
  Set-Content -Path $rsp -Value (($objs | ForEach-Object { $_ -replace '\\','/' }) -join "`n")
  & $ar rcs "$luaPrefix\lib\liblua.a" "@$rsp"
  Copy-Item "$ls\lua.h","$ls\luaconf.h","$ls\lualib.h","$ls\lauxlib.h" "$luaPrefix\include\" -Force
  Remove-Item "$tmp\lua.tgz","$tmp\luasrc",$rsp -Recurse -Force -ErrorAction SilentlyContinue
  $objs | ForEach-Object { Remove-Item $_ -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------- 5. Dear ImGui (optional)
# The dev overlay and the launcher window. imgui_impl_opengl2 rather than opengl3: the GL3
# backend calls glGenVertexArrays unconditionally on desktop GL, and the context this build
# takes is not guaranteed to have it.
$imguiPrefix = Join-Path $Prefix 'imgui-win'
if (-not (Test-Path "$imguiPrefix\lib\libimgui.a")) {
  Step "Dear ImGui v1.91.9b (dev overlay + launcher)"
  $u = 'https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip'
  Invoke-WebRequest -Uri $u -OutFile "$tmp\imgui.zip" -UseBasicParsing
  Expand-Archive -Path "$tmp\imgui.zip" -DestinationPath "$tmp\imguisrc" -Force
  $i = "$tmp\imguisrc\imgui-1.91.9b"
  New-Item -ItemType Directory -Force -Path "$imguiPrefix\lib","$imguiPrefix\include" | Out-Null
  $src = @(
    "$i\imgui.cpp","$i\imgui_draw.cpp","$i\imgui_tables.cpp","$i\imgui_widgets.cpp",
    "$i\imgui_demo.cpp","$i\backends\imgui_impl_sdl2.cpp","$i\backends\imgui_impl_opengl2.cpp"
  )
  # ImGui's IM_ASSERT expands __FILE__, so without this every assert string in libimgui.a
  # carries the absolute path it was compiled from -- and $tmp is $env:TEMP, which on a normal
  # Windows account is C:\Users\<name>\AppData\Local\Temp. That put the builder's account name
  # into setup_wizard.exe, a candidate release artifact other people may download. Measured: 12
  # such strings in the first wizard build. Mapping the prefix rewrites
  # __FILE__ at compile time, so the strings read "imgui/imgui.cpp" and identify nobody.
  $imguiMap = "$tmp\imguisrc"
  $objs = @()
  foreach ($f in $src) {
    $o = Join-Path $tmp ("imgui_" + [IO.Path]::GetFileNameWithoutExtension($f) + ".o")
    & $gxx -std=c++17 -O2 -w -fno-exceptions -fno-rtti -I"$i" -I"$i\backends" `
           "-ffile-prefix-map=$imguiMap=imgui" "-fmacro-prefix-map=$imguiMap=imgui" `
           -I"$Mingw\include\SDL2" -c $f -o $o
    if (Test-Path $o) { $objs += $o }
  }
  $rsp = "$tmp\imgui.rsp"
  Set-Content -Path $rsp -Value (($objs | ForEach-Object { $_ -replace '\\','/' }) -join "`n")
  & $ar rcs "$imguiPrefix\lib\libimgui.a" "@$rsp"
  Copy-Item "$i\imgui.h","$i\imconfig.h","$i\imgui_internal.h","$i\imstb_textedit.h", `
            "$i\imstb_rectpack.h","$i\imstb_truetype.h" "$imguiPrefix\include\" -Force
  Copy-Item "$i\backends\imgui_impl_sdl2.h","$i\backends\imgui_impl_opengl2.h" "$imguiPrefix\include\" -Force
  Remove-Item "$tmp\imgui.zip","$tmp\imguisrc",$rsp -Recurse -Force -ErrorAction SilentlyContinue
  $objs | ForEach-Object { Remove-Item $_ -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------- 6. Tracy (optional)
# Profiling. REUSE_AUDIT.md: "Frame cost is currently unattributed: there is no measurement
# separating game tick, render, audio and AI." getv/port/include/ge_tracy.h is what every call
# site includes instead of <tracy/TracyC.h> directly, so a checkout that has not run this step
# still compiles -- its macros are no-ops without GE_WITH_TRACY, same shape as GE_WITH_IMGUI.
#
# TracyClient.cpp is a literal unity build: it #includes the client, common and (optionally)
# libbacktrace sources itself, so compiling this one file is the entire client library. Every
# TracyC.h macro is ALSO a no-op unless TRACY_ENABLE is defined for the translation unit that
# includes it -- not just for TracyClient.cpp -- which is why build_windows.ps1 must add
# -DTRACY_ENABLE to the game/port flags themselves, not only to this compile.
$tracyPrefix = Join-Path $Prefix 'tracy-win'
if ($WizardOnly) {
  Step "Tracy 0.14.1 skipped (not linked into the setup wizard)"
} elseif (-not (Test-Path "$tracyPrefix\lib\libtracy.a")) {
  Step "Tracy 0.14.1 (profiler client)"
  $sha = '908f3a2917fa86a247abfcf85dcf04bad1db6986a4d40f94b70512f3e9e98d5b'
  Invoke-WebRequest -Uri 'https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.zip' -OutFile "$tmp\tracy.zip" -UseBasicParsing
  $got = (Get-FileHash "$tmp\tracy.zip" -Algorithm SHA256).Hash.ToLower()
  if ($got -ne $sha) { throw "tracy checksum mismatch`n  expected $sha`n  got      $got" }
  Expand-Archive -Path "$tmp\tracy.zip" -DestinationPath "$tmp\tracysrc" -Force
  $t = "$tmp\tracysrc\tracy-0.14.1\public"
  New-Item -ItemType Directory -Force -Path "$tracyPrefix\lib","$tracyPrefix\include\tracy" | Out-Null
  & $gxx -std=c++17 -O2 -w -DTRACY_ENABLE -fno-exceptions -I"$t" -c "$t\TracyClient.cpp" -o "$tmp\tracyclient.o"
  if (Test-Path "$tmp\tracyclient.o") {
    & $ar rcs "$tracyPrefix\lib\libtracy.a" "$tmp\tracyclient.o"
  }
  # Only the C API and its two dependency headers -- this port's own code is C, and Tracy.hpp
  # (the C++ macro API) is not used anywhere here. Checked by reading TracyC.h's own #include
  # lines rather than assumed: it wants ../common/TracyApi.h and ../common/TracyFormat.h,
  # both self-contained (no further local includes of their own).
  Copy-Item "$t\tracy\TracyC.h" "$tracyPrefix\include\tracy\" -Force
  New-Item -ItemType Directory -Force -Path "$tracyPrefix\include\common" | Out-Null
  Copy-Item "$t\common\TracyApi.h","$t\common\TracyFormat.h" "$tracyPrefix\include\common\" -Force
  Remove-Item "$tmp\tracy.zip","$tmp\tracysrc","$tmp\tracyclient.o" -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- report
Write-Output ""
Write-Output "gcc        : $(if (Test-Path "$Mingw\bin\gcc.exe") { (& $gcc -dumpversion) } else { 'MISSING' })"
Write-Output "SDL2       : $(if (Test-Path "$Mingw\include\SDL2\SDL.h") { 'ok' } else { 'MISSING' })"
Write-Output "GLEW       : $(if (Test-Path "$Mingw\lib\libglew32.a") { 'ok' } else { 'MISSING' })"
Write-Output "Lua        : $(if ($WizardOnly) { 'skipped (wizard-only setup)' } elseif (Test-Path "$luaPrefix\lib\liblua.a") { 'ok (mods enabled)' } else { 'absent (mods disabled)' })"
Write-Output "Dear ImGui : $(if (Test-Path "$imguiPrefix\lib\libimgui.a") { 'ok (overlay + launcher enabled)' } else { 'absent (overlay + launcher disabled)' })"
Write-Output "Tracy      : $(if ($WizardOnly) { 'skipped (wizard-only setup)' } elseif (Test-Path "$tracyPrefix\lib\libtracy.a") { 'ok (profiling enabled)' } else { 'absent (profiling disabled)' })"
Write-Output ""
Write-Output "next: powershell -NoProfile -ExecutionPolicy Bypass -File getv\build_windows.ps1 -Target all"
