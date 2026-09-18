# test_mcp_server.ps1 — QuickScriptTool 作为 MCP server 的端到端冒烟
#
# 用法: powershell -ExecutionPolicy Bypass -File tools\test_mcp_server.ps1 [-Exe <路径>]
# 退出码 0 = 全部通过。
#
# 为什么用脚本而不是 C++ 自检：MCP 模式必须**真的把产品 exe 当子进程拉起来**、
# 走管道说话才有意义（协议层是 stdio 一行一个 JSON-RPC）。C++ 自检里链接整套
# window_mode/输入库代价大，这里用真 exe 反而更接近真实客户端（DSH/Claude 等）。
param(
    [string]$Exe = ""
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Exe)) { $Exe = Join-Path $repo 'build\Release\QuickScriptTool.exe' }
if (-not (Test-Path -LiteralPath $Exe)) { throw "找不到 exe：$Exe（先构建 QstWebViewShell）" }

$fail = 0
function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) { Write-Host ("  [OK]   " + $name + $(if ($detail) { " — " + $detail } else { "" })) }
    else { Write-Host ("  [FAIL] " + $name + $(if ($detail) { " — " + $detail } else { "" })) -ForegroundColor Red; $script:fail++ }
}

$requests = @(
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}}',
    '{"jsonrpc":"2.0","method":"notifications/initialized"}',
    '{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
    '{"jsonrpc":"2.0","id":3,"method":"ping"}',
    '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"cursor_position","arguments":{}}}',
    '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"screenshot","arguments":{"x1":0,"y1":0,"x2":160,"y2":120}}}',
    '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"no_such_tool","arguments":{}}}',
    '{"jsonrpc":"2.0","id":7,"method":"no/such/method"}',
    'this is not json'
)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = (Resolve-Path -LiteralPath $Exe).Path
$psi.Arguments = '--mcp'
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$proc = [System.Diagnostics.Process]::Start($psi)
foreach ($r in $requests) { $proc.StandardInput.WriteLine($r) }
$proc.StandardInput.Close()
$stdout = $proc.StandardOutput.ReadToEnd()
$proc.WaitForExit(60000) | Out-Null

Write-Host "== MCP 冒烟：$Exe --mcp（退出码 $($proc.ExitCode)）"
Check '进程正常退出' ($proc.ExitCode -eq 0) "exit=$($proc.ExitCode)"

$responses = @()
foreach ($line in ($stdout -split "`n")) {
    if ($line.Trim().Length -gt 0) { $responses += ($line.Trim() | ConvertFrom-Json) }
}
# 通知不应有响应：9 条请求里 1 条是通知 → 期望 8 条响应
Check '响应条数=请求-通知' ($responses.Count -eq 8) "实际 $($responses.Count)"

$byId = @{}
foreach ($r in $responses) { if ($null -ne $r.id) { $byId[[string]$r.id] = $r } }

$init = $byId['1']
Check 'initialize 返回 serverInfo' ($init.result.serverInfo.name -eq 'quickscripttool') `
    ($init.result.serverInfo.title)
Check 'initialize 协商协议版本' ($init.result.protocolVersion -eq '2025-06-18') $init.result.protocolVersion

$tools = @($byId['2'].result.tools)
$names = $tools | ForEach-Object { $_.name }
Check 'tools/list 非空' ($tools.Count -ge 8) "工具数=$($tools.Count)"
foreach ($need in @('screenshot', 'click', 'move', 'type_text', 'key', 'scroll',
        'cursor_position', 'list_windows', 'activate_window', 'list_ui_controls',
        'invoke_ui_control', 'read_document')) {
    Check ("工具存在: " + $need) ($names -contains $need)
}
Check '每个工具都有 inputSchema' (@($tools | Where-Object { -not $_.inputSchema }).Count -eq 0)

Check 'ping 返回空结果' ($null -ne $byId['3'].result) ''
$cursor = $byId['4'].result.content[0].text
Check 'cursor_position 返回坐标' ($cursor -match '光标屏幕坐标') $cursor

$shot = $byId['5'].result
$img = @($shot.content | Where-Object { $_.type -eq 'image' })
Check 'screenshot 返回图片' ($img.Count -eq 1 -and $img[0].mimeType -like 'image/*') `
    $(if ($img.Count -eq 1) { "$($img[0].mimeType) base64=$($img[0].data.Length)" } else { "无图片" })
Check 'screenshot 图片是合法 base64' ($img.Count -eq 1 -and $img[0].data.Length -gt 100) ''
Check 'screenshot 附带尺寸说明' ($shot.content[1].text -match '\d+×\d+ 像素') $shot.content[1].text

Check '未知工具 → isError' ($byId['6'].result.isError -eq $true) $byId['6'].result.content[0].text
Check '未知方法 → -32601' ($byId['7'].error.code -eq -32601) $byId['7'].error.message
$parseErr = $responses | Where-Object { $_.error.code -eq -32700 }
Check '非法 JSON → -32700' ($null -ne $parseErr) ''

Write-Host ""
if ($fail -eq 0) { Write-Host "MCP 冒烟全部通过。" -ForegroundColor Green; exit 0 }
Write-Host "$fail 项失败。" -ForegroundColor Red
exit 1
