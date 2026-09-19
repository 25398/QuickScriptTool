@echo off
setlocal EnableExtensions
REM Build FakeFocus32.dll with the x86 MSVC toolset (called from CMake x64 builds).
REM Usage: build_fakefocus32.cmd <source_root> <output_dir>
REM NOTE: keep this file ASCII-only. A .cmd with UTF-8 Chinese comments is read as
REM GBK on a zh-CN host and can corrupt quoting/parsing.

set "SRCROOT=%~1"
set "OUTDIR=%~2"
if "%SRCROOT%"=="" exit /b 1
if "%OUTDIR%"=="" exit /b 1

REM Do NOT test "if defined ProgramFiles(x86)" directly: the parentheses break the
REM if parser, so BuildTools installed under "Program Files (x86)" was never found
REM and the 32-bit DLL was silently skipped. Copy to plain vars first.
set "PF86=%ProgramFiles(x86)%"
if not defined PF86 set "PF86=C:\Program Files (x86)"
set "PF64=%ProgramFiles%"
if not defined PF64 set "PF64=C:\Program Files"

set "VCVARS="

REM 1) vswhere: covers any edition/path, including BuildTools.
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%PF64%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if exist "%%I\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvarsall.bat"
  )
)

REM 2) Well-known fallbacks (64-bit Program Files, then 32-bit BuildTools).
if not defined VCVARS if exist "%PF64%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%PF64%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "%PF64%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%PF64%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "%PF64%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%PF64%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "%PF86%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%PF86%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "%PF86%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%PF86%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not defined VCVARS (
  echo [FakeFocus32] vcvarsall.bat not found - skip 32-bit DLL
  exit /b 0
)

:found_vcvars
REM x86 native OR x64-host cross x86
call "%VCVARS%" x86 >nul 2>&1
if errorlevel 1 (
  call "%VCVARS%" amd64_x86 >nul 2>&1
)
if errorlevel 1 (
  echo [FakeFocus32] vcvarsall x86/amd64_x86 failed - skip 32-bit DLL
  exit /b 0
)

where /Q cl >nul 2>&1
if errorlevel 1 (
  echo [FakeFocus32] cl.exe not on PATH after vcvars - skip 32-bit DLL
  exit /b 0
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set "SRC1=%SRCROOT%\src\window_mode\fake_focus\fake_focus_dll.cpp"
set "SRC2=%SRCROOT%\src\window_mode\fake_focus\fake_focus_hook.cpp"
set "INC=%SRCROOT%\src\window_mode\fake_focus"

set "DEF=%INC%\fake_focus.def"

cl /nologo /LD /O2 /W3 /EHsc /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DFAKEFOCUS_EXPORTS ^
  /I"%INC%" /I"%SRCROOT%\src" ^
  "%SRC1%" "%SRC2%" ^
  /Fe"%OUTDIR%\FakeFocus32.dll" /Fo"%OUTDIR%\\" /Fd"%OUTDIR%\FakeFocus32.pdb" ^
  /link /DLL /DEF:"%DEF%" user32.lib dwmapi.lib

if errorlevel 1 (
  echo [FakeFocus32] build failed
  exit /b 1
)
echo [FakeFocus32] OK: %OUTDIR%\FakeFocus32.dll
exit /b 0
