@echo off
rem ---------------------------------------------------------------------------
rem QuickScriptTool one-shot release helper.
rem
rem ASCII only on purpose: .cmd files are read with the OEM/ANSI code page
rem (GB2312 on this machine), and non-ASCII bytes get mis-parsed - see AGENTS.md.
rem
rem Usage:
rem   release.cmd 1.3.4              release version 1.3.4
rem   release.cmd 1.3.4 -DryRun      only print the plan, touch nothing
rem   release.cmd 1.3.4 -SkipBuild   reuse build\Release
rem   release.cmd                    prompt for the version
rem
rem It always cd's to its own folder first, so it works from any directory
rem (this is what breaks if you call tools\package_with_version.ps1 with a
rem relative path from somewhere else, e.g. C:\WINDOWS\system32).
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

rem Detect double-click (explorer launches "cmd /c ""...\release.cmd"""), so we
rem can keep the window open at the end only in that case.
set "PAUSEFLAG="
echo %cmdcmdline% | find /i "%~nx0" >nul 2>&1 && set "PAUSEFLAG=1"

set "VER=%~1"
if "%VER%"=="" set /p "VER=Version to release (e.g. 1.3.4): "
if "%VER%"=="" (
    echo [release] No version given. Aborted.
    if defined PAUSEFLAG pause
    exit /b 1
)

echo [release] Version : %VER%
echo [release] Repo    : %~dp0
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package_with_version.ps1" -Version "%VER%" %2 %3 %4 %5
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo [release] Done.
) else (
    echo [release] FAILED - exit code %RC%
)

if defined PAUSEFLAG pause
exit /b %RC%
