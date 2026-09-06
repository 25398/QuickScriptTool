@echo off
chcp 65001 >nul
setlocal EnableExtensions
cd /d "%~dp0"
set "REPORT=%~dp0diagnose_report.txt"
set "APPDATA_LOG=%LOCALAPPDATA%\QuickScriptTool\shell_startup.log"

echo === QuickScriptTool diagnose === > "%REPORT%"
echo time=%date% %time%>> "%REPORT%"
echo dir=%cd%>> "%REPORT%"
echo.>> "%REPORT%"

echo [files]>> "%REPORT%"
if exist "QuickScriptTool.exe" (echo QuickScriptTool.exe=OK>> "%REPORT%") else (echo QuickScriptTool.exe=MISSING>> "%REPORT%")
if exist "opencv_world4100.dll" (echo opencv_world4100.dll=OK>> "%REPORT%") else (echo opencv_world4100.dll=MISSING>> "%REPORT%")
if exist "vcruntime140.dll" (echo vcruntime140.dll=OK>> "%REPORT%") else (echo vcruntime140.dll=MISSING>> "%REPORT%")
if exist "msvcp140.dll" (echo msvcp140.dll=OK>> "%REPORT%") else (echo msvcp140.dll=MISSING>> "%REPORT%")
if exist "WebView2Fixed\msedgewebview2.exe" (echo WebView2Fixed=OK>> "%REPORT%") else (echo WebView2Fixed=MISSING>> "%REPORT%")
if exist "ui\index.html" (echo ui=OK>> "%REPORT%") else (echo ui=MISSING>> "%REPORT%")
if exist "%SystemRoot%\System32\MFPlat.DLL" (echo MFPlat.DLL=OK>> "%REPORT%") else (echo MFPlat.DLL=MISSING>> "%REPORT%")
if exist "%SystemRoot%\System32\MF.dll" (echo MF.dll=OK>> "%REPORT%") else (echo MF.dll=MISSING>> "%REPORT%")
if exist "%SystemRoot%\System32\MFReadWrite.dll" (echo MFReadWrite.dll=OK>> "%REPORT%") else (echo MFReadWrite.dll=MISSING>> "%REPORT%")
if exist "FakeFocus32.dll" (echo FakeFocus32.dll=OK>> "%REPORT%") else (echo FakeFocus32.dll=MISSING>> "%REPORT%")
if exist "FakeFocus64.dll" (echo FakeFocus64.dll=OK>> "%REPORT%") else (echo FakeFocus64.dll=MISSING>> "%REPORT%")
echo.>> "%REPORT%"
echo [fakefocus_files]>> "%REPORT%"
powershell -NoProfile -Command "Get-ChildItem -LiteralPath . -Filter 'FakeFocus*.dll' -ErrorAction SilentlyContinue | ForEach-Object { '{0} size={1} time={2:yyyy-MM-dd HH:mm:ss}' -f $_.Name, $_.Length, $_.LastWriteTime }" >> "%REPORT%"
echo.>> "%REPORT%"
echo [window_mode_debug.log]>> "%REPORT%"
if exist "window_mode_debug.log" (
  echo --- last 120 lines --- >> "%REPORT%"
  powershell -NoProfile -Command "Get-Content -LiteralPath 'window_mode_debug.log' -Tail 120 -ErrorAction SilentlyContinue" >> "%REPORT%"
) else (
  echo window_mode_debug.log=MISSING>> "%REPORT%"
)
echo.>> "%REPORT%"

echo [logs_before_launch]>> "%REPORT%"
if exist "shell_startup.log" (
  echo --- exe_dir shell_startup.log --- >> "%REPORT%"
  type "shell_startup.log" >> "%REPORT%"
) else (echo exe_dir_shell_startup.log=NO>> "%REPORT%")
if exist "%APPDATA_LOG%" (
  echo --- appdata shell_startup.log --- >> "%REPORT%"
  type "%APPDATA_LOG%" >> "%REPORT%"
) else (echo appdata_shell_startup.log=NO>> "%REPORT%")
if exist "webview_boot.log" (
  echo --- webview_boot.log before --- >> "%REPORT%"
  type "webview_boot.log" >> "%REPORT%"
) else (echo webview_boot.log=NO>> "%REPORT%")
echo.>> "%REPORT%"

echo [launch]>> "%REPORT%"
echo Starting QuickScriptTool.exe ...>> "%REPORT%"
start /wait "" "%~dp0QuickScriptTool.exe"
set "EC=%ERRORLEVEL%"
echo exit_code=%EC%>> "%REPORT%"
rem Decode common NTSTATUS crash codes (cmd ERRORLEVEL is signed; also accept unsigned)
if "%EC%"=="-1073740771" echo exit_meaning=0xC000041D STATUS_FATAL_USER_CALLBACK_EXCEPTION (often LL-hook / window callback crash^)>> "%REPORT%"
if "%EC%"=="3221226525" echo exit_meaning=0xC000041D STATUS_FATAL_USER_CALLBACK_EXCEPTION (often LL-hook / window callback crash^)>> "%REPORT%"
if "%EC%"=="-1073741819" echo exit_meaning=0xC0000005 ACCESS_VIOLATION>> "%REPORT%"
if "%EC%"=="3221225477" echo exit_meaning=0xC0000005 ACCESS_VIOLATION>> "%REPORT%"
if "%EC%"=="-1073741515" echo exit_meaning=0xC0000135 DLL_NOT_FOUND>> "%REPORT%"
if "%EC%"=="-1073741701" echo exit_meaning=0xC000007B BAD_IMAGE_FORMAT>> "%REPORT%"
if "%EC%"=="0" echo exit_meaning=ok_or_handoff>> "%REPORT%"
echo.>> "%REPORT%"

echo [logs_after_launch]>> "%REPORT%"
if exist "shell_startup.log" (
  echo --- exe_dir shell_startup.log --- >> "%REPORT%"
  type "shell_startup.log" >> "%REPORT%"
) else (echo exe_dir_shell_startup.log=NO>> "%REPORT%")
if exist "webview_boot.log" (
  echo --- webview_boot.log --- >> "%REPORT%"
  type "webview_boot.log" >> "%REPORT%"
) else (echo webview_boot.log=NO>> "%REPORT%")
if exist "%APPDATA_LOG%" (
  echo --- appdata shell_startup.log --- >> "%REPORT%"
  type "%APPDATA_LOG%" >> "%REPORT%"
) else (echo appdata_shell_startup.log=NO>> "%REPORT%")
echo.>> "%REPORT%"
echo Send this file to support: %REPORT%>> "%REPORT%"

echo.
echo ========================================
echo 诊断完成。请把下面文件发给开发者：
echo %REPORT%
echo.
echo 日志也可能在：
echo 1^) 本目录 shell_startup.log
echo 2^) %APPDATA_LOG%
echo 3^) 本目录 webview_boot.log
echo 4^) 本目录 window_mode_debug.log  （窗口模式 / 冒险岛后台必看）
echo 5^) 本目录 FakeFocus32.dll 时间戳须新于修复日期
echo ========================================
echo.
notepad "%REPORT%"
endlocal
