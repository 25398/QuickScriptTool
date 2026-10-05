# ──────────────────────────────────────────────────────────────────
# tools/sync_website_demo.ps1
#
# 把产品前端 ui/ 同步到官网 Demo website/demo/，并重新贴上「网页体验版」补丁。
#
# 为什么要有这个脚本（2026-09-28）：
#   官网 Demo 是从 ui/ 拷贝出来的，但「拷贝 + 手改三步」一直写在 website/README.md 里
#   靠人肉执行 ⇒ 实测已经漂移了整整一轮 UI 重构（demo 还停在 --qst-u 变量化之前，
#   缺 exportScriptAsExe / previewScriptActions / pickScreenDrag 等桥接方法）。
#   手工步骤必然漂移，所以固化成脚本；README 只保留「怎么跑」。
#
# 补丁清单（只有这两处，其余文件与 ui/ **逐字节一致**）：
#   1) demo/index.html  <title> 加「· 网页体验版」
#   2) demo/index.html  在 vendor/katex 之前注入「网页体验版 badge / 小屏提示」块
#   3) demo/index.html  bridge.stub.js 插到 bridge.js 之前、agent-inline.js 插到之后
#   4) demo/agent.html  bridge.stub.js 插到 bridge.js 之前
#
# ⚠ 补丁是在**内存里对 ui/ 的原文做的**，再与 website/demo 的现状比较 ——
#   所以 -Check 是幂等的（第一次跑它报「不一致」是对的，跑完再跑必须报一致）。
#
# 不含 ui/debug.html —— 它是开发用独立页，index.html 里只有一句注释提到它。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\sync_website_demo.ps1
#   powershell -ExecutionPolicy Bypass -File tools\sync_website_demo.ps1 -Check   # 只校验不写
# ──────────────────────────────────────────────────────────────────

[CmdletBinding()]
param(
    # 只校验 website/demo 与「ui/ + 补丁」是否一致（不改任何文件）
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $repo 'ui'
$dst  = Join-Path $repo 'website\demo'

if (-not (Test-Path $src)) { throw "找不到产品前端目录：$src" }
if (-not (Test-Path $dst)) { throw "找不到官网 Demo 目录：$dst" }

# ui/ 里需要同步过来的文件（debug.html 刻意排除）
$files = @(
    'app.js',
    'bridge.js',
    'visual_editor.js',
    'pro-mode.js',
    'shell.css',
    'agent.css',
    'pro-mode.css',
    'app_icon.ico'
)

function Read-Utf8([string]$p) {
    return [System.IO.File]::ReadAllText($p, (New-Object System.Text.UTF8Encoding($false)))
}
function Write-Utf8NoBom([string]$p, [string]$text) {
    [System.IO.File]::WriteAllText($p, $text, (New-Object System.Text.UTF8Encoding($false)))
}

$changed = 0

# ── 1) 逐字节拷贝（这些文件 demo 侧不做任何改动）──────────────────
foreach ($f in $files) {
    $s = Join-Path $src $f
    if (-not (Test-Path $s)) { Write-Warning "ui/$f 不存在，跳过"; continue }
    $d = Join-Path $dst $f
    $need = $true
    if (Test-Path $d) { $need = (Get-FileHash $s).Hash -ne (Get-FileHash $d).Hash }
    if ($need) {
        if (-not $Check) { Copy-Item -LiteralPath $s -Destination $d -Force }
        Write-Host ("  [拷贝] {0}" -f $f)
        $changed++
    }
}

# vendor（katex）整树同步
$vs = Join-Path $src 'vendor'
$vd = Join-Path $dst 'vendor'
if (Test-Path $vs) {
    foreach ($vf in (Get-ChildItem -Recurse -File -LiteralPath $vs)) {
        $rel = $vf.FullName.Substring($vs.Length).TrimStart('\')
        $target = Join-Path $vd $rel
        $need = $true
        if (Test-Path $target) { $need = (Get-FileHash $vf.FullName).Hash -ne (Get-FileHash $target).Hash }
        if ($need) {
            if (-not $Check) {
                $tdir = Split-Path -Parent $target
                if (-not (Test-Path $tdir)) { New-Item -ItemType Directory -Force -Path $tdir | Out-Null }
                Copy-Item -LiteralPath $vf.FullName -Destination $target -Force
            }
            Write-Host ("  [拷贝] vendor/{0}" -f $rel)
            $changed++
        }
    }
}

# ── 2) index.html：从 ui/ 原文打补丁，再与 demo 现状比 ───────────
function New-DemoIndexHtml([string]$html) {
    # 2.1 标题
    $html = $html -replace '<title>键鼠工坊</title>', '<title>键鼠工坊 · 网页体验版</title>'

    # 2.2 网页体验版标识（badge + 小屏提示），插在 vendor/katex 之前
    $badge = @'
<style id="qst-demo-badge-css">
/* 官网 Demo 标识：仅网页体验版注入，桌面壳中不存在 */
#qstDemoBadge{
  position:fixed;right:14px;bottom:14px;z-index:2147483600;
  display:flex;align-items:center;gap:10px;
  padding:9px 14px;border-radius:999px;
  background:linear-gradient(115deg,#0b1624,#122338 60%,#134e4a);
  border:1px solid rgba(56,189,248,.5);
  box-shadow:0 8px 26px rgba(6,16,24,.38),0 0 0 1px rgba(45,212,191,.12) inset;
  color:#dcebf7;font-size:12px;line-height:1;user-select:none;
}
#qstDemoBadge .qb-dot{
  width:8px;height:8px;border-radius:50%;flex-shrink:0;
  background:#2dd4bf;
  box-shadow:0 0 8px #2dd4bf;
}
#qstDemoBadge a{
  color:#7dd3fc;text-decoration:none;font-weight:600;
  white-space:nowrap;
}
#qstDemoBadge a:hover{text-decoration:underline}
#qstDemoBadge .qb-close{
  color:#8fa0ae;cursor:pointer;font-size:14px;line-height:1;padding:2px;
}
#qstDemoBadge .qb-close:hover{color:#fff}
body.editor-open #qstDemoBadge{display:none}
#qstDemoNote{
  position:fixed;left:50%;bottom:18px;transform:translateX(-50%);z-index:2147483600;
  display:none;align-items:center;gap:8px;
  padding:8px 14px;border-radius:10px;
  background:rgba(11,22,36,.9);border:1px solid rgba(56,189,248,.45);
  color:#dcebf7;font-size:12px;white-space:nowrap;
  box-shadow:0 8px 26px rgba(6,16,24,.38);
}
@media (max-width:1180px){
  #qstDemoNote{display:flex}
}
</style>
<div id="qstDemoBadge" title="网页体验版仅演示界面，不会注入真实键鼠">
  <i class="qb-dot"></i>
  <span>网页体验版 · 界面演示</span>
  <a href="../download.html" target="_blank" rel="noopener">下载客户端 →</a>
  <span class="qb-close" id="qstDemoBadgeClose" title="关闭提示" aria-label="关闭提示">×</span>
