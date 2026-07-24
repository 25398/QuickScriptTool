# Package QuickScriptTool Release build into dist/
# Usage: powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
#
# 必含 Edge 配套扩展：先校验/打 zip（tools\pack_edge_extension.ps1），再拷入 dist。
# 规范见 extension\PACKAGING.md — 漏扩展视为发版失败。

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = Join-Path $Root "build"
$ReleaseDir = Join-Path $BuildDir "Release"
$DistRoot = Join-Path $Root "dist"
$DistDir = Join-Path $DistRoot "QuickScriptTool"
$Cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$PackExt = Join-Path $Root "tools\pack_edge_extension.ps1"

Write-Host "Validating + packing Edge extension..."
& $PackExt -OutDir $DistRoot
if ($LASTEXITCODE -ne 0) { throw "Edge extension pack failed (see extension\PACKAGING.md)." }

Write-Host "Building Release..."
if (-not (Test-Path $Cmake)) {
    throw "CMake not found. Install Visual Studio 2022 with CMake."
}
& $Cmake --build $BuildDir --config Release --target QuickScriptTool -j 16
if ($LASTEXITCODE -ne 0) { throw "Release build failed." }

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

# 始终以仓库源码 extension\edge 为准（避免 build 副本过期/损坏）
$extSrc = Join-Path $Root "extension\edge"
if (-not (Test-Path (Join-Path $extSrc "manifest.json"))) {
    throw "FATAL: extension\edge\manifest.json missing. Cannot ship without Edge bridge."
}
Copy-Item $extSrc (Join-Path $DistDir "extension\edge") -Recurse -Force
Write-Host "  + extension\edge (browser companion, required)"
$manifestCheck = Join-Path $DistDir "extension\edge\manifest.json"
if (-not (Test-Path $manifestCheck)) {
    throw "FATAL: dist copy of extension\edge failed."
}

$pythonInstaller = Join-Path $Root "tools\python-3.12.10-amd64.exe"
if (Test-Path $pythonInstaller) {
    Copy-Item $pythonInstaller (Join-Path $DistDir "tools")
    Write-Host "  + python-3.12.10-amd64.exe (offline OCR install)"
} else {
    Write-Host "  (optional) Run tools\download_python312.ps1 to bundle Python for offline OCR install"
}

$pythonPortable = Join-Path $Root "tools\python312"
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
Write-Host "If target PC lacks VCRUNTIME140.dll, install:"
Write-Host "  https://aka.ms/vs/17/release/vc_redist.x64.exe"
Write-Host ""
Write-Host "For installer EXE, compile installer\QuickScriptTool.iss with Inno Setup 6."
