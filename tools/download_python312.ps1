# Download Python 3.12 installer into tools/ for offline OCR one-click install packaging.
# Usage: powershell -ExecutionPolicy Bypass -File tools\download_python312.ps1

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ToolsDir = Join-Path $Root "tools"
$Version = "3.12.10"
$FileName = "python-$Version-amd64.exe"
$Url = "https://www.python.org/ftp/python/$Version/$FileName"
$Dest = Join-Path $ToolsDir $FileName

Write-Host "Downloading Python $Version installer..."
Write-Host "  URL:  $Url"
Write-Host "  Dest: $Dest"

Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing

Write-Host ""
Write-Host "Done. Re-run tools\package_release.ps1 to include it in the release folder."
