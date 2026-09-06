# Package QuickScriptTool Release build into dist/
# Usage: powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 [-SkipBuild]
#
# ---------------------------------------------------------------------------
# 发版依赖策略（硬规则）
# ---------------------------------------------------------------------------
# 凡运行主程序必需的依赖，必须打进 dist\QuickScriptTool（→ zip + 安装包）。
# 缺则 FATAL，禁止“让用户自己去官网下运行库”。
#
# 【必须随包】（旁路 / 内置，解压或安装后即可用）
#   - QuickScriptTool.exe / ui\ / WebView2Fixed\     （主程序 + 界面 + Fixed Runtime）
#   - MSVC x64 CRT：vcruntime140*.dll / msvcp140*.dll 等（exe 同目录旁路）
#   - opencv_world*.dll、FakeFocus*.dll、VirtualDesktopAccessor*.dll
#   - interception.dll                                （用户态 HID 库；内核驱动走设置里安全安装脚本）
#   - driver\qst_vhid\                                （VHID 安装链随包；装驱动走设置按钮）
#   - extension\edge\、skills\agent\、OCR helper 脚本
#
# 【允许不随包 / 运行时或设置里安装】（软件内有安装按钮或首次用时再装）
#   - Interception / 虚拟 HID 内核驱动本身（用户点「安装驱动」；失败则回滚，不改启动配置）
#   - Python 3.12 + PaddleOCR 环境（设置里 OCR「一键安装」；可选预置 tools\python* 做离线）
#   - 系统 Evergreen WebView2（便携包用 WebView2Fixed，不要求用户装）
#
# 版本号：tools\product_version.txt（与 AppBranding / Inno MyAppVersion 同步改）
# Edge 扩展：先校验/打 zip（tools\pack_edge_extension.ps1），再拷入 dist。
# 规范见 extension\PACKAGING.md — 漏扩展视为发版失败。
# ---------------------------------------------------------------------------

param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
# Prefer PSScriptRoot (stable). Avoid $Root — pack_edge_extension.ps1 assigns $Root.
if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) {
    throw "FATAL: PSScriptRoot is empty; run via powershell -File tools\package_release.ps1"
}
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($RepoRoot) -or -not (Test-Path -LiteralPath (Join-Path $RepoRoot "CMakeLists.txt"))) {
    throw "FATAL: cannot resolve repo root from PSScriptRoot: $PSScriptRoot"
}
$BuildDir = Join-Path $RepoRoot "build"
$ReleaseDir = Join-Path $BuildDir "Release"
$DistRoot = Join-Path $RepoRoot "dist"
$DistDir = Join-Path $DistRoot "QuickScriptTool"
$Cmake = $null
foreach ($cand in @(
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${env:ProgramFiles}\CMake\bin\cmake.exe"
)) {
    if ($cand -and (Test-Path -LiteralPath $cand)) { $Cmake = $cand; break }
}
if (-not $Cmake) {
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { $Cmake = $cmd.Source }
}
$PackExt = Join-Path $RepoRoot "tools\pack_edge_extension.ps1"
$VersionFile = Join-Path $RepoRoot "tools\product_version.txt"

function Get-ProductVersion {
    if (-not (Test-Path -LiteralPath $VersionFile)) {
        throw "FATAL: missing $VersionFile (expected e.g. 1.0.3)"
    }
    $line = Get-Content -LiteralPath $VersionFile -ErrorAction Stop |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith("#") } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($line)) { throw "FATAL: product_version.txt empty" }
    if ($line -notmatch '^\d+\.\d+\.\d+') {
        throw "FATAL: product_version.txt must look like 1.0.3, got: $line"
    }
    return $line
}

