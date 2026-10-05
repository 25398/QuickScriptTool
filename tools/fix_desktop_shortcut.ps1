# fix_desktop_shortcut.ps1 — 修复桌面快捷方式的「大图标失效 / 变成一张白纸」
#
# 背景：桌面快捷方式若没写 IconLocation，图标就继承自目标 exe。本项目的开发副本
# 在 build\Release 下每次构建都会被替换；Explorer 的图标缓存只要在那次替换的空档里
# 取不到图标，就会把「空白图标」长期缓存下来（表现就是桌面上一张白纸，右键属性里
# 目标路径却是对的）。安装包侧已改成显式 IconFilename + 装完通知 Shell 刷新；
# 本脚本用来**修已经坏掉的那一个**（安装包不会覆盖旧名字的快捷方式）。
#
# 做三件事：
#   1) 找到真实安装目录（注册表 InstallPath → 仓库开发输出目录）；
#   2) 重建/修正桌面快捷方式，图标显式指到 app_icon.ico（内容稳定，不再被构建打断）；
#   3) 通知 Shell 刷新图标缓存。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\fix_desktop_shortcut.ps1
#   powershell -ExecutionPolicy Bypass -File tools\fix_desktop_shortcut.ps1 -AppDir "D:\other\software\build\Release"
#   powershell -ExecutionPolicy Bypass -File tools\fix_desktop_shortcut.ps1 -WhatIf   # 只看计划
#
# 注意：必须在普通 PowerShell / CMD 终端里跑（本机某些受限宿主禁止 COM 与启动外部程序）。

[CmdletBinding()]
param(
    [string]$AppDir = "",
    [string]$ShortcutName = "键鼠工坊",
    [switch]$WhatIf
)

$ErrorActionPreference = "Stop"

function Test-AppDir([string]$dir) {
    if ([string]::IsNullOrWhiteSpace($dir)) { return $false }
    return Test-Path (Join-Path $dir "QuickScriptTool.exe")
}

function Find-AppDir {
    foreach ($k in @("HKLM:\SOFTWARE\QuickScriptTool", "HKCU:\SOFTWARE\QuickScriptTool")) {
        if (-not (Test-Path $k)) { continue }
        $p = (Get-ItemProperty -Path $k -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
        if (Test-AppDir $p) { return $p }
    }
    $repo = Split-Path -Parent $PSScriptRoot
    foreach ($cand in @("build\Release", "dist\QuickScriptTool")) {
        $p = Join-Path $repo $cand
        if (Test-AppDir $p) { return $p }
    }
    return ""
}

if (-not (Test-AppDir $AppDir)) {
    $found = Find-AppDir
    if ($found) { $AppDir = $found }
}
if (-not (Test-AppDir $AppDir)) {
    Write-Host "[fix-shortcut] 找不到 QuickScriptTool.exe；请用 -AppDir 指定安装目录。" -ForegroundColor Red
    exit 1
}
$AppDir = (Resolve-Path $AppDir).Path
$exe = Join-Path $AppDir "QuickScriptTool.exe"
$ico = Join-Path $AppDir "app_icon.ico"
if (-not (Test-Path $ico)) {
    Write-Host "[fix-shortcut] 警告：$ico 不存在，图标退回用 exe 自身（仍可能被构建打断）。" -ForegroundColor Yellow
    $ico = $exe
}

$desktop = [Environment]::GetFolderPath("Desktop")
# 旧名字（QuickScriptTool.lnk）也一起修：安装包只按新名字 {autodesktop}\键鼠工坊 建，
# 不会覆盖旧的那一个，而用户桌面上摆着的往往正是旧的那一个。
$targets = New-Object System.Collections.Generic.List[string]
$newLnk = Join-Path $desktop ($ShortcutName + ".lnk")
$targets.Add($newLnk)
foreach ($old in @("QuickScriptTool.lnk", "QstWebViewShell.lnk")) {
    $p = Join-Path $desktop $old
    if (Test-Path $p) { $targets.Add($p) }
}

Write-Host "[fix-shortcut] 安装目录: $AppDir"
Write-Host "[fix-shortcut] 图标文件: $ico"
foreach ($t in $targets) {
    $exists = Test-Path $t
    if ($WhatIf) {
        Write-Host ("[fix-shortcut] 将{0}：{1}" -f ($(if ($exists) { "修正" } else { "新建" })), $t)
        continue
    }
    $ws = New-Object -ComObject WScript.Shell
    $sc = $ws.CreateShortcut($t)
    $sc.TargetPath = $exe
    $sc.WorkingDirectory = $AppDir
    $sc.IconLocation = "$ico,0"
    $sc.Description = "键鼠工坊"
    $sc.Save()
    Write-Host ("[fix-shortcut] 已{0}：{1}" -f ($(if ($exists) { "修正" } else { "新建" })), $t) -ForegroundColor Green
}

if (-not $WhatIf) {
    # 图标缓存刷新：ie4uinit -show 是 Windows 10/11 官方的「重读图标缓存」入口，
    # 不删缓存库、不动资源管理器。若个别机器上仍显示白纸，重启一次资源管理器即可。
    $ie4u = Join-Path $env:SystemRoot "System32\ie4uinit.exe"
    if (Test-Path $ie4u) {
        Start-Process -FilePath $ie4u -ArgumentList "-show" -WindowStyle Hidden
        Write-Host "[fix-shortcut] 已请求 Shell 刷新图标缓存。" -ForegroundColor Green
    }
    Write-Host "[fix-shortcut] 完成。若桌面仍显示白纸：任务管理器里重启「Windows 资源管理器」。" -ForegroundColor Cyan
}
