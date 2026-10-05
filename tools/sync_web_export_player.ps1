# ──────────────────────────────────────────────────────────────────
# tools/sync_web_export_player.ps1
#
# 把播放器模板同步到 website\export\player\QstPlayer.exe（网页在线导出用）。
#
# 为什么单独有这个脚本：
#   `website/export/` 的「在线脚本工坊」与官网 Demo 的「导出为独立 EXE」都在
#   **浏览器里**把脚本追加到这个模板后面（格式见 src/script_package.h 的 payload 说明）。
#   模板不在位 ⇒ 网页导出直接报「不是有效的 exe」。
#   完整发版（release.cmd）会自己同步它；但只想改官网文案、单独部署网站时，
#   用这个脚本一条命令补上就够了，不必跑整套打包。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\sync_web_export_player.ps1
#   powershell -ExecutionPolicy Bypass -File tools\sync_web_export_player.ps1 -Check
# ──────────────────────────────────────────────────────────────────

[CmdletBinding()]
param(
    # 只校验（缺了或不一致就 exit 1，不写文件）
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$candidates = @(
    (Join-Path $repo 'build\Release\tools\player\QstPlayer.exe'),
    (Join-Path $repo 'dist\QuickScriptTool\tools\player\QstPlayer.exe'),
    (Join-Path $repo 'build-solo\Release\tools\player\QstPlayer.exe')
)
$dstDir = Join-Path $repo 'website\export\player'
$dst = Join-Path $dstDir 'QstPlayer.exe'

$src = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $src) {
    Write-Host "✘ 找不到播放器模板，先构建一次：" -ForegroundColor Red
    Write-Host "    MSBuild build\QuickScriptTool.sln /t:QstScriptPlayer /p:Configuration=Release" -ForegroundColor DarkGray
    Write-Host "  查找过：" -ForegroundColor DarkGray
    $candidates | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    exit 1
}

$srcSize = (Get-Item -LiteralPath $src).Length
if (Test-Path -LiteralPath $dst) {
    $same = (Get-FileHash -LiteralPath $src).Hash -eq (Get-FileHash -LiteralPath $dst).Hash
    if ($same) {
        Write-Host ("✔ 已是最新：website\export\player\QstPlayer.exe ({0:N0} bytes)" -f $srcSize) -ForegroundColor Green
        exit 0
    }
    if ($Check) {
        Write-Host "✘ website\export\player\QstPlayer.exe 与构建产物不一致（去掉 -Check 即可同步）。" -ForegroundColor Yellow
        exit 1
    }
} elseif ($Check) {
    Write-Host "✘ website\export\player\QstPlayer.exe 缺失 —— 网页在线导出会失效。" -ForegroundColor Yellow
    exit 1
}

New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
Copy-Item -LiteralPath $src -Destination $dst -Force
Write-Host ("✔ 已同步 {0}" -f $src) -ForegroundColor Green
Write-Host ("  → website\export\player\QstPlayer.exe ({0:N0} bytes)" -f $srcSize)
Write-Host "  提示：上传网站后打开 export/selftest.html 确认链路可用。" -ForegroundColor DarkGray
