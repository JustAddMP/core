@echo off
rem JustAddMP core build. Usage: build.cmd [core build dir]   (runnable from any directory)
rem
rem Without an argument: the core libraries, the test binaries (made-up game, tests\support\) and every tests\*.cpp
rem   into out\ (exes, dlls, PDBs; objects and libraries in out\obj\).
rem With a directory (a game repo's build.cmd passes e.g. <game>\out\obj\core): only the core libraries, into it.
rem
rem A game links (see README.md):
rem   mod dll:  <dir>\entry\dllmain.obj + <dir>\mc_core.lib + its game layer (GetGameModule, GetGameInfo)
rem   launcher: <dir>\launcher\*.obj + its GetGameInfo object   (/SUBSYSTEM:WINDOWS)
rem   proxy:    <dir>\proxy\*.obj + its GetGameInfo object       (/DLL /DEF:src\proxy\version.def)
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "OUT=%ROOT%\out"
set "CB=%OUT%\obj"
set "LIBONLY="
if not "%~1"=="" (
  set "CB=%~f1"
  set "LIBONLY=1"
)
set "GEN=%CB%\gen"

call "%ROOT%\tools\msvc.cmd" || goto :fail

if not exist "%CB%" mkdir "%CB%" || goto :fail
for %%d in (gen minhook core net render swf dll entry launcher proxy support tests) do if not exist "%CB%\%%d" mkdir "%CB%\%%d"

set "INCS=/I"%ROOT%\src" /I"%ROOT%\include" /I"%GEN%" /I"%ROOT%\third_party\minhook\include""
set "CXXFLAGS=%MC_CXXFLAGS% %INCS%"

rem ---- minhook (C) -----------------------------------------------------------
echo [core] minhook
del /q "%CB%\minhook\*.obj" 2>nul
cl %MC_CFLAGS% /Fd"%CB%\minhook\vc.pdb" /Fo"%CB%\minhook\\" ^
  "%ROOT%\third_party\minhook\src\buffer.c" "%ROOT%\third_party\minhook\src\hook.c" ^
  "%ROOT%\third_party\minhook\src\trampoline.c" "%ROOT%\third_party\minhook\src\hde\hde64.c" >"%CB%\minhook\cl.log"
if errorlevel 1 ( type "%CB%\minhook\cl.log" & echo [core] ERROR: minhook failed & goto :fail )

rem ---- shaders: src\render\*.hlsl -> gen\<name>_vs.h / _ps.h (fxc from the Windows SDK) ------------------------
rem Bytecode arrays k<Name>VS / k<Name>PS (e.g. overlay.hlsl -> kOverlayVS in overlay_vs.h). Precompiled so the
rem dll needs no d3dcompiler_47.dll at runtime.
where fxc.exe >nul 2>nul || ( echo [core] ERROR: fxc.exe not on PATH ^(Windows SDK^) & goto :fail )
for %%f in ("%ROOT%\src\render\*.hlsl") do (
  echo [core] shader %%~nxf
  set "SN=%%~nf"
  set "SC=!SN:~0,1!"
  for %%u in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do if /i "!SC!"=="%%u" set "SC=%%u"
  set "SC=!SC!!SN:~1!"
  fxc /nologo /T vs_4_0 /E VSMain /O3 /Vn k!SC!VS /Fh "%GEN%\%%~nf_vs.h" "%%f" >"%GEN%\%%~nf_vs.log" 2>&1
  if errorlevel 1 ( type "%GEN%\%%~nf_vs.log" & echo [core] ERROR: shader %%~nxf VS failed & goto :fail )
  fxc /nologo /T ps_4_0 /E PSMain /O3 /Vn k!SC!PS /Fh "%GEN%\%%~nf_ps.h" "%%f" >"%GEN%\%%~nf_ps.log" 2>&1
  if errorlevel 1 ( type "%GEN%\%%~nf_ps.log" & echo [core] ERROR: shader %%~nxf PS failed & goto :fail )
)

rem ---- source groups (every .cpp in each dir) ----------------------------------
for %%d in (core net render swf dll launcher proxy) do (
  del /q "%CB%\%%d\*.obj" 2>nul
  echo [core] src\%%d
  cl %CXXFLAGS% /Fd"%CB%\%%d\vc.pdb" /Fo"%CB%\%%d\\" "%ROOT%\src\%%d\*.cpp"
  if errorlevel 1 ( echo [core] ERROR: compiling src\%%d failed & goto :fail )
)
rem DllMain goes into the mod dll as an object (from a library the CRT's default DllMain could win).
del /q "%CB%\entry\*.obj" 2>nul
move /y "%CB%\dll\dllmain.obj" "%CB%\entry\dllmain.obj" >nul || goto :fail
ml64 /nologo /c /W3 /Zi /Fo"%CB%\proxy\version_stubs.obj" "%ROOT%\src\proxy\version_stubs.asm" >"%CB%\proxy\ml.log"
if errorlevel 1 ( type "%CB%\proxy\ml.log" & echo [core] ERROR: assembling the proxy stubs failed & goto :fail )

echo [core] lib mc_core.lib
lib /nologo /OUT:"%CB%\mc_core.lib" "%CB%\minhook\*.obj" "%CB%\core\*.obj" "%CB%\net\*.obj" "%CB%\render\*.obj" ^
  "%CB%\swf\*.obj" "%CB%\dll\*.obj"
if errorlevel 1 ( echo [core] ERROR: lib mc_core.lib failed & goto :fail )

if defined LIBONLY goto :done

rem ---- test binaries: a made-up game (tests\support\test_game_info.cpp) and a game module that does nothing ----
echo [core] tests\support
del /q "%CB%\support\*.obj" 2>nul
cl %CXXFLAGS% /Fd"%CB%\support\vc.pdb" /Fo"%CB%\support\\" "%ROOT%\tests\support\*.cpp"
if errorlevel 1 ( echo [core] ERROR: compiling tests\support failed & goto :fail )
set "TESTINFO="%CB%\support\test_game_info.obj""

echo [core] link mc_test.dll
link %MC_LDFLAGS% /DLL /OUT:"%OUT%\mc_test.dll" /PDB:"%OUT%\mc_test.pdb" /IMPLIB:"%CB%\mc_test.lib" ^
  "%CB%\entry\dllmain.obj" "%CB%\support\test_module.obj" %TESTINFO% "%CB%\mc_core.lib" %MC_SYSLIBS%
if errorlevel 1 ( echo [core] ERROR: linking mc_test.dll failed & goto :fail )

echo [core] link mc_test_launcher.exe
link %MC_LDFLAGS% /SUBSYSTEM:WINDOWS /OUT:"%OUT%\mc_test_launcher.exe" /PDB:"%OUT%\mc_test_launcher.pdb" ^
  "%CB%\launcher\*.obj" %TESTINFO% kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 ( echo [core] ERROR: linking mc_test_launcher.exe failed & goto :fail )

echo [core] link mc_test_proxy.dll
link %MC_LDFLAGS% /DLL /DEF:"%ROOT%\src\proxy\version.def" /OUT:"%OUT%\mc_test_proxy.dll" /PDB:"%OUT%\mc_test_proxy.pdb" ^
  /IMPLIB:"%CB%\proxy\mc_test_proxy.lib" "%CB%\proxy\*.obj" %TESTINFO% kernel32.lib
if errorlevel 1 ( echo [core] ERROR: linking mc_test_proxy.dll failed & goto :fail )

rem ---- tests: one exe per tests\*.cpp, linked with mc_core.lib and the test descriptor ---------------------------
del /q "%CB%\tests\*.obj" 2>nul
for %%f in ("%ROOT%\tests\*.cpp") do (
  echo [core] tests\%%~nxf
  cl %CXXFLAGS% /I"%ROOT%\tests" /Fd"%CB%\tests\vc.pdb" /Fo"%CB%\tests\%%~nf.obj" "%%f"
  if errorlevel 1 ( echo [core] ERROR: compiling tests\%%~nxf failed & goto :fail )
  link %MC_LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%OUT%\%%~nf.exe" /PDB:"%OUT%\%%~nf.pdb" "%CB%\tests\%%~nf.obj" ^
    %TESTINFO% "%CB%\mc_core.lib" %MC_SYSLIBS%
  if errorlevel 1 ( echo [core] ERROR: linking %%~nf.exe failed & goto :fail )
)

:done
echo [core] OK -^> %CB%
endlocal
exit /b 0

:fail
echo [core] FAILED
endlocal
exit /b 1
