# ──────────────────────────────────────────────────────────────────
# tools/sync_website.ps1
#
# **一键**把「当前产品前端 + 播放器模板」同步到 website\（官网 / Demo / 在线导出）。
#
# 为什么要有这一层（2026-09-28 用户要求）：
#   同步原本分成两个脚本（sync_website_demo.ps1 / sync_web_export_player.ps1），
#   发版时得记得各跑一次 —— 「要记得做的事」迟早会忘，而忘了的后果是**静默**的：
#     · Demo 停在旧 UI ⇒ 用户看到的网页版和软件不是一回事；
#     · 模板与产品脱节 ⇒ 网页导出的 exe 在跑旧引擎，产物照样能跑，看不出来。
#   所以收成一个入口，并且**发版脚本调用的就是它**（tools\package_release.ps1），
#   于是「发一次版」和「手动同步一次」走的是同一条路，不会漂移。
#
# 做三件事：
#   1. ui/            → website\demo\      （含「网页体验版」补丁）
#   2. QstPlayer.exe  → website\export\player\
#   3. 守卫：对同步后的 demo JS 跑 node --check（有 node 才跑）
#      —— 发版会把这份 JS 直接推到公网，语法错必须在这里拦住，不能等用户白屏。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\sync_website.ps1
#   powershell -ExecutionPolicy Bypass -File tools\sync_website.ps1 -Check     # 只校验不写
#   powershell -ExecutionPolicy Bypass -File tools\sync_website.ps1 -Serve     # 同步完顺手起本地预览
#
# 也有一键入口：仓库根 `sync_web.cmd`（可直接双击）。
# ──────────────────────────────────────────────────────────────────

[CmdletBinding()]
param(
    # 只校验（不一致就 exit 1，不写文件）
    [switch]$Check,
    # 同步完成后启动本地预览服务器（前台运行，Ctrl+C 退出）
    [switch]$Serve,
    # 起预览服务器用的端口
    [int]$Port = 8090
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$demoSync  = Join-Path $PSScriptRoot 'sync_website_demo.ps1'
$tplSync   = Join-Path $PSScriptRoot 'sync_web_export_player.ps1'
$serveScript = Join-Path $PSScriptRoot 'serve_website.ps1'
$demoDir   = Join-Path $repo 'website\demo'

foreach ($s in @($demoSync, $tplSync)) {
    if (-not (Test-Path -LiteralPath $s)) { throw "找不到子脚本：$s" }
}

function Write-Step([string]$t) { Write-Host ""; Write-Host "--- $t ---" -ForegroundColor Cyan }
function Write-Ok([string]$t)   { Write-Host "  + $t" -ForegroundColor Green }
function Write-Bad([string]$t)  { Write-Host "  x $t" -ForegroundColor Red }
function Write-Warn2([string]$t){ Write-Host "  ! $t" -ForegroundColor Yellow }

if ($Check) { Write-Host "`n[同步] 只校验模式（不改任何文件）" -ForegroundColor Yellow }

$failed = @()

# ── 1) ui/ → website\demo\ ───────────────────────────────────────
Write-Step "1/3 产品前端 ui\ → website\demo\"
$demoArgs = @('-ExecutionPolicy', 'Bypass', '-File', $demoSync)
if ($Check) { $demoArgs += '-Check' }
& powershell @demoArgs
if ($LASTEXITCODE -ne 0) {
    if ($Check) { $failed += 'Demo 与 ui/ 不一致' } else { $failed += 'Demo 同步失败' }
}

# ── 2) 播放器模板 → website\export\player\ ──────────────────────
Write-Step "2/3 播放器模板 → website\export\player\"
$tplArgs = @('-ExecutionPolicy', 'Bypass', '-File', $tplSync)
if ($Check) { $tplArgs += '-Check' }
& powershell @tplArgs
if ($LASTEXITCODE -ne 0) {
    if ($Check) { $failed += '播放器模板不一致或缺失' } else { $failed += '播放器模板同步失败' }
}

# ── 3) 守卫：同步后的 demo JS 必须过语法检查 ────────────────────
# 为什么放在这里：这段 JS 会被发版直接推到公网。语法错在浏览器里就是**整页白屏**，
# 而它来自 ui/（本仓长期有并行会话在改），构建不会拦（JS 不参与编译）。
# 有 node 才检查；没有就明确说一句"跳过"，不假装检查过了。
Write-Step "3/3 语法守卫（node --check）"
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Warn2 "没找到 node，跳过语法检查（这只影响守卫，不影响同步结果）"
} else {
    $jsFiles = @('app.js', 'bridge.js', 'visual_editor.js', 'pro-mode.js', 'bridge.stub.js')
    $bad = @()
    foreach ($f in $jsFiles) {
        $p = Join-Path $demoDir $f
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { & node --check $p 2>&1 | Out-Null; $rc = $LASTEXITCODE }
        finally { $ErrorActionPreference = $prev }
        if ($rc -ne 0) { $bad += $f; Write-Bad "$f 语法错误" } else { Write-Ok $f }
    }
    if ($bad.Count -gt 0) {
        $failed += ("demo JS 语法错误：" + ($bad -join ', ') + "（多半是 ui/ 里有并行会话没改完）")
    }
}

# ── 结果 ─────────────────────────────────────────────────────────
Write-Host ""
if ($failed.Count -gt 0) {
    Write-Host "同步未通过：" -ForegroundColor Red
    foreach ($f in $failed) { Write-Host "  - $f" -ForegroundColor Red }
    Write-Host ""
    if ($Check) { Write-Host "去掉 -Check 即可同步。" -ForegroundColor DarkGray }
    exit 1
}
Write-Ok "全部同步/校验通过。"

if ($Serve) {
    if (-not (Test-Path -LiteralPath $serveScript)) {
        Write-Warn2 "找不到 serve_website.ps1，跳过预览"
        exit 0
    }
    Write-Host ""
    Write-Host "启动本地预览（Ctrl+C 停止）…" -ForegroundColor Cyan
    & powershell -ExecutionPolicy Bypass -File $serveScript -Port $Port
}
exit 0
