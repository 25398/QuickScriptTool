# fetch_webview2_fixed.ps1 — download + expand WebView2 Fixed Version Runtime (not committed to git)
# Usage:
#   powershell -File tools\fetch_webview2_fixed.ps1 [-OutDir path] [-Force]
# Default OutDir: <repo>\third_party\webview2_fixed\<version>\WebView2Fixed

param(
    [string]$OutDir = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) {
    throw "FATAL: PSScriptRoot empty; run via powershell -File"
}
$RepoRoot = Split-Path -Parent $PSScriptRoot
$VersionFile = Join-Path $PSScriptRoot "webview2_fixed_version.txt"
if (-not (Test-Path -LiteralPath $VersionFile)) {
    throw "Missing $VersionFile"
}

$lines = Get-Content -LiteralPath $VersionFile | ForEach-Object { $_.Trim() } |
    Where-Object { $_ -and -not $_.StartsWith("#") }
if ($lines.Count -lt 3) {
    throw "webview2_fixed_version.txt needs: version, arch, url"
}
$Version = $lines[0]
$Arch = $lines[1].ToLowerInvariant()
$Url = $lines[2]
if ($Arch -ne "x64" -and $Arch -ne "x86" -and $Arch -ne "arm64") {
    throw "Unsupported arch: $Arch"
}

$CacheRoot = Join-Path $RepoRoot "third_party\webview2_fixed"
$CabDir = Join-Path $CacheRoot "_cabs"
$ExpandRoot = Join-Path $CacheRoot $Version
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $ExpandRoot "WebView2Fixed"
}

New-Item -ItemType Directory -Force -Path $CabDir | Out-Null
New-Item -ItemType Directory -Force -Path $ExpandRoot | Out-Null

$cabName = "Microsoft.WebView2.FixedVersionRuntime.$Version.$Arch.cab"
$cabPath = Join-Path $CabDir $cabName
$marker = Join-Path $OutDir "msedgewebview2.exe"

function Find-MsEdgeWebView2([string]$root) {
    $direct = Join-Path $root "msedgewebview2.exe"
    if (Test-Path -LiteralPath $direct) { return $direct }
    $hit = Get-ChildItem -LiteralPath $root -Filter "msedgewebview2.exe" -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

if ((-not $Force) -and (Test-Path -LiteralPath $marker)) {
    Write-Host "Fixed Runtime already present: $OutDir"
    exit 0
}

if ($Force -and (Test-Path -LiteralPath $OutDir)) {
    Remove-Item -LiteralPath $OutDir -Recurse -Force
}

if ($Force -or -not (Test-Path -LiteralPath $cabPath)) {
    Write-Host "Downloading Fixed Version cab..."
    Write-Host "  $Url"
    Invoke-WebRequest -Uri $Url -OutFile $cabPath -UseBasicParsing
}

if (-not (Test-Path -LiteralPath $cabPath)) {
    throw "CAB missing after download: $cabPath"
}

$stage = Join-Path $ExpandRoot "_expand_stage"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Write-Host "Expanding cab with expand.exe (do not use Explorer)..."
$expand = Join-Path $env:SystemRoot "System32\expand.exe"
if (-not (Test-Path -LiteralPath $expand)) { throw "expand.exe not found" }
& $expand $cabPath -F:* $stage
if ($LASTEXITCODE -ne 0) { throw "expand.exe failed with $LASTEXITCODE" }

$found = Find-MsEdgeWebView2 $stage
if (-not $found) {
    throw "msedgewebview2.exe not found after expand under $stage"
}

# Cab usually expands to Microsoft.WebView2.FixedVersionRuntime.<ver>.<arch>\...
$runtimeRoot = Split-Path -Parent $found
Write-Host "Runtime folder: $runtimeRoot"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
# Copy tree into WebView2Fixed (flat: contents of runtime root)
& robocopy $runtimeRoot $OutDir /E /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
$rc = $LASTEXITCODE
if ($rc -ge 8) { throw "robocopy failed code=$rc" }

$verify = Find-MsEdgeWebView2 $OutDir
if (-not $verify) { throw "Copy failed; msedgewebview2.exe missing in $OutDir" }

# Win10 unpackaged Fixed Version >=120 needs AppContainer RX on the folder
Write-Host "Granting AppContainer read/execute (Win10 Fixed Version requirement)..."
icacls $OutDir /grant "*S-1-15-2-1:(OI)(CI)(RX)" | Out-Null
icacls $OutDir /grant "*S-1-15-2-2:(OI)(CI)(RX)" | Out-Null

# Cleanup stage (keep cab for cache)
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "OK Fixed Runtime ready:"
Write-Host "  version=$Version arch=$Arch"
Write-Host "  path=$OutDir"
Write-Host "  exe=$verify"
