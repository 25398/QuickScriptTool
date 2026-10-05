@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
echo.
echo ============================================================
echo  Web AI log tail  -  for crash / connection diagnosis
echo ============================================================
echo.

set PY=
if exist "C://Users//%USERNAME%//.workbuddy-ai//binaries//python//versions//3.13.12//python.exe" (
  set PY=C://Users//%USERNAME%//.workbuddy-ai//binaries//python//versions//3.13.12//python.exe
)
if "%PY%"=="" (
  where python >nul 2>nul && set PY=python
)
if "%PY%"=="" (
  where py >nul 2>nul && set PY=py
)
if "%PY%"=="" (
  echo [!!] Python not found. Install Python, or run:
  echo      python tools\verify\web_ai_log_tail.py
  echo.
  pause
  exit /b 2
)

"%PY%" tools\verify\web_ai_log_tail.py %*
set RC=%ERRORLEVEL%
echo.
echo ------------------------------------------------------------
echo Copy the block above and send it back.
echo ------------------------------------------------------------
pause
exit /b %RC%