# 从本机 VS 2022 Redist 解析 x64 CRT 目录（旁路部署到 exe 旁，用户无需装 vc_redist）
function Resolve-Vc143CrtX64Dir {
    $vsRoots = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Redist\MSVC",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\VC\Redist\MSVC",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"
    )
    foreach ($root in $vsRoots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $hit = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object {
                $crt = Join-Path $_.FullName "x64\Microsoft.VC143.CRT"
                if (-not (Test-Path -LiteralPath $crt)) {
                    $crt = Get-ChildItem -LiteralPath (Join-Path $_.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
                        Select-Object -First 1 -ExpandProperty FullName
                }
                if ($crt -and (Test-Path -LiteralPath (Join-Path $crt "vcruntime140.dll"))) { $crt }
            } |
            Select-Object -First 1
        if ($hit) { return $hit }
    }
    return $null
}

function Copy-VcRuntimeAppLocal {
    param([Parameter(Mandatory = $true)][string]$DestDir)

    # 主程序 + OpenCV 动态链接这些 DLL；必须与 exe 同目录（app-local）
    $required = @(
        "vcruntime140.dll",
        "vcruntime140_1.dll",
        "msvcp140.dll"
    )
    $optionalExtras = @(
        "msvcp140_1.dll",
        "msvcp140_2.dll",
        "msvcp140_atomic_wait.dll",
        "msvcp140_codecvt_ids.dll",
        "concrt140.dll",
        "vccorlib140.dll",
        "vcruntime140_threads.dll"
    )

    $crtDir = Resolve-Vc143CrtX64Dir
    if (-not $crtDir) {
        throw @"
FATAL: 未找到 VS2022 VC Redist x64 CRT（Microsoft.VC143.CRT）。
发版机需安装 Visual Studio 2022（含 C++ 桌面开发 / VC 可再发行组件）。
查找路径示例: ...\VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT\
"@
    }
    Write-Host "  VC CRT source: $crtDir"

    foreach ($name in $required) {
        $src = Join-Path $crtDir $name
        if (-not (Test-Path -LiteralPath $src)) {
            throw "FATAL: required VC runtime missing: $src"
        }
        Copy-Item -LiteralPath $src -Destination (Join-Path $DestDir $name) -Force
        Write-Host "  + $name (MSVC CRT app-local)"
    }
    foreach ($name in $optionalExtras) {
        $src = Join-Path $crtDir $name
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $DestDir $name) -Force
            Write-Host "  + $name (MSVC CRT app-local)"
        }
    }

    foreach ($name in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $DestDir $name))) {
            throw "FATAL: VC runtime not in dist after copy: $name"
        }
    }
}

$ProductVersion = Get-ProductVersion
Write-Host "Product version: $ProductVersion (from tools\product_version.txt)"

Write-Host "Validating + packing Edge extension..."
& $PackExt -OutDir $DistRoot
# pack_edge uses return (not exit) so callers keep variables; trust $? not LASTEXITCODE
if (-not $?) { throw "Edge extension pack failed (see extension\PACKAGING.md)." }

if (-not $SkipBuild) {
    Write-Host "Building Release (QstWebViewShell)..."
    if (-not (Test-Path $Cmake)) {
        throw "CMake not found. Install Visual Studio 2022 with CMake."
    }
    & $Cmake --build $BuildDir --config Release --target QstWebViewShell -j 16
    if ($LASTEXITCODE -ne 0) { throw "QstWebViewShell build failed." }
} else {
    Write-Host "SkipBuild: using existing $ReleaseDir"
    if (-not (Test-Path (Join-Path $ReleaseDir "QstWebViewShell.exe"))) {
        throw "SkipBuild but QstWebViewShell.exe missing in $ReleaseDir"
    }
}

Write-Host "Preparing dist folder: $DistDir"
if (Test-Path $DistDir) { Remove-Item $DistDir -Recurse -Force }
New-Item -ItemType Directory -Path $DistDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "tools") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "scripts") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "recordings") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "extension") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "ui") | Out-Null

