# Sync build\Release artifacts to the install dir (default D:\quickscripttool).
# For local testing only; the real installer lives in installer\ + qst-release-package.
#
#   powershell -ExecutionPolicy Bypass -File tools\deploy_to_install_dir.ps1
#   powershell -ExecutionPolicy Bypass -File tools\deploy_to_install_dir.ps1 -WhatIf
#   powershell -ExecutionPolicy Bypass -File tools\deploy_to_install_dir.ps1 -InstallDir F:\quickscripttool
#
# WARNING: close the running QuickScriptTool first (the exe is locked otherwise).
# This script only reports; it never kills your process.
#
# NOTE: keep this file ASCII-only. PowerShell 5.1 reads .ps1 as GB2312 on zh-CN
# hosts, so any UTF-8 CJK text here would be mangled into syntax errors.
param(
    [string]$InstallDir = 'D:\quickscripttool',
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot '..\build\Release'

if (-not (Test-Path $src)) { throw "Build output dir not found: $src (build QstWebViewShell first)" }
if (-not (Test-Path $InstallDir)) { throw "Install dir not found: $InstallDir (use -InstallDir to override)" }

# Main exe + both bitness DLLs. 32/64 are independent builds: always ship together,
# otherwise the two processes behave differently.
$files = @('QuickScriptTool.exe', 'FakeFocus64.dll', 'FakeFocus32.dll')

# Occupancy check: do NOT kill the user's process.
$running = Get-Process -Name 'QuickScriptTool' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host '[SKIP] QuickScriptTool is running. Close it, then re-run.' -ForegroundColor Yellow
    $running | Select-Object Id, ProcessName, StartTime | Format-Table -AutoSize | Out-Host
    exit 2
}

Write-Host "source : $src"
Write-Host "target : $InstallDir"
Write-Host ''

$plan = @()
foreach ($f in $files) {
    $s = Join-Path $src $f
    $d = Join-Path $InstallDir $f
    if (-not (Test-Path $s)) {
        Write-Host ("  [missing-src] {0}" -f $f) -ForegroundColor Yellow
        continue
    }
    $sItem = Get-Item $s
    $sTime = $sItem.LastWriteTime
    $sSize = $sItem.Length
    $dTimeText = 'absent'
    $dSize = 0
    $same = $false
    if (Test-Path $d) {
        $dItem = Get-Item $d
        $dTimeText = $dItem.LastWriteTime.ToString('MM-dd HH:mm')
        $dSize = $dItem.Length
        $same = ($dItem.LastWriteTime -eq $sTime) -and ($dSize -eq $sSize)
    }
    if ($same) {
        Write-Host ("  [same] {0,-22} build={1:MM-dd HH:mm} {2,10} B  install={3} {4,10} B" -f `
            $f, $sTime, $sSize, $dTimeText, $dSize) -ForegroundColor DarkGray
    } else {
        Write-Host ("  [UPDATE] {0,-20} build={1:MM-dd HH:mm} {2,10} B  install={3} {4,10} B" -f `
            $f, $sTime, $sSize, $dTimeText, $dSize) -ForegroundColor Green
        $plan += , @($s, $d, $f)
    }
}

Write-Host ''
if ($plan.Count -eq 0) {
    Write-Host 'All files already up to date. Nothing to do.' -ForegroundColor Green
    exit 0
}

if ($WhatIf) {
    Write-Host ("(-WhatIf) would copy {0} file(s). Nothing was written." -f $plan.Count) -ForegroundColor Cyan
    exit 0
}

foreach ($item in $plan) {
    $s, $d, $name = $item
    Copy-Item -LiteralPath $s -Destination $d -Force
    Write-Host ("  updated: {0}" -f $name) -ForegroundColor Green
}

Write-Host ''
Write-Host ("Done. {0} file(s) copied to {1}" -f $plan.Count, $InstallDir) -ForegroundColor Green
Write-Host 'Tip: if a desktop shortcut points at an old dir, run tools\fix_desktop_shortcut.ps1' -ForegroundColor DarkGray
