<#
  Windows dependency setup.

  One script for everything the Windows build needs, because the alternative is a README
  section that says "download six things and put them in the right places" and is wrong
  within a month. Everything lands in C:\mingw64 (the toolchain) or %USERPROFILE%\.n64tvos
  (the optional libraries), matching where build_windows.ps1 looks.

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
  [switch]$SkipToolchain
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
  Expand-Archive -Path "$tmp\winlibs.zip" -DestinationPath 'C:\' -Force
  Remove-Item "$tmp\winlibs.zip" -ErrorAction SilentlyContinue
}
if (-not (Test-Path "$Mingw\bin\gcc.exe")) { throw "no gcc at $Mingw\bin -- toolchain step failed" }
$gcc = "$Mingw\bin\gcc.exe"; $gxx = "$Mingw\bin\g++.exe"; $ar = "$Mingw\bin\ar.exe"

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
if (-not (Test-Path "$luaPrefix\lib\liblua.a")) {
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
  $objs = @()
  foreach ($f in $src) {
    $o = Join-Path $tmp ("imgui_" + [IO.Path]::GetFileNameWithoutExtension($f) + ".o")
    & $gxx -std=c++17 -O2 -w -fno-exceptions -fno-rtti -I"$i" -I"$i\backends" `
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

# ---------------------------------------------------------------- report
Write-Output ""
Write-Output "gcc        : $(if (Test-Path "$Mingw\bin\gcc.exe") { (& $gcc -dumpversion) } else { 'MISSING' })"
Write-Output "SDL2       : $(if (Test-Path "$Mingw\include\SDL2\SDL.h") { 'ok' } else { 'MISSING' })"
Write-Output "GLEW       : $(if (Test-Path "$Mingw\lib\libglew32.a") { 'ok' } else { 'MISSING' })"
Write-Output "Lua        : $(if (Test-Path "$luaPrefix\lib\liblua.a") { 'ok (mods enabled)' } else { 'absent (mods disabled)' })"
Write-Output "Dear ImGui : $(if (Test-Path "$imguiPrefix\lib\libimgui.a") { 'ok (overlay + launcher enabled)' } else { 'absent (overlay + launcher disabled)' })"
Write-Output ""
Write-Output "next: powershell -NoProfile -ExecutionPolicy Bypass -File getv\build_windows.ps1 -Target all"