# Primary product entry = WebView shell + full engine (same mutex / scripts dir)
$ShellExe = Join-Path $ReleaseDir "QstWebViewShell.exe"
if (-not (Test-Path $ShellExe)) { throw "Missing $ShellExe" }
Copy-Item $ShellExe (Join-Path $DistDir "QuickScriptTool.exe") -Force
Write-Host "  + QuickScriptTool.exe (WebView UI + engine)"
Copy-Item $ShellExe (Join-Path $DistDir "QstWebViewShell.exe") -Force

# Product readme (ASCII-only literals for Windows PowerShell 5 encoding safety)
$readmeProduct = @"
QuickScriptTool $ProductVersion (Release)
================================

Main: QuickScriptTool.exe = WebView UI + C++ engine.
Unpack the WHOLE folder (ui\ + WebView2Fixed\ + *.dll next to exe). Do not copy exe alone.

Bundled (no extra download):
  - WebView2 Fixed Runtime (WebView2Fixed\)
  - MSVC x64 CRT (vcruntime140*.dll / msvcp140*.dll next to exe)
  - OpenCV / FakeFocus / VirtualDesktopAccessor / Edge extension / VHID install scripts

Optional in-app install (settings buttons):
  - Interception / Virtual HID kernel drivers
  - Python + PaddleOCR (OCR one-click install)
"@
Set-Content -LiteralPath (Join-Path $DistDir "README.txt") -Value $readmeProduct -Encoding UTF8
Write-Host "  + README.txt (product=Web)"

$diagnoseSrc = Join-Path $RepoRoot "tools\diagnose_start.cmd"
if (Test-Path -LiteralPath $diagnoseSrc) {
    # 同时放 ASCII 名：部分环境对 zip 内中文文件名不友好
    Copy-Item -LiteralPath $diagnoseSrc -Destination (Join-Path $DistDir "diagnose_start.cmd") -Force
    Copy-Item -LiteralPath $diagnoseSrc -Destination (Join-Path $DistDir "启动诊断.cmd") -Force
    Write-Host "  + diagnose_start.cmd / 启动诊断.cmd"
}

# MSVC CRT 旁路：安装包与 zip 共用此目录，目标机无需再装 vc_redist
Copy-VcRuntimeAppLocal -DestDir $DistDir

# WebView UI + Fixed Runtime (portable, no system WebView2 install)
# -Recurse 一次拷齐；勿再把 ui\vendor 目录本身拷进已存在的 ui\vendor（会变成 vendor\vendor）
Copy-Item (Join-Path $RepoRoot "ui\*") (Join-Path $DistDir "ui") -Recurse -Force
foreach ($ico in @("app_icon.ico", "tray_running.ico", "breakout_pause.ico")) {
    $p = Join-Path $ReleaseDir $ico
    if (Test-Path $p) { Copy-Item $p $DistDir -Force }
}
$fixedSrc = Join-Path $ReleaseDir "WebView2Fixed"
if (Test-Path (Join-Path $fixedSrc "msedgewebview2.exe")) {
    Copy-Item $fixedSrc (Join-Path $DistDir "WebView2Fixed") -Recurse -Force
    # Win10 Fixed≥120：AppContainer 需 RX；zip 可能丢 ACL，安装目录仍尽量带上
    $fixedOut = Join-Path $DistDir "WebView2Fixed"
    icacls $fixedOut /grant "*S-1-15-2-1:(OI)(CI)(RX)" | Out-Null
    icacls $fixedOut /grant "*S-1-15-2-2:(OI)(CI)(RX)" | Out-Null
    Write-Host "  + WebView2Fixed\"
} else {
    throw "FATAL: WebView2Fixed missing next to shell — run tools\fetch_webview2_fixed.ps1 / rebuild QstWebViewShell"
}
New-Item -ItemType Directory -Path (Join-Path $DistDir "WebView2UserData") -Force | Out-Null

