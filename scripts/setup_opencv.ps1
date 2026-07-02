# Download and extract OpenCV prebuilt for Windows (Visual Studio x64)
# Usage: powershell -ExecutionPolicy Bypass -File scripts/setup_opencv.ps1

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ThirdParty = Join-Path $Root "third_party"
$Version = "4.10.0"
$ArchiveName = "opencv-$Version-windows.exe"
$DownloadUrl = "https://github.com/opencv/opencv/releases/download/$Version/$ArchiveName"
$DownloadPath = Join-Path $ThirdParty $ArchiveName
$ExtractDir = Join-Path $ThirdParty "opencv-src"
$TargetDir = Join-Path $ThirdParty "opencv"

New-Item -ItemType Directory -Force -Path $ThirdParty | Out-Null

if (Test-Path (Join-Path $TargetDir "build/OpenCVConfig.cmake")) {
    Write-Host "OpenCV already installed at $TargetDir"
    exit 0
}

Write-Host "Downloading OpenCV $Version ..."
Invoke-WebRequest -Uri $DownloadUrl -OutFile $DownloadPath -UseBasicParsing

Write-Host "Extracting (this may take a minute) ..."
if (Test-Path $ExtractDir) { Remove-Item $ExtractDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $ExtractDir | Out-Null

# OpenCV Windows package is a self-extracting 7z archive
$proc = Start-Process -FilePath $DownloadPath -ArgumentList @("-o$ExtractDir", "-y") -Wait -PassThru
if ($proc.ExitCode -ne 0) {
    throw "OpenCV extractor failed with exit code $($proc.ExitCode)"
}

$InnerRoot = Get-ChildItem $ExtractDir -Directory | Select-Object -First 1
if (-not $InnerRoot) { throw "OpenCV extract folder not found" }

if (Test-Path $TargetDir) { Remove-Item $TargetDir -Recurse -Force }
Move-Item $InnerRoot.FullName $TargetDir

Remove-Item $DownloadPath -Force -ErrorAction SilentlyContinue
Remove-Item $ExtractDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "OpenCV installed to $TargetDir"
Write-Host "Set OpenCV_DIR=$TargetDir\build and reconfigure CMake."
