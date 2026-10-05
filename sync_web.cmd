@echo off
rem ---------------------------------------------------------------------------
rem QuickScriptTool one-shot web sync helper.
rem
rem Syncs the product front-end (ui\) and the online-export player template into
rem website\, then runs the syntax guard. Use it whenever ui\ changed and you
rem want the public site to match - no release needed.
rem
rem ASCII only on purpose: .cmd files are read with the OEM/ANSI code page
rem (GB2312 on this machine), and non-ASCII bytes get mis-parsed - see AGENTS.md.
rem
rem Usage:
rem   sync_web.cmd                 sync ui\ -> website\demo\ + player template
rem   sync_web.cmd -Check          verify only, change nothing (exit 1 if stale)
rem   sync_web.cmd -Serve          sync, then start the local preview server
rem   sync_web.cmd -Port 9001      preview port (with -Serve)
rem
rem The same script is called by tools\package_release.ps1 during a release, so
rem "manual sync" and "release sync" are literally the same code path.
rem
rem It always cd's to its own folder first, so it works from any directory.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

rem Detect double-click (explorer launches "cmd /c ""...\sync_web.cmd"""), so we
rem can keep the window open at the end only in that case.
set "PAUSEFLAG="
echo %cmdcmdline% | find /i "%~nx0" >nul 2>&1 && set "PAUSEFLAG=1"

echo [sync_web] Repo : %~dp0
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\sync_website.ps1" %*
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo [sync_web] Done.
) else (
    echo [sync_web] FAILED - exit code %RC%
)

if defined PAUSEFLAG pause
exit /b %RC%
