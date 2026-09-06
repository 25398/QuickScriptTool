# package_webview_portable.ps1 — 便携 zip：解压即用，无需安装 WebView2 Runtime
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\package_webview_portable.ps1 [-SkipBuild] [-SkipFetch]

param(
    [switch]$SkipBuild,
    [switch]$SkipFetch
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) {
    throw "FATAL: PSScriptRoot empty"
}
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build"
$ReleaseDir = Join-Path $BuildDir "Release"
$DistRoot = Join-Path $RepoRoot "dist"
$StageDir = Join-Path $DistRoot "QstWebViewShell-Portable"
$ZipPath = Join-Path $DistRoot "QstWebViewShell-Portable.zip"
$Cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Fetch = Join-Path $PSScriptRoot "fetch_webview2_fixed.ps1"
$VersionFile = Join-Path $PSScriptRoot "webview2_fixed_version.txt"

$verLine = (Get-Content -LiteralPath $VersionFile | ForEach-Object { $_.Trim() } |
    Where-Object { $_ -and -not $_.StartsWith("#") } | Select-Object -First 1)
$FixedCache = Join-Path $RepoRoot "third_party\webview2_fixed\$verLine\WebView2Fixed"

if (-not $SkipFetch) {
    Write-Host "Fetching Fixed Version Runtime..."
    & $Fetch
    if (-not $?) { throw "fetch_webview2_fixed.ps1 failed" }
}

if (-not (Test-Path -LiteralPath (Join-Path $FixedCache "msedgewebview2.exe"))) {
    $hit = Get-ChildItem -LiteralPath $FixedCache -Filter "msedgewebview2.exe" -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $hit) { throw "Fixed Runtime missing at $FixedCache — run tools\fetch_webview2_fixed.ps1" }
}

if (-not $SkipBuild) {
    if (-not (Test-Path -LiteralPath $Cmake)) { throw "CMake not found: $Cmake" }
    Write-Host "Building QstWebViewShell Release..."
    & $Cmake -S $RepoRoot -B $BuildDir -DQST_BUILD_WEBVIEW_SHELL=ON -DQST_WEBVIEW_BUNDLE_FIXED=ON -DQST_WEBVIEW_ALLOW_EVERGREEN=OFF
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    & $Cmake --build $BuildDir --config Release --target QstWebViewShell -- /m /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$exe = Join-Path $ReleaseDir "QstWebViewShell.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing $exe" }

Write-Host "Staging portable tree: $StageDir"
if (Test-Path -LiteralPath $StageDir) { Remove-Item -LiteralPath $StageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "ui") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "WebView2Fixed") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "WebView2UserData") | Out-Null

Copy-Item -LiteralPath $exe -Destination $StageDir -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "ui\index.html") -Destination (Join-Path $StageDir "ui") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "ui\agent.html") -Destination (Join-Path $StageDir "ui") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "ui\agent.css") -Destination (Join-Path $StageDir "ui") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "ui\shell.css") -Destination (Join-Path $StageDir "ui") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "ui\bridge.js") -Destination (Join-Path $StageDir "ui") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "ui\app.js") -Destination (Join-Path $StageDir "ui") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "ui\pro-mode.css") -Destination (Join-Path $StageDir "ui") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "ui\pro-mode.js") -Destination (Join-Path $StageDir "ui") -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "ui\app_icon.ico") -Destination (Join-Path $StageDir "ui") -Force
$vendorSrc = Join-Path $RepoRoot "ui\vendor"
if (Test-Path -LiteralPath $vendorSrc) {
  Copy-Item -LiteralPath $vendorSrc -Destination (Join-Path $StageDir "ui\vendor") -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $RepoRoot "resources\app_icon.ico") -Destination $StageDir -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot "resources\tray_running.ico") -Destination $StageDir -Force

# Agent Skills（文件化 Skill：对话编辑 / 撤销 / 命令行），随包分发到 skills\agent\
$skillsStage = Join-Path $StageDir "skills\agent"
New-Item -ItemType Directory -Force -Path $skillsStage | Out-Null
foreach ($skillName in @("agent-conversation", "agent-revert", "agent-shell", "agent-script", "agent-optimize")) {
    $skillSrc = Join-Path $RepoRoot (Join-Path ".cursor\skills" (Join-Path $skillName "SKILL.md"))
    if (Test-Path -LiteralPath $skillSrc) {
        $section = $skillName -replace "^agent-", ""
        if ($skillName -eq "agent-script") { $section = "scriptstrategy" }
        Copy-Item -LiteralPath $skillSrc -Destination (Join-Path $skillsStage ($section + ".md")) -Force
    }
}

# Prefer POST_BUILD copy if present; else cache
$builtFixed = Join-Path $ReleaseDir "WebView2Fixed"
$srcFixed = if (Test-Path -LiteralPath (Join-Path $builtFixed "msedgewebview2.exe")) { $builtFixed } else { $FixedCache }
Write-Host "Copying Fixed Runtime from $srcFixed ..."
& robocopy $srcFixed (Join-Path $StageDir "WebView2Fixed") /E /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy WebView2Fixed failed: $LASTEXITCODE" }

# Re-apply ACLs inside stage (zip may drop them; also help Win10)
$fixedOut = Join-Path $StageDir "WebView2Fixed"
icacls $fixedOut /grant "*S-1-15-2-1:(OI)(CI)(RX)" | Out-Null
icacls $fixedOut /grant "*S-1-15-2-2:(OI)(CI)(RX)" | Out-Null

$readmePath = Join-Path $StageDir "README-portable.txt"
$readme = @"
键鼠工坊 · WebView 壳便携版
========================

产品主路径 = Web UI + C++ 引擎（本包）。解压后运行 exe 即可，无需安装 WebView2 / Edge；请整夹一起拷贝，不要只拷 exe。
本包不包含、也不需要 QuickScriptTool.Gdi.exe（旧 GDI 界面仅为开发可选逃生舱）。

目录说明：
  QstWebViewShell.exe   — 启动程序（产品）
  ui\                   — 界面资源（勿删）
  WebView2Fixed\        — 内置 WebView2 Fixed Runtime（勿删）
  WebView2UserData\     — 运行时用户数据（可删，下次自动重建）

Fixed Runtime 版本：$verLine
"@
Set-Content -LiteralPath $readmePath -Value $readme -Encoding UTF8
if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
Write-Host "Compressing $ZipPath ..."
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -CompressionLevel Optimal

$zipSize = (Get-Item -LiteralPath $ZipPath).Length
$fixedSize = (Get-ChildItem -LiteralPath $fixedOut -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host "OK"
Write-Host "  stage: $StageDir"
Write-Host "  zip:   $ZipPath"
Write-Host ("  zip bytes: {0:N0} (~{1:N1} MB)" -f $zipSize, ($zipSize / 1MB))
Write-Host ("  WebView2Fixed bytes: {0:N0} (~{1:N1} MB)" -f $fixedSize, ($fixedSize / 1MB))
Write-Host "  runtime version: $verLine"