Copy-Item (Join-Path $ReleaseDir "tools\paddle_ocr_helper.py") (Join-Path $DistDir "tools") -ErrorAction SilentlyContinue
Copy-Item (Join-Path $ReleaseDir "tools\requirements-ocr.txt") (Join-Path $DistDir "tools") -ErrorAction SilentlyContinue

# Agent Skills（文件化 Skill：对话编辑 / 撤销 / 命令行），随包分发到 skills\agent\
$skillsStage = Join-Path $DistDir "skills\agent"
New-Item -ItemType Directory -Force -Path $skillsStage | Out-Null
foreach ($skillName in @("agent-conversation", "agent-revert", "agent-shell", "agent-script", "agent-optimize")) {
    $skillSrc = Join-Path $RepoRoot (Join-Path ".cursor\skills" (Join-Path $skillName "SKILL.md"))
    if (Test-Path -LiteralPath $skillSrc) {
        $section = $skillName -replace "^agent-", ""
        if ($skillName -eq "agent-script") { $section = "scriptstrategy" }
        Copy-Item -LiteralPath $skillSrc -Destination (Join-Path $skillsStage ($section + ".md")) -Force
    }
}

# Runtime companions (window-mode / fake-focus); same names as CMake post-build copies
foreach ($dllName in @(
    "FakeFocus64.dll",
    "FakeFocus32.dll",
    "VirtualDesktopAccessor10.dll",
    "VirtualDesktopAccessor11.dll",
    "VirtualDesktopAccessor11_23h2.dll"
)) {
    $dllPath = Join-Path $ReleaseDir $dllName
    if (Test-Path -LiteralPath $dllPath) {
        Copy-Item -LiteralPath $dllPath -Destination $DistDir -Force
        Write-Host "  + $dllName"
    } else {
        throw "FATAL: missing required runtime DLL: $dllName (rebuild Release)"
    }
}

# 始终以仓库源码 extension\edge 为准（避免 build 副本过期/损坏）
# Re-resolve from PSScriptRoot — avoids any accidental clobber of $RepoRoot above.
if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) {
    throw "FATAL: PSScriptRoot is empty; cannot locate extension\\edge"
}
$RepoRoot = Split-Path -Parent $PSScriptRoot
$EdgeExtensionDir = Join-Path $RepoRoot "extension\edge"
$EdgeManifestPath = Join-Path $EdgeExtensionDir "manifest.json"
Write-Host "Repo root: $RepoRoot"
Write-Host "Edge src:  $EdgeExtensionDir"
if (-not (Test-Path -LiteralPath $EdgeManifestPath)) {
    throw "FATAL: extension\edge\manifest.json missing under $RepoRoot. Cannot ship without Edge bridge."
}
Copy-Item -LiteralPath $EdgeExtensionDir -Destination (Join-Path $DistDir 'extension\edge') -Recurse -Force
Write-Host "  + extension\edge (browser companion, required)"
$manifestCheck = Join-Path $DistDir 'extension\edge\manifest.json'
if (-not (Test-Path -LiteralPath $manifestCheck)) {
    throw "FATAL: dist copy of extension\edge failed."
}

# OCR 离线包：可选。有则拷入；无则用户走设置「一键安装」（在线下载）——不属于强制随包依赖
$pythonInstaller = Join-Path $RepoRoot "tools\python-3.12.10-amd64.exe"
if (Test-Path $pythonInstaller) {
    Copy-Item $pythonInstaller (Join-Path $DistDir "tools")
    Write-Host "  + python-3.12.10-amd64.exe (offline OCR install)"
} else {
    Write-Host "  (optional/runtime) Python installer not bundled — OCR uses in-app download. Run tools\download_python312.ps1 for offline."
}

$pythonPortable = Join-Path $RepoRoot "tools\python312"
if (Test-Path (Join-Path $pythonPortable "python.exe")) {
    Copy-Item $pythonPortable (Join-Path $DistDir "tools\python312") -Recurse
    Write-Host "  + tools\python312\ (portable Python for offline OCR install)"
}

