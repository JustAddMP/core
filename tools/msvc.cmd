@echo off
rem Shared by core\build.cmd and the game repos' build.cmd: puts the x64 MSVC toolchain on PATH (Visual Studio found
rem with vswhere) if cl.exe is not there yet, and sets the flags every JustAddMP binary is built with.
rem Use with "call"; it changes the caller's environment (no setlocal here). Exit code 1 if no toolchain was found.
rem Sets: MC_CXXFLAGS, MC_CFLAGS, MC_LDFLAGS (no include paths: each build adds its own), MC_SYSLIBS (mod dll + tests).
where cl.exe >nul 2>nul && goto :flags
set "MC_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%MC_VSWHERE%" goto :novswhere
set "MC_VCVARS="
for /f "usebackq delims=" %%i in (`"%MC_VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "MC_VCVARS=%%i"
if not defined MC_VCVARS goto :novs
echo [build] using %MC_VCVARS%
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%MC_VCVARS%" >nul
if errorlevel 1 goto :vcvarsfailed
where cl.exe >nul 2>nul || goto :nocl

:flags
set "MC_DEFS=/DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
set "MC_CXXFLAGS=/nologo /c /std:c++20 /EHsc /MT /O2 /Zi /FS /W4 /permissive- /utf-8 /Zc:__cplusplus %MC_DEFS%"
set "MC_CFLAGS=/nologo /c /MT /O2 /Zi /FS /W3 /utf-8 %MC_DEFS%"
set "MC_LDFLAGS=/nologo /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO /MACHINE:X64"
set "MC_SYSLIBS=kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib ws2_32.lib d3d11.lib dxgi.lib dbghelp.lib"
exit /b 0

:novswhere
echo [build] ERROR: vswhere.exe not found (install Visual Studio 2022 or newer with the x64 C++ toolset)
exit /b 1
:novs
echo [build] ERROR: no Visual Studio with the x64 C++ toolset found
exit /b 1
:vcvarsfailed
echo [build] ERROR: vcvars64.bat failed
exit /b 1
:nocl
echo [build] ERROR: cl.exe still not on PATH
exit /b 1
