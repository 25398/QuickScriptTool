# Package QuickScriptTool Release build into dist/
# Usage: powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 [-SkipBuild]
#
# 必含 Edge 配套扩展：先校验/打 zip（tools\pack_edge_extension.ps1），再拷入 dist。
# 规范见 extension\PACKAGING.md — 漏扩展视为发版失败。

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
$Cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$PackExt = Join-Path $RepoRoot "tools\pack_edge_extension.ps1"

Write-Host "Validating + packing Edge extension..."
& $PackExt -OutDir $DistRoot
# pack_edge uses return (not exit) so callers keep variables; trust $? not LASTEXITCODE
if (-not $?) { throw "Edge extension pack failed (see extension\PACKAGING.md)." }

if (-not $SkipBuild) {
    Write-Host "Building Release..."
    if (-not (Test-Path $Cmake)) {
        throw "CMake not found. Install Visual Studio 2022 with CMake."
    }
    & $Cmake --build $BuildDir --config Release --target QuickScriptTool -j 16
    if ($LASTEXITCODE -ne 0) { throw "Release build failed." }
} else {
    Write-Host "SkipBuild: using existing $ReleaseDir"
    if (-not (Test-Path (Join-Path $ReleaseDir "QuickScriptTool.exe"))) {
        throw "SkipBuild but QuickScriptTool.exe missing in $ReleaseDir"
    }
}

Write-Host "Preparing dist folder: $DistDir"
if (Test-Path $DistDir) { Remove-Item $DistDir -Recurse -Force }
New-Item -ItemType Directory -Path $DistDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "tools") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "scripts") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "recordings") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DistDir "extension") | Out-Null

Copy-Item (Join-Path $ReleaseDir "QuickScriptTool.exe") $DistDir
Copy-Item (Join-Path $ReleaseDir "tools\paddle_ocr_helper.py") (Join-Path $DistDir "tools")
Copy-Item (Join-Path $ReleaseDir "tools\requirements-ocr.txt") (Join-Path $DistDir "tools")