$opencv = Get-ChildItem $ReleaseDir -Filter "opencv_world*.dll" |
    Where-Object { $_.Name -notmatch 'd\.dll$' }
if ($opencv.Count -eq 0) {
    throw "FATAL: Release opencv_world*.dll not found in $ReleaseDir"
}
foreach ($dll in $opencv) {
    Copy-Item $dll.FullName $DistDir
    Write-Host "  + $($dll.Name)"
}

$interceptionDll = Join-Path $ReleaseDir "interception.dll"
if (-not (Test-Path $interceptionDll)) {
    $interceptionDll = Join-Path $RepoRoot "third_party\interception\x64\interception.dll"
}
if (Test-Path $interceptionDll) {
    Copy-Item $interceptionDll $DistDir
    Write-Host "  + interception.dll (HID driver-level sim user-mode)"
} else {
    throw "FATAL: interception.dll missing — required companion (driver install is in-app)"
}

# 官方 install-interception.exe 会在驱动未加载成功时写入键盘/鼠标 LowerFilters，导致无法开机。
# 禁止打进发版包；设置按钮只跑 driver\qst_vhid\_elevate_install.ps1。
if (Test-Path (Join-Path $DistDir "install-interception.exe")) {
    Remove-Item (Join-Path $DistDir "install-interception.exe") -Force
    Write-Host "  - stripped install-interception.exe (unsafe class-filter installer, not shipped)"
}

