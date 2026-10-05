@echo off
rem ---------------------------------------------------------------------------
rem QuickScriptTool one-shot release helper.
rem
rem ASCII only on purpose: .cmd files are read with the OEM/ANSI code page
rem (GB2312 on this machine), and non-ASCII bytes get mis-parsed - see AGENTS.md.
rem
rem Usage:
rem   release.cmd 1.3.4                 release 1.3.4 (portable zip + installer)
rem   release.cmd 1.3.4 -Mode zip        portable zip only (skips the slow ISCC step)
rem   release.cmd 1.3.4 -Mode setup      installer only (no portable zip)
rem   release.cmd 1.3.4 -Mode all        both (this is the default)
rem   release.cmd 1.3.4 -DryRun          only print the plan, touch nothing
rem   release.cmd 1.3.4 -SkipBuild       reuse build\Release
rem   release.cmd                       prompt for the version
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
if "%VER%"=="" (
    rem Show the mode rules right before prompting. ASCII only: .cmd is read with
    rem the OEM/ANSI code page (GB2312 here) and non-ASCII bytes get mis-parsed,
    rem possibly eating quotes/parens - see AGENTS.md.
    echo.
    echo   Product modes ^(pass as the 2nd argument; default is all^):
    echo     -Mode all      portable zip + installer        ^(default^)
    echo     -Mode zip      portable zip only                ^(skips the slow ISCC step^)
    echo     -Mode setup    installer only                   ^(no portable zip^)
    echo.
    echo   Other switches:
    echo     -DryRun        print the plan only, touch nothing
    echo     -SkipBuild     reuse build\Release ^(code unchanged^)
    echo.
    set /p "VER=Version to release (e.g. 1.3.4)  [switches OK here too]: "
)

rem Allow switches to be typed at the prompt, e.g. "1.3.3 -Mode setup".
rem set /p stores the WHOLE line, so split off the first token as the version and
rem treat the rest as extra switches. Without this the whole line reaches the
rem script as one -Version value and fails the format check.
set "EXTRA="
for /f "tokens=1,*" %%a in ("%VER%") do (
    set "VER=%%a"
    set "EXTRA=%%b"
)

if "%VER%"=="" (
    echo [release] No version given. Aborted.
    if defined PAUSEFLAG pause
    exit /b 1
)

echo [release] Version : %VER%
echo [release] Repo    : %~dp0
echo.

rem %EXTRA% carries switches typed at the prompt, %2..%9 those given on the command
rem line. Either way they land after -Version.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package_with_version.ps1" -Version "%VER%" %EXTRA% %2 %3 %4 %5 %6 %7 %8 %9
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo [release] Done.
) else (
    echo [release] FAILED - exit code %RC%
)

if defined PAUSEFLAG pause
exit /b %RC%
