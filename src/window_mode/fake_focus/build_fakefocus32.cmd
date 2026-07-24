@echo off
setlocal EnableExtensions
REM Build FakeFocus32.dll with the x86 MSVC toolset (called from CMake x64 builds).
REM Usage: build_fakefocus32.cmd <source_root> <output_dir>

set "SRCROOT=%~1"
set "OUTDIR=%~2"
if "%SRCROOT%"=="" exit /b 1
if "%OUTDIR%"=="" exit /b 1

set "VCVARS="

REM Prefer vswhere (quoted paths) - avoids "Program Files" splitting bugs.
where /Q vswhere >nul 2>&1
if not errorlevel 1 (
  for /f "usebackq delims=" %%I in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if exist "%%I\VC\Auxiliary\Build\vcvarsall.bat" (
      set "VCVARS=%%I\VC\Auxiliary\Build\vcvarsall.bat"
      goto :found_vcvars
    )
  )
)

if defined ProgramFiles (
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    goto :found_vcvars
  )
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    goto :found_vcvars
  )
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    goto :found_vcvars
  )
)
if defined ProgramFiles^(x86^) (
  if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    goto :found_vcvars
  )
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" (
  set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
  goto :found_vcvars
)

echo [FakeFocus32] vcvarsall.bat not found - skip 32-bit DLL
exit /b 0

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

cl /nologo /LD /O2 /W3 /EHsc /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DFAKEFOCUS_EXPORTS ^
  /I"%INC%" /I"%SRCROOT%\src" ^
  "%SRC1%" "%SRC2%" ^
  /Fe"%OUTDIR%\FakeFocus32.dll" /Fo"%OUTDIR%\\" /Fd"%OUTDIR%\FakeFocus32.pdb" ^
  /link /DLL user32.lib dwmapi.lib

if errorlevel 1 (
  echo [FakeFocus32] build failed
  exit /b 1
)
echo [FakeFocus32] OK: %OUTDIR%\FakeFocus32.dll
exit /b 0