</div>
<div id="qstDemoNote">建议在 1400px 以上的窗口体验完整界面，或前往<a href="../demo.html" style="color:#7dd3fc;margin-left:4px">在线体验页</a></div>
<script>
(function () {
  try {
    var closeBtn = document.getElementById("qstDemoBadgeClose");
    if (closeBtn) {
      closeBtn.addEventListener("click", function () {
        var b = document.getElementById("qstDemoBadge");
        if (b) b.style.display = "none";
      });
    }
  } catch (e) {}
})();
</script>
'@
    $anchor = '<script src="vendor/katex/katex.min.js" defer></script>'
    if (-not $html.Contains($anchor)) { throw 'index.html 里找不到 vendor/katex 的 script 标签，无法插入体验版标识' }
    if (-not $html.Contains('id="qst-demo-badge-css"')) {
        $html = $html.Replace($anchor, $badge.TrimEnd("`r", "`n") + "`n" + $anchor)
    }

    # 2.3 桥接桩：bridge.stub.js 必须在 bridge.js 之前（它提供 window.qstBridge）；
    #     agent-inline.js 必须在 bridge.js 之后、app.js 之前（它改 qst.openAgentWindow）；
    #     ../export/qst-web-export.js 必须在 bridge.stub.js 之前 ——
    #     Demo 的「导出为独立 EXE」是真做（在浏览器里拼 exe），它要用这个模块。
    $m = [regex]::Match($html, '<script src="bridge\.js(\?[^"]*)?"([^>]*)></script>')
    if (-not $m.Success) { throw 'index.html 里找不到 bridge.js 的 script 标签，无法注入桥接桩' }
    if (-not $html.Contains('bridge.stub.js')) {
        $ver = $m.Groups[1].Value
        $attrs = $m.Groups[2].Value      # 例如 " defer"
        $inject = "<script src=""../export/qst-web-export.js""$attrs></script>`n" +
                  "<script src=""bridge.stub.js$ver""$attrs></script>`n" + $m.Value +
                  "`n<script src=""agent-inline.js?v=20260818""$attrs></script>"
        $html = $html.Replace($m.Value, $inject)
    }
    return $html
}

function New-DemoAgentHtml([string]$html) {
    $m = [regex]::Match($html, '<script src="bridge\.js(\?[^"]*)?"([^>]*)></script>')
    if (-not $m.Success) { throw 'agent.html 里找不到 bridge.js 的 script 标签，无法注入桥接桩' }
    if (-not $html.Contains('bridge.stub.js')) {
        $ver = $m.Groups[1].Value
        $attrs = $m.Groups[2].Value
        $html = $html.Replace($m.Value, "<script src=""bridge.stub.js$ver""$attrs></script>`n" + $m.Value)
    }
    return $html
}

$htmlJobs = @(
    @{ Name = 'index.html'; Fn = 'New-DemoIndexHtml'; Label = '标题 / 体验版标识 / 桥接桩顺序' },
    @{ Name = 'agent.html'; Fn = 'New-DemoAgentHtml'; Label = '桥接桩顺序' }
)
foreach ($job in $htmlJobs) {
    $sPath = Join-Path $src $job.Name
    if (-not (Test-Path $sPath)) { Write-Warning "ui/$($job.Name) 不存在，跳过"; continue }
    $want = & $job.Fn (Read-Utf8 $sPath)
    $dPath = Join-Path $dst $job.Name
    $have = if (Test-Path $dPath) { Read-Utf8 $dPath } else { '' }
    if ($want -ne $have) {
        if (-not $Check) { Write-Utf8NoBom $dPath $want }
        Write-Host ("  [补丁] {0}（{1}）" -f $job.Name, $job.Label)
        $changed++
    }
}

# ── 3) 结果 ─────────────────────────────────────────────────────
Write-Host ""
if ($changed -eq 0) {
    Write-Host "✔ website/demo 已与 ui/ 一致，无需改动。" -ForegroundColor Green
} elseif ($Check) {
    Write-Host "✘ website/demo 有 $changed 处与 ui/ 不一致（去掉 -Check 即可同步）。" -ForegroundColor Yellow
    exit 1
} else {
    Write-Host "✔ 已同步 $changed 个文件/补丁到 website\demo。" -ForegroundColor Green
    Write-Host "  建议接着跑：node --check website\demo\app.js" -ForegroundColor DarkGray
}