# Runtime companions (window-mode / fake-focus); same names as CMake post-build copies
foreach ($dllName in @(
    "FakeFocus64.dll",
    "FakeFocus32.dll",
    "VirtualDesktopAccessor10.dll",
    "VirtualDesktopAccessor11.dll"
)) {
    $dllPath = Join-Path $ReleaseDir $dllName
    if (Test-Path -LiteralPath $dllPath) {
        Copy-Item -LiteralPath $dllPath -Destination $DistDir -Force
        Write-Host "  + $dllName"
    } else {
        Write-Warning "missing optional runtime DLL: $dllName"
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

$pythonInstaller = Join-Path $RepoRoot "tools\python-3.12.10-amd64.exe"
if (Test-Path $pythonInstaller) {
    Copy-Item $pythonInstaller (Join-Path $DistDir "tools")
    Write-Host "  + python-3.12.10-amd64.exe (offline OCR install)"
} else {
    Write-Host "  (optional) Run tools\download_python312.ps1 to bundle Python for offline OCR install"
}

$pythonPortable = Join-Path $RepoRoot "tools\python312"
if (Test-Path (Join-Path $pythonPortable "python.exe")) {
    Copy-Item $pythonPortable (Join-Path $DistDir "tools\python312") -Recurse
    Write-Host "  + tools\python312\ (portable Python for offline OCR install)"
}

$opencv = Get-ChildItem $ReleaseDir -Filter "opencv_world*.dll" |
    Where-Object { $_.Name -notmatch 'd\.dll$' }
if ($opencv.Count -eq 0) {
    throw "Release opencv_world*.dll not found in $ReleaseDir"
}
foreach ($dll in $opencv) {
    Copy-Item $dll.FullName $DistDir
    Write-Host "  + $($dll.Name)"
}

$interceptionDll = Join-Path $ReleaseDir "interception.dll"
if (Test-Path $interceptionDll) {
    Copy-Item $interceptionDll $DistDir
    Write-Host "  + interception.dll (HID driver-level sim user-mode)"
} else {
    Write-Host "  (optional) interception.dll not in Release — HID sim will fall back to SendInput"
}

$interceptionInstaller = Join-Path $ReleaseDir "install-interception.exe"
if (-not (Test-Path $interceptionInstaller)) {
    $interceptionInstaller = Join-Path $RepoRoot "third_party\interception\install-interception.exe"
}
if (Test-Path $interceptionInstaller) {
    Copy-Item $interceptionInstaller (Join-Path $DistDir "install-interception.exe")
    Write-Host "  + install-interception.exe (one-click HID kernel driver installer; needs admin + reboot)"
} else {
    Write-Host "  (optional) install-interception.exe missing — settings '安装驱动' button will warn"
}

# QstVHid driver (expired EV cert sample + cert-store bypass for security lab)
# Target machine: NO WDK/SDK. Settings UI finds sign_and_install.ps1 then runs _elevate_install.ps1.
# Required layout (must match SettingsDialog::InstallVirtualHidDriver):
#   driver\qst_vhid\_elevate_install.ps1
#   driver\qst_vhid\sign_and_install.ps1
#   driver\qst_vhid\import_certs.reg
#   driver\qst_vhid\package\{QstVHid.sys, qst_vhid.inf, qst_vhid.cat}
$driverSrc = Join-Path $RepoRoot "driver\qst_vhid"
$driverDst = Join-Path $DistDir "driver\qst_vhid"
$pkgSrc = Join-Path $driverSrc "package"
$requiredDriverFiles = @(
    (Join-Path $driverSrc "_elevate_install.ps1"),
    (Join-Path $driverSrc "sign_and_install.ps1"),
    (Join-Path $driverSrc "import_certs.reg"),
    (Join-Path $pkgSrc "QstVHid.sys"),
    (Join-Path $pkgSrc "qst_vhid.inf"),
    (Join-Path $pkgSrc "qst_vhid.cat")
)
$missingDriver = @($requiredDriverFiles | Where-Object { -not (Test-Path $_) })
if ($missingDriver.Count -gt 0) {
    throw ("FATAL: VHID install chain incomplete. Missing:`n  " + ($missingDriver -join "`n  ") + `
        "`nPrepare package\ + import_certs.reg first (see driver\qst_vhid\README.md).")
}
New-Item -ItemType Directory -Path $driverDst -Force | Out-Null
Copy-Item (Join-Path $driverSrc "_elevate_install.ps1") $driverDst -Force
Copy-Item (Join-Path $driverSrc "sign_and_install.ps1") $driverDst -Force
Copy-Item (Join-Path $driverSrc "import_certs.reg") $driverDst -Force
if (Test-Path (Join-Path $driverSrc "QstVHidDev.cer")) {
    Copy-Item (Join-Path $driverSrc "QstVHidDev.cer") $driverDst -Force
}
Copy-Item $pkgSrc (Join-Path $driverDst "package") -Recurse -Force
# Refuse stale flat layout leftovers (old dist used driver\*.sys without qst_vhid\)
$staleFlat = @(
    (Join-Path $DistDir "driver\QstVHid.sys"),
    (Join-Path $DistDir "driver\qst_vhid.inf")
) | Where-Object { Test-Path $_ }
foreach ($f in $staleFlat) { Remove-Item $f -Force }
$verifyDriver = @(
    (Join-Path $driverDst "_elevate_install.ps1"),
    (Join-Path $driverDst "sign_and_install.ps1"),
    (Join-Path $driverDst "import_certs.reg"),
    (Join-Path $driverDst "package\QstVHid.sys"),
    (Join-Path $driverDst "package\qst_vhid.inf"),
    (Join-Path $driverDst "package\qst_vhid.cat")
) | Where-Object { -not (Test-Path $_) }
if ($verifyDriver.Count -gt 0) {
    throw ("FATAL: dist VHID copy incomplete:`n  " + ($verifyDriver -join "`n  "))
}
Write-Host "  + driver/qst_vhid/ (elevate install + package + import_certs.reg)"

$zipPath = Join-Path $DistRoot "QuickScriptTool-Release.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
try {
    Compress-Archive -Path (Join-Path $DistDir "*") -DestinationPath $zipPath -Force
} catch {
    Write-Warning "Zip failed (close QuickScriptTool if running, then re-run). Folder package is ready."
    $zipPath = $null
}

Write-Host ""
Write-Host "Done."
Write-Host "  Folder: $DistDir"
if ($zipPath) { Write-Host "  Zip:    $zipPath" }
$edgeZips = Get-ChildItem $DistRoot -Filter "QstEdgeBridge-*.zip" -ErrorAction SilentlyContinue
foreach ($z in $edgeZips) { Write-Host "  Edge:   $($z.FullName)" }
Write-Host ""
Write-Host "Do NOT ship build\Debug. Ship this Release package instead."
Write-Host "Edge extension MUST be in package: extension\edge (see extension\PACKAGING.md)."
Write-Host "VHID driver is included: driver\qst_vhid\ — target machine needs NO WDK/SDK."
Write-Host "  User flow: open exe -> settings -> install VHID driver -> reboot -> done."
Write-Host "If target PC lacks VCRUNTIME140.dll, install:"
Write-Host "  https://aka.ms/vs/17/release/vc_redist.x64.exe"
Write-Host ""
Write-Host "For installer EXE, compile installer\QuickScriptTool.iss with Inno Setup 6."
