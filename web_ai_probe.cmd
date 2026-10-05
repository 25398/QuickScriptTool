@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
echo.
echo ============================================================
echo  Web AI live probe - can we drive Doubao?
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
  echo [!!] Python not found. Install Python, or run:
  echo      python tools\verify\web_ai_live_probe.py
  echo.
  pause
  exit /b 2
)

"%PY%" tools\verify\web_ai_live_probe.py %*
set RC=%ERRORLEVEL%

echo.
echo ------------------------------------------------------------
echo  Done. Lines marked [OK] are fine, [!!] are problems.
echo  Copy the [!!] lines back to me if anything fails.
echo ------------------------------------------------------------
echo.
echo  To actually send one message (types into Doubao and clicks send):
echo    web_ai_probe.cmd --send "hello"
echo.
pause
exit /b %RC%
