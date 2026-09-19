<#
.SYNOPSIS
  dist\ 产物保留策略：只留最近 N 个版本，旧的归档到 dist\archive\，并清理打包临时目录。

.DESCRIPTION
  架构评估 #15：dist\ 曾累积到 21 GB / 573 文件（56 个历史 exe、35 个历史 zip、
  _edge_pack_stage_* 临时目录），既没有保留策略也没有清理入口。

  默认行为是**非破坏性**的：
    - 带版本号的历史产物，超过 KeepVersions 的移入 dist\archive\（不是删除）；
    - _edge_pack_stage_* / *_stage_* 这类打包临时目录直接删除（可再生）；
    - 无版本号的固定别名（QuickScriptTool-Release.zip / QuickScriptTool-Setup.exe /
      QuickScriptTool-HidDriver.zip 等，官网按钮用）永不移动。

  归档目录本身不会自动清空——要真正回收磁盘请显式 -PurgeArchive，
  或手动删除 dist\archive\。

.PARAMETER KeepVersions
  每个产物系列保留最近几个版本（按语义版本排序），默认 5。

.PARAMETER PurgeArchive
  移动完成后清空 dist\archive\（真正回收磁盘）。

.PARAMETER DryRun
  只打印将要做什么，不改动任何文件。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\prune_dist.ps1 -DryRun
  powershell -ExecutionPolicy Bypass -File tools\prune_dist.ps1 -KeepVersions 3
  powershell -ExecutionPolicy Bypass -File tools\prune_dist.ps1 -PurgeArchive
#>
[CmdletBinding()]
param(
    [int]$KeepVersions = 5,

    [string]$DistRoot = '',

    [switch]$PurgeArchive,

    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
if ($KeepVersions -lt 1) { throw 'KeepVersions 必须 >= 1' }

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $DistRoot) { $DistRoot = Join-Path $repoRoot 'dist' }
if (-not (Test-Path -LiteralPath $DistRoot)) {
    Write-Host "dist 目录不存在，跳过：$DistRoot"
    exit 0
}

$archiveDir = Join-Path $DistRoot 'archive'

function Format-Size([long]$bytes) {
    if ($bytes -ge 1GB) { return ('{0:N2} GB' -f ($bytes / 1GB)) }
    if ($bytes -ge 1MB) { return ('{0:N1} MB' -f ($bytes / 1MB)) }
    if ($bytes -ge 1KB) { return ('{0:N1} KB' -f ($bytes / 1KB)) }
    return "$bytes B"
}

# ── 1) 打包临时目录：可再生，直接删 ───────────────────────────────
$tempDirs = @(Get-ChildItem -LiteralPath $DistRoot -Directory -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like '_edge_pack_stage_*' -or $_.Name -like '_stage_*' })
foreach ($d in $tempDirs) {
    if ($DryRun) { Write-Host "[DryRun] 删除临时目录 $($d.Name)" }
    else {
        Remove-Item -LiteralPath $d.FullName -Recurse -Force
        Write-Host "已删除打包临时目录：$($d.Name)"
    }
}

# ── 2) 带版本号的历史产物：保留最近 KeepVersions 个 ───────────────
# 每个系列 = (文件名前缀, 扩展名, 版本正则)。版本正则的第一个捕获组必须是版本号。
$series = @(
    @{ Name = '安装包(QuickScriptTool-<ver>.exe)'; Regex = '^QuickScriptTool-(\d+(?:\.\d+)*)\.exe$' },
    @{ Name = '安装包(Setup-<ver>)';               Regex = '^QuickScriptTool-Setup-(\d+(?:\.\d+)*)\.exe$' },
    @{ Name = '绿色版 zip(Release-<ver>)';         Regex = '^QuickScriptTool-Release-(\d+(?:\.\d+)*)\.zip$' },
    @{ Name = 'Edge 扩展(QstEdgeBridge-<ver>)';    Regex = '^QstEdgeBridge-(\d+(?:\.\d+)*)\.zip$' },
    @{ Name = 'WebView 便携(QstWebViewShell-<ver>)'; Regex = '^QstWebViewShell-(\d+(?:\.\d+)*)\.zip$' }
)

$movedCount = 0
$movedBytes = [long]0

foreach ($s in $series) {
    $files = @(Get-ChildItem -LiteralPath $DistRoot -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $s.Regex })
    if ($files.Count -le $KeepVersions) { continue }

    # 按版本号排序（不能按文件名，1.0.10 会排在 1.0.9 前面）
    $sorted = @($files | Sort-Object -Property @{ Expression = {
        $m = [regex]::Match($_.Name, $s.Regex)
        if ($m.Success) { [version]$m.Groups[1].Value } else { [version]'0.0' }
    } } -Descending)

    $stale = @($sorted | Select-Object -Skip $KeepVersions)
    if ($stale.Count -eq 0) { continue }

    Write-Host ("{0}：保留 {1} 个，归档 {2} 个" -f $s.Name, $KeepVersions, $stale.Count)
    foreach ($f in $stale) {
        if ($DryRun) {
            Write-Host ("  [DryRun] {0} ({1}) -> dist\archive\" -f $f.Name, (Format-Size $f.Length))
        } else {
            if (-not (Test-Path -LiteralPath $archiveDir)) {
                New-Item -ItemType Directory -Path $archiveDir -Force | Out-Null
            }
            $dest = Join-Path $archiveDir $f.Name
            if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Force }
            Move-Item -LiteralPath $f.FullName -Destination $dest -Force
            Write-Host ("  {0} ({1}) -> dist\archive\" -f $f.Name, (Format-Size $f.Length))
        }
        $movedCount++
        $movedBytes += $f.Length
    }
}

# ── 3) 可选：清空归档目录 ─────────────────────────────────────────
if ($PurgeArchive) {
    if (-not (Test-Path -LiteralPath $archiveDir)) {
        Write-Host 'dist\archive\ 不存在，无需清理。'
    } else {
        $size = (Get-ChildItem -LiteralPath $archiveDir -Recurse -File -Force -ErrorAction SilentlyContinue |
            Measure-Object -Property Length -Sum).Sum
        if (-not $size) { $size = 0 }
        if ($DryRun) {
            Write-Host ("[DryRun] 清空 dist\archive\（{0}）" -f (Format-Size ([long]$size)))
        } else {
            Remove-Item -LiteralPath $archiveDir -Recurse -Force
            Write-Host ("已清空 dist\archive\（回收 {0}）" -f (Format-Size ([long]$size)))
        }
    }
}

# ── 汇总 ──────────────────────────────────────────────────────────
$total = (Get-ChildItem -LiteralPath $DistRoot -Recurse -File -Force -ErrorAction SilentlyContinue |
    Measure-Object -Property Length -Sum).Sum
if (-not $total) { $total = 0 }
Write-Host ''
if ($movedCount -gt 0) {
    Write-Host ("保留策略：归档 {0} 个文件，共 {1}" -f $movedCount, (Format-Size $movedBytes))
    if (-not $PurgeArchive) {
        Write-Host '提示：dist\archive\ 仍占用磁盘；确认无用后跑 -PurgeArchive 或手动删除。'
    }
} else {
    Write-Host '保留策略：没有超出保留数量的历史产物。'
}
Write-Host ("dist\ 当前总占用：{0}" -f (Format-Size ([long]$total)))
