@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
echo.
echo ============================================================
echo  Bridge abort-latch fix verification
echo ============================================================
echo.

set PY=
if exist "C:\Users\%USERNAME%\.workbuddy-ai\binaries\python\versions\3.13.12\python.exe" (
  set PY=C:\Users\%USERNAME%\.workbuddy-ai\binaries\python\versions\3.13.12\python.exe
)
if "%PY%"=="" (
  where python >nul 2>nul && set PY=python
)
if "%PY%"=="" (
  where py >nul 2>nul && set PY=py
)
if "%PY%"=="" (
  echo [!!] Python not found.
  echo.
  pause
  exit /b 2
)

"%PY%" tools\verify\verify_bridge_abort_fix.py %*
set RC=%ERRORLEVEL%

echo.
echo ------------------------------------------------------------
echo  Step 1: run this file as-is      -^> expect [OK]
echo  Step 2: in the app, run a script with window-mode OFF,
echo          then STOP it
echo  Step 3: run:  check_bridge_fix.cmd --after   -^> must stay [OK]
echo ------------------------------------------------------------
echo.
pause
exit /b %RC%