# QstVHid driver package (product installer never flips boot policy / HVCI)
# Target machine: NO WDK/SDK. Settings UI runs _elevate_install.ps1.
# Required layout:
#   driver\qst_vhid\_elevate_install.ps1
#   driver\qst_vhid\repair_boot.ps1
#   driver\qst_vhid\package\{QstVHid.sys, qst_vhid.inf, qst_vhid.cat, interception.sys}
# Lab-only (do NOT ship): sign_and_install.ps1, import_certs.reg, install_fake_driver.ps1, portable\
$driverSrc = Join-Path $RepoRoot "driver\qst_vhid"
$driverDst = Join-Path $DistDir "driver\qst_vhid"
$pkgSrc = Join-Path $driverSrc "package"
$requiredDriverFiles = @(
    (Join-Path $driverSrc "_elevate_install.ps1"),
    (Join-Path $driverSrc "repair_boot.ps1"),
    (Join-Path $pkgSrc "QstVHid.sys"),
    (Join-Path $pkgSrc "qst_vhid.inf"),
    (Join-Path $pkgSrc "qst_vhid.cat"),
    (Join-Path $pkgSrc "interception.sys")
)
$missingDriver = @($requiredDriverFiles | Where-Object { -not (Test-Path $_) })
if ($missingDriver.Count -gt 0) {
    throw ("FATAL: VHID install chain incomplete. Missing:`n  " + ($missingDriver -join "`n  ") + `
        "`nPrepare driver\qst_vhid\package\ first (see driver\qst_vhid\README.md).")
}
New-Item -ItemType Directory -Path $driverDst -Force | Out-Null
Copy-Item (Join-Path $driverSrc "_elevate_install.ps1") $driverDst -Force
Copy-Item (Join-Path $driverSrc "repair_boot.ps1") $driverDst -Force
Copy-Item $pkgSrc (Join-Path $driverDst "package") -Recurse -Force
# Lab-only assets must never ship (Root CA import / clock rollback / fake-sign).
$forbidShip = @(
    (Join-Path $driverDst "sign_and_install.ps1"),
    (Join-Path $driverDst "import_certs.reg"),
    (Join-Path $driverDst "install_fake_driver.ps1"),
    (Join-Path $driverDst "QstVHidDev.cer"),
    (Join-Path $driverDst "portable"),
    (Join-Path $DistDir "install-interception.exe")
)
foreach ($f in $forbidShip) {
    if (Test-Path -LiteralPath $f) {
        Remove-Item -LiteralPath $f -Recurse -Force
        Write-Host "  - stripped lab/unsafe installer: $f"
    }
}
# Refuse stale flat layout leftovers (old dist used driver\*.sys without qst_vhid\)
$staleFlat = @(
    (Join-Path $DistDir "driver\QstVHid.sys"),
    (Join-Path $DistDir "driver\qst_vhid.inf")
) | Where-Object { Test-Path $_ }
foreach ($f in $staleFlat) { Remove-Item $f -Force }
$verifyDriver = @(
    (Join-Path $driverDst "_elevate_install.ps1"),
    (Join-Path $driverDst "repair_boot.ps1"),
    (Join-Path $driverDst "package\QstVHid.sys"),
    (Join-Path $driverDst "package\qst_vhid.inf"),
    (Join-Path $driverDst "package\qst_vhid.cat"),
    (Join-Path $driverDst "package\interception.sys")
) | Where-Object { -not (Test-Path $_) }
if ($verifyDriver.Count -gt 0) {
    throw ("FATAL: dist VHID copy incomplete:`n  " + ($verifyDriver -join "`n  "))
}
if (Test-Path (Join-Path $DistDir "install-interception.exe")) {
    throw "FATAL: install-interception.exe must not ship (class-filter brick risk)"
}
if (Test-Path (Join-Path $driverDst "sign_and_install.ps1")) {
    throw "FATAL: sign_and_install.ps1 must not ship (lab signing / Root CA import)"
}
Write-Host "  + driver/qst_vhid/ (safe elevate install + repair_boot + package)"

# 最终门禁：主程序旁关键文件
$gate = @(
    (Join-Path $DistDir "QuickScriptTool.exe"),
    (Join-Path $DistDir "ui\index.html"),
    (Join-Path $DistDir "WebView2Fixed\msedgewebview2.exe"),
    (Join-Path $DistDir "vcruntime140.dll"),
    (Join-Path $DistDir "vcruntime140_1.dll"),
    (Join-Path $DistDir "msvcp140.dll"),
    (Join-Path $DistDir "extension\edge\manifest.json")
) | Where-Object { -not (Test-Path -LiteralPath $_) }
if ($gate.Count -gt 0) {
    throw ("FATAL: release gate failed, missing:`n  " + ($gate -join "`n  "))
}

$zipName = "QuickScriptTool-Release-$ProductVersion.zip"
$zipPath = Join-Path $DistRoot $zipName
$zipAlias = Join-Path $DistRoot "QuickScriptTool-Release.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
if (Test-Path $zipAlias) { Remove-Item $zipAlias -Force }
try {
    # 勿用 tar -a：条目带 "./" 前缀时，资源管理器会报「压缩文件夹无效」(Shell.Items=0)。
    # .NET ZipFile 生成的 zip 可被 Windows 自带解压打开。
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $DistDir, $zipPath,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false)
    if (-not (Test-Path -LiteralPath $zipPath)) { throw "ZipFile produced no zip" }
    # 门禁：资源管理器必须能枚举到条目
    $shell = New-Object -ComObject Shell.Application
    $zipNs = $shell.NameSpace($zipPath)
    $itemCount = 0
    if ($zipNs) { $itemCount = @($zipNs.Items()).Count }
    if ($itemCount -lt 1) {
        throw "FATAL: zip not readable by Windows Explorer (Shell.Items=$itemCount). Do not ship."
    }
    Write-Host "  Zip OK (Explorer items=$itemCount): $zipPath"
    Copy-Item -LiteralPath $zipPath -Destination $zipAlias -Force
} catch {
    Write-Warning ("Zip failed (close QuickScriptTool if running, then re-run). Folder package is ready. " + $_.Exception.Message)
    $zipPath = $null
}

