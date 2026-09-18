# Pack / validate QST Edge companion extension (MV3)
# Usage (repo root):
#   powershell -ExecutionPolicy Bypass -File tools\pack_edge_extension.ps1
#   powershell -ExecutionPolicy Bypass -File tools\pack_edge_extension.ps1 -SkipZip
# Spec: extension\PACKAGING.md

[CmdletBinding()]
param(
    [string]$OutDir = "",
    [switch]$SkipZip
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ExtDir = Join-Path $Root "extension\edge"
if (-not $OutDir) { $OutDir = Join-Path $Root "dist" }

function Fail([string]$msg) {
    throw $msg
}

if (-not (Test-Path -LiteralPath $ExtDir)) {
    Fail "Missing extension dir: $ExtDir"
}

$required = @(
    "manifest.json",
    "background.js",
    "popup.html",
    "popup.js",
    "options.html",
    "options.js",
    "guide.html",
    "offscreen.html",
    "offscreen.js",
    "wake.js",
    "README.txt",
    "icons\icon16.png",
    "icons\icon48.png",
    "icons\icon128.png"
)
foreach ($rel in $required) {
    $p = Join-Path $ExtDir $rel
    if (-not (Test-Path -LiteralPath $p)) {
        Fail "Missing file: extension\edge\$rel"
    }
}

$manifestPath = Join-Path $ExtDir "manifest.json"
$manifestRaw = [System.IO.File]::ReadAllText($manifestPath)
if ($manifestRaw -match '"version"\s*:\s*"[^"]+"\s*;') {
    Fail "manifest.json invalid: version line ends with ';' (must be ',')"
}

try {
    $manifest = $manifestRaw | ConvertFrom-Json
} catch {
    Fail ("manifest.json is not valid JSON: " + $_.Exception.Message)
}

$ver = [string]$manifest.version
if ([string]::IsNullOrWhiteSpace($ver)) {
    Fail "manifest.json missing version"
}
if ([int]$manifest.manifest_version -ne 3) {
    Fail ("manifest_version must be 3, got " + $manifest.manifest_version)
}

$bgPath = Join-Path $ExtDir "background.js"
$bg = [System.IO.File]::ReadAllText($bgPath)
$m = [regex]::Match($bg, 'BRIDGE_VERSION\s*=\s*"([^"]+)"')
if (-not $m.Success) {
    Fail "background.js: BRIDGE_VERSION not found"
}
$bridgeVer = $m.Groups[1].Value
if ($bridgeVer -ne $ver) {
    Fail ("Version mismatch: manifest=$ver background.js=$bridgeVer")
}
$perms = @($manifest.permissions)
if ($perms -notcontains "nativeMessaging") {
    Fail "manifest.json missing nativeMessaging permission (packed CRX needs it for token)"
}

Write-Host ("OK  extension\edge v{0}  files+JSON OK" -f $ver)

if ($SkipZip) {
    # Use return (not exit) so callers like package_release.ps1 keep their variables.
    return
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zipName = "QstEdgeBridge-$ver.zip"
$zipPath = Join-Path $OutDir $zipName
$stage = Join-Path $OutDir ("_edge_pack_stage_" + [guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    # Shared exclusion helper. bridge_runtime.json is host runtime state whose ACL is
    # hardened, so a plain recursive copy can fail the whole pack. See copy_extension_edge.ps1.
    $copyHelper = Join-Path $Root "tools\copy_extension_edge.ps1"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $copyHelper -Source $ExtDir -Dest $stage
    if ($LASTEXITCODE -ne 0) { Fail "extension copy failed (see tools\copy_extension_edge.ps1)" }
    $runtimeJson = Join-Path $stage "bridge_runtime.json"
    if (Test-Path -LiteralPath $runtimeJson) {
        Remove-Item -LiteralPath $runtimeJson -Force
    }
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zipPath -Force
} finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
$zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $rootManifest = $false
    foreach ($e in $zip.Entries) {
        $n = $e.FullName.Replace("\", "/")
        if ($n.StartsWith("/")) { $n = $n.Substring(1) }
        if ($n -eq "manifest.json") {
            $rootManifest = $true
            break
        }
    }
    if (-not $rootManifest) {
        $sample = @($zip.Entries | Select-Object -First 12 | ForEach-Object { $_.FullName }) -join ", "
        Fail ("zip root missing manifest.json. sample entries: " + $sample)
    }
} finally {
    $zip.Dispose()
}

Write-Host ("OK  Zip: {0}" -f $zipPath)
Write-Host ""
Write-Host "Install: unzip, then edge://extensions -> Load unpacked -> folder with manifest.json"
# Use return (not exit) so callers like package_release.ps1 keep their variables.
