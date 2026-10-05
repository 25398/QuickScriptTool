# ──────────────────────────────────────────────────────────────────
# tools/serve_website.ps1
#
# 在本地起一个**只读静态服务器**指向 website\，用来预览官网 / Demo / 在线脚本工坊。
#
# 为什么必须有它（2026-09-28 用户实测反馈「我在本地打开貌似不能导出 exe」）：
#   直接双击 `website\demo\index.html` 是 `file://` 协议，而浏览器**禁止 fetch 读本地文件**
#   ⇒ 在线导出取不到播放器模板，只会弹一句 `Failed to fetch`。
#   `file://` 下没有任何绕法（XHR 同样被拦），必须走 http。
#   本机没装 Python，所以这里用 .NET 的 HttpListener 自己起一个 —— 零依赖、不需要管理员。
#
# 用法（在仓库根目录）：
#   powershell -ExecutionPolicy Bypass -File tools\serve_website.ps1
#   powershell -ExecutionPolicy Bypass -File tools\serve_website.ps1 -Port 9000 -NoBrowser
#
# 它**只监听 127.0.0.1**（不对外），**只读**文件，不写任何东西。
# ──────────────────────────────────────────────────────────────────

[CmdletBinding()]
param(
    [int]$Port = 8090,
    [switch]$NoBrowser
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path $repo 'website'
if (-not (Test-Path $root)) { throw "找不到 website 目录：$root" }

$mime = @{
    '.html' = 'text/html; charset=utf-8'
    '.js'   = 'text/javascript; charset=utf-8'
    '.css'  = 'text/css; charset=utf-8'
    '.json' = 'application/json; charset=utf-8'
    '.ico'  = 'image/x-icon'
    '.png'  = 'image/png'
    '.jpg'  = 'image/jpeg'
    '.jpeg' = 'image/jpeg'
    '.bmp'  = 'image/bmp'
    '.svg'  = 'image/svg+xml'
    '.woff2' = 'font/woff2'
    '.exe'  = 'application/octet-stream'
    '.zip'  = 'application/zip'
    '.txt'  = 'text/plain; charset=utf-8'
    '.md'   = 'text/plain; charset=utf-8'
}

$listener = [System.Net.HttpListener]::new()
$prefix = "http://127.0.0.1:$Port/"
$listener.Prefixes.Add($prefix)
try {
    $listener.Start()
} catch {
    Write-Host "✘ 无法监听 $prefix —— 端口可能被占用（换 -Port 9001 试试）。" -ForegroundColor Red
    Write-Host "  $($_.Exception.Message)" -ForegroundColor DarkGray
    exit 1
}

$rootFull = [System.IO.Path]::GetFullPath($root)
Write-Host ""
Write-Host "  官网本地预览已启动（只读、仅本机）" -ForegroundColor Green
Write-Host "  ─────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host "    官网首页     ${prefix}index.html"
Write-Host "    在线体验     ${prefix}demo.html"
Write-Host "    在线脚本工坊 ${prefix}export/export.html"
Write-Host "    导出链路自检 ${prefix}export/selftest.html" -ForegroundColor Cyan
Write-Host "  ─────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host "  按 Ctrl+C 停止。" -ForegroundColor DarkGray
Write-Host ""

if (-not $NoBrowser) {
    try { Start-Process "${prefix}export/selftest.html" | Out-Null } catch {}
}

try {
    while ($listener.IsListening) {
        $ctx = $listener.GetContext()
        $req = $ctx.Request
        $res = $ctx.Response
        try {
            $rel = [System.Uri]::UnescapeDataString($req.Url.AbsolutePath)
            if ($rel.EndsWith('/')) { $rel += 'index.html' }
            $full = [System.IO.Path]::GetFullPath((Join-Path $rootFull $rel.TrimStart('/')))

            # 目录逃逸防护（与线上 nginx 的 root 语义一致）
            if (-not $full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
                $res.StatusCode = 403
                $res.Close()
                continue
            }
            if (-not [System.IO.File]::Exists($full)) {
                $res.StatusCode = 404
                # ⚠ HEAD 请求**不能带响应体**：HttpListener 会抛
                #   "Cannot send a content-body with this verb-type"，
                #   异常被下面的 catch 接住就变成 500（客户端看到 500 而不是 404）。
                if ($req.HttpMethod -ne 'HEAD') {
                    $body = [System.Text.Encoding]::UTF8.GetBytes("404 $rel")
                    $res.ContentType = 'text/plain; charset=utf-8'
                    $res.ContentLength64 = $body.Length
                    $res.OutputStream.Write($body, 0, $body.Length)
                }
                $res.Close()
                Write-Host "  404 $rel" -ForegroundColor DarkYellow
                continue
            }

            $ext = [System.IO.Path]::GetExtension($full).ToLowerInvariant()
            $res.ContentType = if ($mime.ContainsKey($ext)) { $mime[$ext] } else { 'application/octet-stream' }
            $res.Headers['Cache-Control'] = 'no-store'
            $bytes = [System.IO.File]::ReadAllBytes($full)
            $res.ContentLength64 = $bytes.Length
            if ($req.HttpMethod -ne 'HEAD') {
                $res.OutputStream.Write($bytes, 0, $bytes.Length)
            }
            $res.Close()
        } catch {
            try { $res.StatusCode = 500; $res.Close() } catch {}
        }
    }
} finally {
    $listener.Stop()
    $listener.Close()
    Write-Host "`n  已停止。" -ForegroundColor DarkGray
}