function Copy-AtomicFile {
    param([string]$From, [string]$To)
    $dir = Split-Path -Parent $To
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $tmp = Join-Path $dir ((Split-Path -Leaf $To) + ".partial")
    Copy-Item -LiteralPath $From -Destination $tmp -Force
    Move-Item -LiteralPath $tmp -Destination $To -Force
}

# 同步官网静态下载目录（部署 website/ 即可直接点按钮下载）
$WebDl = Join-Path $RepoRoot "website\downloads"
New-Item -ItemType Directory -Path $WebDl -Force | Out-Null
$SetupName = "QuickScriptTool-Setup-$ProductVersion.exe"
$SetupAlias = "QuickScriptTool-$ProductVersion.exe"
# ISCC 当前输出 QuickScriptTool-<ver>.exe；Setup- 只做旧链接别名
$SetupSrc = Join-Path $DistRoot $SetupAlias
if (-not (Test-Path -LiteralPath $SetupSrc)) {
    $SetupSrc = Join-Path $DistRoot $SetupName
}
# 官网按钮用固定别名（无版本号），发版只需覆盖文件，不必改 HTML
$StableZip = "QuickScriptTool-Release.zip"
$StableSetup = "QuickScriptTool-Setup.exe"
if ($zipPath -and (Test-Path -LiteralPath $zipPath)) {
    Copy-AtomicFile $zipPath (Join-Path $WebDl $zipName)
    Copy-AtomicFile $zipPath (Join-Path $WebDl $StableZip)
    Write-Host "  Website: website\downloads\$zipName"
    Write-Host "  Website: website\downloads\$StableZip  (stable link)"
}
if (Test-Path -LiteralPath $SetupSrc) {
    Copy-AtomicFile $SetupSrc (Join-Path $WebDl $SetupName)
    Copy-AtomicFile $SetupSrc (Join-Path $WebDl $SetupAlias)
    Copy-AtomicFile $SetupSrc (Join-Path $WebDl $StableSetup)
    if ((Split-Path -Leaf $SetupSrc) -ne $SetupName) {
        Copy-AtomicFile $SetupSrc (Join-Path $DistRoot $SetupName)
    }
    if ((Split-Path -Leaf $SetupSrc) -ne $SetupAlias) {
        Copy-AtomicFile $SetupSrc (Join-Path $DistRoot $SetupAlias)
    }
    Copy-AtomicFile $SetupSrc (Join-Path $DistRoot $StableSetup)
    Write-Host "  Website: website\downloads\$SetupName"
    Write-Host "  Website: website\downloads\$SetupAlias"
    Write-Host "  Website: website\downloads\$StableSetup  (stable link)"
} else {
    Write-Host "  (installer) $SetupName / $SetupAlias not built yet; compile installer\QuickScriptTool.iss then re-run -SkipBuild"
}

Write-Host ""
Write-Host "Done. version=$ProductVersion"
Write-Host "  Folder: $DistDir"
if ($zipPath) { Write-Host "  Zip:    $zipPath" }
$edgeZips = Get-ChildItem $DistRoot -Filter "QstEdgeBridge-*.zip" -ErrorAction SilentlyContinue
foreach ($z in $edgeZips) { Write-Host "  Edge:   $($z.FullName)" }
Write-Host ""
Write-Host "Product = QuickScriptTool.exe (WebView UI + engine). Gdi.exe is optional legacy only."
Write-Host "Do NOT ship build\Debug. Ship this Release package instead."
Write-Host "MSVC CRT is app-local next to exe — users need NOT install vc_redist.x64.exe."
Write-Host "Edge extension MUST be in package: extension\edge (see extension\PACKAGING.md)."
Write-Host "VHID / Interception installers are bundled; kernel drivers install via settings buttons."
Write-Host ""
Write-Host "For installer EXE:"
Write-Host "  1) confirm tools\product_version.txt == installer MyAppVersion"
Write-Host "  2) & `"`$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe`" installer\QuickScriptTool.iss"
Write-Host "  3) re-run this script with -SkipBuild to sync website\downloads\"
